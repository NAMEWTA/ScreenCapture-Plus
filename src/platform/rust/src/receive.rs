use crate::event::timeout_duration;
use crate::lease;
use crate::status::{self, Error, Result};
use bytes::Bytes;
use localsend::http::server::common::verified_upload::{
    CommitSender, CommittedMedia, UploadFailure, UploadVerification,
    VerificationReceiver, VerifiedUploadReceiver,
};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use tokio::runtime::Handle;
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;

pub type ReceiveReport = Arc<Mutex<HashMap<String, i32>>>;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HuhuVerification {
    pub received_bytes: u64,
    pub sha256: [u8; 32],
    pub sender_checksum_matched: u32,
    pub media_kind: u32,
}

struct Inner {
    chunks: mpsc::Receiver<Bytes>,
    pending: Bytes,
    eof: bool,
    verification: Option<VerificationReceiver>,
    verified: Option<UploadVerification>,
    failed: Option<i32>,
    completion: Option<CommitSender>,
}

pub struct HuhuReceive {
    runtime: Handle,
    global_cancel: CancellationToken,
    session_cancel: CancellationToken,
    file_id: String,
    kind: u32,
    report: ReceiveReport,
    inner: Mutex<Inner>,
}

impl HuhuReceive {
    pub fn new(runtime: Handle, global_cancel: CancellationToken,
        session_cancel: CancellationToken, file_id: String, mime: &str,
        report: ReceiveReport, receiver: VerifiedUploadReceiver) -> Result<Self> {
        Ok(Self { runtime, global_cancel, session_cancel, file_id,
            kind: lease::kind_from_mime(mime)?, report,
            inner: Mutex::new(Inner { chunks: receiver.chunks, pending: Bytes::new(), eof: false,
                verification: Some(receiver.verification), verified: None, failed: None,
                completion: Some(receiver.completion) }) })
    }

    fn record(&self, status: i32) {
        self.report.lock().unwrap_or_else(|error| error.into_inner())
            .insert(self.file_id.clone(), status);
    }

    fn stopped(&self) -> bool {
        self.global_cancel.is_cancelled() || self.session_cancel.is_cancelled()
    }

    pub fn is_cancelled(&self) -> Result<bool> {
        if self.stopped() { return Ok(true); }
        let inner = self.inner.lock().map_err(|_| Error::new(status::PANIC, "Receive lock was poisoned"))?;
        Ok(inner.verified.as_ref().is_some_and(|verification| verification.cancellation.is_cancelled()))
    }

    pub fn read(&self, destination: &mut [u8], timeout_ms: u32) -> Result<usize> {
        let wait = timeout_duration(timeout_ms)?;
        if destination.is_empty() || destination.len() > 512 * 1024 {
            return Err(Error::new(status::INVALID_ARGUMENT, "Receive buffer is outside its byte budget"));
        }
        if self.stopped() { return Err(Error::new(status::CANCELLED, "Receive stopped")); }
        let mut inner = self.inner.lock().map_err(|_| Error::new(status::PANIC, "Receive lock was poisoned"))?;
        if inner.completion.is_none() { return Err(Error::new(status::INVALID_STATE, "Receive was completed")); }
        if inner.eof { return Err(Error::new(status::END, "Receive stream reached EOF")); }
        if inner.pending.is_empty() {
            let chunk = self.runtime.block_on(async {
                tokio::select! {
                    biased;
                    _ = self.global_cancel.cancelled() => Err(status::CANCELLED),
                    _ = self.session_cancel.cancelled() => Err(status::CANCELLED),
                    chunk = tokio::time::timeout(wait, inner.chunks.recv()) => {
                        chunk.map_err(|_| status::TIMEOUT)
                    }
                }
            }).map_err(|code| Error::new(code, "Receive read did not complete"))?;
            match chunk {
                Some(bytes) => inner.pending = bytes,
                None => { inner.eof = true; return Err(Error::new(status::END, "Receive stream reached EOF")); }
            }
        }
        let length = destination.len().min(inner.pending.len());
        if length == 0 { return Err(Error::new(status::IO, "Unexpected empty receive chunk")); }
        destination[..length].copy_from_slice(&inner.pending.split_to(length));
        Ok(length)
    }

    pub fn verify(&self, timeout_ms: u32) -> Result<HuhuVerification> {
        let wait = timeout_duration(timeout_ms)?;
        if self.stopped() { return Err(Error::new(status::CANCELLED, "Receive stopped")); }
        let mut inner = self.inner.lock().map_err(|_| Error::new(status::PANIC, "Receive lock was poisoned"))?;
        if !inner.eof || inner.completion.is_none() {
            return Err(Error::new(status::INVALID_STATE, "Read to EOF before verification"));
        }
        if let Some(code) = inner.failed { return Err(Error::new(code, "Receive verification failed")); }
        if inner.verified.is_none() {
            let future = inner.verification.as_mut()
                .ok_or_else(|| Error::new(status::INVALID_STATE, "Verification was already consumed"))?;
            let received = self.runtime.block_on(async {
                tokio::select! {
                    biased;
                    _ = self.global_cancel.cancelled() => Err(status::CANCELLED),
                    _ = self.session_cancel.cancelled() => Err(status::CANCELLED),
                    result = tokio::time::timeout(wait, future) => result.map_err(|_| status::TIMEOUT),
                }
            }).map_err(|code| Error::new(code, "Receive verification did not complete"))?;
            inner.verification.take();
            match received {
                Ok(Ok(verification)) => inner.verified = Some(verification),
                failed => {
                    let code = match failed {
                        Ok(Err(failure)) => status::failure_status(failure),
                        _ => status::CLOSED,
                    };
                    inner.failed = Some(code);
                    self.record(code);
                    return Err(Error::new(code, "Receive verification failed"));
                }
            }
        }
        let verified = inner.verified.as_ref().ok_or_else(|| Error::new(status::INVALID_STATE, "Missing verification"))?;
        if verified.cancellation.is_cancelled() { return Err(Error::new(status::CANCELLED, "Commit permission was cancelled")); }
        let mut sha256 = [0; 32];
        if !status::digest_is_valid(&verified.actual_sha256) {
            return Err(Error::new(status::PROTOCOL, "Invalid verification digest"));
        }
        for (index, byte) in sha256.iter_mut().enumerate() {
            *byte = u8::from_str_radix(&verified.actual_sha256[index * 2..index * 2 + 2], 16)
                .map_err(|_| Error::new(status::PROTOCOL, "Invalid verification digest"))?;
        }
        Ok(HuhuVerification { received_bytes: verified.received_bytes, sha256,
            sender_checksum_matched: u32::from(verified.sender_checksum_matched), media_kind: self.kind })
    }

    pub fn complete(&self, code: i32, media_kind: u32) -> Result<()> {
        let mut inner = self.inner.lock().map_err(|_| Error::new(status::PANIC, "Receive lock was poisoned"))?;
        if inner.completion.is_none() { return Err(Error::new(status::INVALID_STATE, "Receive was completed")); }
        let completion = if code == status::OK {
            let verified = inner.verified.as_ref()
                .ok_or_else(|| Error::new(status::INVALID_STATE, "Success requires consumed verification"))?;
            if self.stopped() || verified.cancellation.is_cancelled() {
                return Err(Error::new(status::CANCELLED, "Commit permission was cancelled"));
            }
            if media_kind != self.kind {
                return Err(Error::new(status::INVALID_MEDIA, "Committed media kind differs from the offer"));
            }
            Ok(match media_kind {
                1 => CommittedMedia::Png, 2 => CommittedMedia::Gif, 3 => CommittedMedia::Mp4,
                _ => return Err(Error::new(status::INVALID_MEDIA, "Invalid committed media kind")),
            })
        } else { Err(status::status_failure(code)?) };
        inner.chunks.close();
        inner.pending = Bytes::new();
        // Record BEFORE waking the server: its terminal event may be immediate.
        self.record(code);
        let sent = inner.completion.take().ok_or_else(|| Error::new(status::INVALID_STATE, "Missing completion sender"))?
            .send(completion);
        sent.map_err(|_| Error::new(status::CLOSED, "The sender no longer waits for a commit receipt"))
    }
}

impl Drop for HuhuReceive {
    fn drop(&mut self) {
        let inner = self.inner.get_mut().unwrap_or_else(|error| error.into_inner());
        if let Some(completion) = inner.completion.take() {
            inner.chunks.close();
            let code = inner.failed.unwrap_or(status::CANCELLED);
            self.report.lock().unwrap_or_else(|error| error.into_inner()).insert(self.file_id.clone(), code);
            let _ = completion.send(Err(UploadFailure::Cancelled));
        }
    }
}
