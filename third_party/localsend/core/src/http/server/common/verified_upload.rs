//! Receive/verify/commit handshake used by the native archive adapter.
//! EOF is not permission to publish. Only a successful `verification` followed
//! by complete-media validation and a durable, no-replace archive commit may
//! produce `CommittedMedia`. UI tasks must not own these channels.

use crate::http::limits::{DATA_QUEUE_CHUNKS, TransferLimits};
use crate::http::server::common::save::{FileUploadTarget, SaveResult};
use bytes::Bytes;
use http_body_util::BodyExt;
use hyper::body::Incoming;
use hyper::Request;
use sha2::{Digest, Sha256};
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, atomic::{AtomicBool, Ordering}};
use std::task::{Context, Poll};
use tokio::sync::{mpsc, oneshot};
use tokio_util::sync::CancellationToken;

#[derive(Debug, Clone)]
pub struct UploadVerification {
    pub received_bytes: u64,
    pub actual_sha256: String,
    /// False for an interoperating peer which omitted the optional digest.
    pub sender_checksum_matched: bool,
    /// Recheck immediately before publication. A dropped request, stopped
    /// listener or cancelled session invalidates the commit permission.
    pub cancellation: CancellationToken,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CommittedMedia {
    Png,
    Gif,
    Mp4,
}

impl CommittedMedia {
    pub fn mime(self) -> &'static str {
        match self { Self::Png => "image/png", Self::Gif => "image/gif", Self::Mp4 => "video/mp4" }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UploadFailure {
    Cancelled,
    Io,
    LengthMismatch,
    ChecksumMismatch,
    InvalidMedia,
    /// Complete decoding would exceed the application's explicit media budget.
    LimitExceeded,
    /// A final file exists, but its persistence could not be confirmed. Keep
    /// it and report uncertainty; never acknowledge durable success or retry
    /// the commit into another archive entry.
    DurabilityUnknown,
}

/// Service-side half. Consume bounded chunks into an exclusively owned staging
/// file. After EOF, await verification; an Err or a dropped sender aborts it.
/// Then validate the full media with the platform's bounded codec/probe, flush,
/// close, commit without replacement, and send the actual completion result.
pub struct VerifiedUploadReceiver {
    pub chunks: mpsc::Receiver<Bytes>,
    pub verification: VerificationReceiver,
    pub completion: CommitSender,
}

/// Opaque server half. A caller cannot construct a raw completion channel and
/// bypass the verification-consumption check enforced by `CommitSender`.
#[derive(Debug)]
pub struct VerifiedUploadTarget {
    pub(crate) binary_tx: mpsc::Sender<Bytes>,
    pub(crate) verification_tx: oneshot::Sender<Result<UploadVerification, UploadFailure>>,
    pub(crate) commit_rx: oneshot::Receiver<Result<CommittedMedia, UploadFailure>>,
}

/// Reading a successful permission, rather than merely queuing it, enables the
/// completion sender. Keeping these as separate channels prevents EOF/ready
/// scheduling from accidentally acknowledging an unvalidated archive.
pub struct VerificationReceiver {
    receiver: oneshot::Receiver<Result<UploadVerification, UploadFailure>>,
    consumed: Arc<AtomicBool>,
}

impl Future for VerificationReceiver {
    type Output = Result<Result<UploadVerification, UploadFailure>, oneshot::error::RecvError>;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let result = Pin::new(&mut self.receiver).poll(context);
        if matches!(&result, Poll::Ready(Ok(Ok(_)))) {
            self.consumed.store(true, Ordering::Release);
        }
        result
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompletionError { BeforeVerification, ReceiverDropped }

pub struct CommitSender {
    sender: oneshot::Sender<Result<CommittedMedia, UploadFailure>>,
    consumed: Arc<AtomicBool>,
}

impl CommitSender {
    /// Only an actually consumed successful verification may be followed by a
    /// successful commit receipt. Invalid early success produces a failure at
    /// the HTTP boundary regardless of how the two tasks were scheduled.
    pub fn send(self, result: Result<CommittedMedia, UploadFailure>) -> Result<(), CompletionError> {
        if result.is_ok() && !self.consumed.load(Ordering::Acquire) {
            let _ = self.sender.send(Err(UploadFailure::Io));
            return Err(CompletionError::BeforeVerification);
        }
        self.sender.send(result).map_err(|_| CompletionError::ReceiverDropped)
    }
}

pub fn verified_upload_channel() -> (FileUploadTarget, VerifiedUploadReceiver) {
    let (binary_tx, chunks) = mpsc::channel(DATA_QUEUE_CHUNKS);
    let (verification_tx, receiver) = oneshot::channel();
    let (sender, commit_rx) = oneshot::channel();
    let consumed = Arc::new(AtomicBool::new(false));
    let verification = VerificationReceiver { receiver, consumed: consumed.clone() };
    let completion = CommitSender { sender, consumed };
    (FileUploadTarget::VerifiedStream(VerifiedUploadTarget { binary_tx, verification_tx, commit_rx }),
        VerifiedUploadReceiver { chunks, verification, completion })
}

pub(crate) async fn receive_verified(
    req: Request<Incoming>,
    binary_tx: mpsc::Sender<Bytes>,
    verification_tx: oneshot::Sender<Result<UploadVerification, UploadFailure>>,
    mut commit_rx: oneshot::Receiver<Result<CommittedMedia, UploadFailure>>,
    file_size: u64,
    expected_sha256: Option<&str>,
    mime: &str,
    limits: &TransferLimits,
    cancel: &CancellationToken,
) -> SaveResult {
    const PREFIX_BYTES: usize = 64 * 1024;
    if binary_tx.max_capacity() > DATA_QUEUE_CHUNKS || file_size > limits.max_file_bytes {
        let _ = verification_tx.send(Err(UploadFailure::Io));
        return SaveResult::Failed;
    }
    let mut body = req.into_body();
    let mut hasher = Sha256::new();
    let mut prefix = Vec::with_capacity(PREFIX_BYTES);
    let mut received = 0_u64;
    let receive = async {
        loop {
            let frame = tokio::time::timeout(limits.idle_timeout, body.frame()).await
                .map_err(|_| UploadFailure::Io)?;
            let Some(frame) = frame else { break; };
            let frame = frame.map_err(|_| UploadFailure::Io)?;
            let Ok(data) = frame.into_data() else { continue; };
            received = received.checked_add(data.len() as u64)
                .filter(|count| *count <= file_size && *count <= limits.max_file_bytes)
                .ok_or(UploadFailure::LengthMismatch)?;
            hasher.update(&data);
            let take = data.len().min(PREFIX_BYTES - prefix.len());
            prefix.extend_from_slice(&data[..take]);
            // Byte bounds matter, not just the number of channel entries. A
            // huge HTTP frame must not become a huge FFI/event queue element.
            for chunk in data.chunks(crate::http::limits::DATA_CHUNK_BYTES) {
                tokio::time::timeout(limits.idle_timeout, binary_tx.send(Bytes::copy_from_slice(chunk)))
                    .await.map_err(|_| UploadFailure::Io)?
                    .map_err(|_| UploadFailure::Io)?;
            }
        }
        if received != file_size { return Err(UploadFailure::LengthMismatch); }
        let actual = crate::crypto::hash::to_hex(&hasher.finalize());
        if expected_sha256.is_some_and(|expected| !actual.eq_ignore_ascii_case(expected)) {
            return Err(UploadFailure::ChecksumMismatch);
        }
        // Reuse infer's bounded magic-number detection. This rejects a type
        // spoof before the commit permission; it is not a decoder/completeness
        // guarantee. Full Qt/FFmpeg validation remains mandatory in the sink.
        if infer::get(&prefix).is_none_or(|kind| kind.mime_type() != mime) {
            return Err(UploadFailure::InvalidMedia);
        }
        Ok(UploadVerification { received_bytes: received, actual_sha256: actual,
            sender_checksum_matched: expected_sha256.is_some(), cancellation: cancel.clone() })
    };
    let verification = tokio::select! {
        biased;
        _ = cancel.cancelled() => Err(UploadFailure::Cancelled),
        result = receive => result,
    };
    drop(binary_tx);
    // A consumer cannot claim persistence before receiving permission. This
    // catches accidentally acknowledging EOF as success in an archive adapter.
    if verification.is_ok()
        && !matches!(commit_rx.try_recv(), Err(oneshot::error::TryRecvError::Empty)) {
        let _ = verification_tx.send(Err(UploadFailure::Io));
        return SaveResult::Failed;
    }
    let failure = verification.as_ref().err().copied();
    if verification_tx.send(verification).is_err() { return SaveResult::Failed; }
    if let Some(failure) = failure { return save_result(failure); }

    // This is the only successful ACK path. Merely consuming the last chunk or
    // passing a checksum never acknowledges that an archive was persisted.
    let committed = tokio::select! {
        biased;
        _ = cancel.cancelled() => return SaveResult::Cancelled,
        result = tokio::time::timeout(limits.commit_timeout, commit_rx) => result,
    };
    match committed {
        Ok(Ok(Ok(media))) if media.mime() == mime => SaveResult::Success,
        Ok(Ok(Ok(_))) => SaveResult::InvalidMedia,
        Ok(Ok(Err(failure))) => save_result(failure),
        _ => SaveResult::Failed,
    }
}

fn save_result(failure: UploadFailure) -> SaveResult {
    match failure {
        UploadFailure::ChecksumMismatch => SaveResult::HashMismatch,
        UploadFailure::LengthMismatch => SaveResult::LengthMismatch,
        UploadFailure::InvalidMedia => SaveResult::InvalidMedia,
        UploadFailure::LimitExceeded => SaveResult::LimitExceeded,
        UploadFailure::Cancelled => SaveResult::Cancelled,
        _ => SaveResult::Failed,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn early_success_fails_before_and_after_permission_is_queued() {
        for permission_already_queued in [false, true] {
            let (target, receiver) = verified_upload_channel();
            let FileUploadTarget::VerifiedStream(mut target) = target else { unreachable!(); };
            if permission_already_queued {
                target.verification_tx.send(Ok(UploadVerification {
                    received_bytes: 1, actual_sha256: "0".repeat(64),
                    sender_checksum_matched: true, cancellation: CancellationToken::new(),
                })).unwrap();
            }
            assert_eq!(receiver.completion.send(Ok(CommittedMedia::Png)),
                Err(CompletionError::BeforeVerification));
            assert_eq!(target.commit_rx.try_recv().unwrap(), Err(UploadFailure::Io));
        }
    }
}
