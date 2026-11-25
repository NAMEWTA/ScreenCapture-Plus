use crate::config;
use crate::engine::Shared;
use crate::event::{self, HuhuEvent};
use crate::lease::{Lease, LeaseUse};
use crate::status::{self, Error, Result};
use bytes::Bytes;
use futures_util::{FutureExt, StreamExt};
use localsend::crypto::hash::{sha256_file_content, HashError};
use localsend::http::client::{ClientError, LsHttpClientV2};
use localsend::http::dto_v2::PrepareUploadRequestDtoV2;
use localsend::model::discovery::ProtocolType;
use localsend::model::transfer::FileContent;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{Seek, SeekFrom};
use std::panic::AssertUnwindSafe;
use std::sync::{Arc, atomic::{AtomicU64, Ordering}};
use std::time::{Duration, Instant};
use tokio::io::AsyncReadExt;
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Peer {
    pub address: String,
    pub port: u16,
    pub fingerprint: String,
    pub pin: Option<String>,
}

impl Peer {
    fn validate(mut self) -> Result<Self> {
        self.address = config::ipv4(&self.address)?.to_string();
        if self.port == 0 || !status::digest_is_valid(&self.fingerprint)
            || self.pin.as_ref().is_some_and(|pin| !status::text_is_bounded(pin, 128)) {
            return Err(Error::new(status::INVALID_ARGUMENT, "Invalid pinned peer endpoint"));
        }
        self.fingerprint.make_ascii_uppercase();
        Ok(self)
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct FileOutcome {
    id: String,
    file_name: String,
    size: u64,
    accepted: bool,
    status: i32,
}

pub fn send(shared: &Arc<Shared>, peer: Peer, leases: Vec<Arc<Lease>>) -> Result<u64> {
    shared.live()?;
    let peer = peer.validate()?;
    if leases.is_empty() || leases.len() > 64 {
        return Err(Error::new(status::INVALID_ARGUMENT, "Send requires 1..64 media leases"));
    }
    let files = leases.iter().enumerate().map(|(index, lease)|
        lease.acquire(index.to_string(), shared.max_file_bytes)).collect::<Result<Vec<_>>>()?;
    let (operation, cancel, terminal) = shared.admit(true, peer.address.clone())?;
    let state = shared.clone();
    shared.tasks.spawn_on(async move {
        let result = AssertUnwindSafe(run_send(state.clone(), operation, peer, files, cancel)).catch_unwind().await;
        let result = match result {
            Ok(event) => event,
            Err(_) => {
                state.record_panic();
                HuhuEvent::error(event::SEND_END, operation, Error::new(status::PANIC, "LocalSend send worker panicked"))
            }
        };
        state.finish_operation(operation);
        terminal.send(result);
    }, &shared.runtime);
    Ok(operation)
}

async fn run_send(shared: Arc<Shared>, operation: u64, peer: Peer,
    mut files: Vec<LeaseUse>, cancel: CancellationToken) -> HuhuEvent {
    let mut results: Vec<_> = files.iter().map(|file| FileOutcome {
        id: file.dto.id.clone(), file_name: file.dto.file_name.clone(), size: file.dto.size,
        accepted: false, status: status::CANCELLED,
    }).collect();
    let mut remote_session = None;
    let client = LsHttpClientV2::try_new(&shared.identity.private_key_pem, &shared.identity.cert_pem,
        Some(peer.fingerprint.clone()), None);
    let client = match client {
        Ok(client) => client,
        Err(error) => return HuhuEvent::error(event::SEND_END, operation, error.into()),
    };
    let transferred: Result<()> = async {
        for file in &mut files {
            let original = file.file.as_mut().ok_or_else(|| Error::new(status::INVALID_STATE, "Media lease lost its file"))?;
            let expected = file.dto.size;
            let hashed = Arc::new(AtomicU64::new(0));
            let count = hashed.clone();
            let progress_state = shared.clone();
            let file_id = file.dto.id.clone();
            let hash_cancel = cancel.child_token();
            let progress_cancel = hash_cancel.clone();
            let hash = sha256_file_content(FileContent::File(original.try_clone()?), &hash_cancel, move |bytes| {
                count.store(bytes, Ordering::Release);
                if bytes > expected { progress_cancel.cancel(); }
                progress_state.update(HuhuEvent::new(event::SEND_PROGRESS, status::OK, operation,
                    json!({"fileId": file_id, "phase": "hashing", "bytes": bytes, "total": expected})));
            }).await;
            if hashed.load(Ordering::Acquire) > expected {
                return Err(Error::new(status::LENGTH_MISMATCH, "Media grew while it was hashed"));
            }
            file.dto.sha256 = Some(hash.map_err(|error| match error {
                HashError::Cancelled => Error::new(status::CANCELLED, "Hashing cancelled"),
                HashError::Io(error) => error.into(),
            })?);
            if hashed.load(Ordering::Acquire) != expected || original.metadata()?.len() != expected {
                return Err(Error::new(status::LENGTH_MISMATCH, "Media changed while it was hashed"));
            }
            original.seek(SeekFrom::Start(0))?;
        }
        let payload = PrepareUploadRequestDtoV2 { info: shared.info.clone(),
            files: files.iter().map(|file| (file.dto.id.clone(), file.dto.clone())).collect::<HashMap<_, _>>() };
        let response = tokio::time::timeout(shared.decision_timeout,
            client.prepare_upload(ProtocolType::Https, &peer.address, peer.port, None,
                payload, peer.pin.as_deref(), cancel.clone())).await
            .map_err(|_| Error::new(status::IO, "Receiver decision timed out"))??;
        let response = response.response.ok_or_else(|| Error::new(status::CANCELLED, "Receiver accepted no files"))?;
        if response.files.is_empty() { return Err(Error::new(status::CANCELLED, "Receiver accepted no files")); }
        remote_session = Some(response.session_id.clone());
        if let Some(active) = shared.operations.lock().unwrap_or_else(|error| error.into_inner()).get_mut(&operation) {
            active.remote_session = Some(response.session_id.clone());
        }
        for (index, mut file) in files.drain(..).enumerate() {
            let Some(token) = response.files.get(&file.dto.id) else { continue; };
            results[index].accepted = true;
            let file_id = file.dto.id.clone();
            let file_size = file.dto.size;
            let (data_tx, content) = FileContent::result_channel();
            let producer_cancel = cancel.child_token();
            let producer_stop = producer_cancel.clone();
            let producer = tokio::spawn(async move {
                // The LeaseUse stays inside this task until all underlying
                // file reads have really completed, including cancellation.
                let source = file.file.take().ok_or_else(|| Error::new(status::INVALID_STATE, "Media lease lost its file"))?;
                read_media(source, file_size, data_tx, producer_cancel).await
            });
            let progress = Arc::new(Progress { bytes: AtomicU64::new(0), last_ms: AtomicU64::new(0), start: Instant::now() });
            let body_progress = progress.clone();
            let state = shared.clone();
            let progress_file = file_id.clone();
            let mut sent = 0_u64;
            let stream = content.into_stream().map(move |chunk| {
                let chunk = chunk?;
                sent = sent.checked_add(chunk.len() as u64)
                    .ok_or_else(|| std::io::Error::other("Upload byte count overflow"))?;
                body_progress.bytes.store(sent, Ordering::Release);
                body_progress.last_ms.store(body_progress.start.elapsed().as_millis() as u64, Ordering::Release);
                state.update(HuhuEvent::new(event::SEND_PROGRESS, status::OK, operation,
                    json!({"fileId": progress_file, "phase": "sending", "bytes": sent, "total": file_size})));
                Ok::<_, std::io::Error>(chunk)
            });
            let upload = client.upload(ProtocolType::Https, &peer.address, peer.port, None,
                &response.session_id, &file_id, token, localsend::reqwest::Body::wrap_stream(stream), cancel.clone());
            let uploaded = wait_upload(upload, &cancel, &progress, file_size, shared.idle_timeout, shared.commit_timeout).await;
            producer_stop.cancel();
            let produced = producer.await.map_err(|_| Error::new(status::PANIC, "Media reader panicked"))?;
            if produced.as_ref().is_err_and(|error| error.status != status::CANCELLED && error.status != status::CLOSED) {
                let error = produced.err().ok_or_else(|| Error::new(status::INVALID_STATE, "Missing media reader error"))?;
                results[index].status = error.status;
                return Err(error);
            }
            if let Err(error) = uploaded {
                results[index].status = error.status;
                return Err(error);
            }
            // A peer returning HTTP success before the stream was consumed
            // does not turn an incomplete local read into a successful send.
            if let Err(error) = produced {
                results[index].status = error.status;
                return Err(error);
            }
            results[index].status = status::OK;
        }
        Ok(())
    }.await;
    let mut cleanup_error = None;
    if transferred.is_err() {
        if let Some(session) = &remote_session {
            match tokio::time::timeout(Duration::from_secs(5),
                client.cancel(ProtocolType::Https, &peer.address, peer.port, session)).await {
                Ok(Ok(())) => {}
                Ok(Err(error)) => cleanup_error = Some(Error::from(error).message),
                Err(_) => cleanup_error = Some("Remote session cancellation timed out".to_owned()),
            }
        }
    }
    let (code, error, http_status) = match transferred {
        Ok(()) => (if results.iter().all(|file| file.accepted) { status::OK } else { status::PARTIAL }, None, None),
        Err(error) => (error.status, Some(error.message), error.http_status),
    };
    HuhuEvent::new(event::SEND_END, code, operation, json!({
        "files": results, "error": error, "httpStatus": http_status, "cleanupError": cleanup_error
    }))
}

struct Progress { bytes: AtomicU64, last_ms: AtomicU64, start: Instant }

async fn wait_upload(upload: impl std::future::Future<Output = std::result::Result<(), ClientError>>,
    cancel: &CancellationToken, progress: &Progress, size: u64, idle: Duration, commit: Duration) -> Result<()> {
    tokio::pin!(upload);
    loop {
        tokio::select! {
            biased;
            _ = cancel.cancelled() => return Err(Error::new(status::CANCELLED, "Upload cancelled")),
            result = &mut upload => return result.map_err(Into::into),
            _ = tokio::time::sleep(Duration::from_millis(100)) => {
                let elapsed = progress.start.elapsed().as_millis() as u64;
                let since_progress = Duration::from_millis(elapsed.saturating_sub(progress.last_ms.load(Ordering::Acquire)));
                let budget = if progress.bytes.load(Ordering::Acquire) == size { commit } else { idle };
                if since_progress >= budget { return Err(Error::new(status::IO, "Upload or commit acknowledgement timed out")); }
            }
        }
    }
}

async fn read_media(source: std::fs::File, expected: u64,
    sender: mpsc::Sender<std::result::Result<Bytes, std::io::Error>>, cancel: CancellationToken) -> Result<()> {
    let mut source = tokio::fs::File::from_std(source);
    let mut buffer = vec![0; 512 * 1024];
    let read: Result<()> = async {
        let mut count = 0_u64;
        loop {
            let length = tokio::select! {
                biased;
                _ = cancel.cancelled() => return Err(Error::new(status::CANCELLED, "Media reader cancelled")),
                read = source.read(&mut buffer) => read?,
            };
            if length == 0 {
                if count != expected { return Err(Error::new(status::LENGTH_MISMATCH, "Media ended before its announced length")); }
                return Ok(());
            }
            count = count.checked_add(length as u64).filter(|count| *count <= expected)
                .ok_or_else(|| Error::new(status::LENGTH_MISMATCH, "Media grew during upload"))?;
            tokio::select! {
                biased;
                _ = cancel.cancelled() => return Err(Error::new(status::CANCELLED, "Media reader cancelled")),
                sent = sender.send(Ok(Bytes::copy_from_slice(&buffer[..length]))) =>
                    sent.map_err(|_| Error::new(status::CLOSED, "Upload stopped consuming the media stream"))?,
            }
        }
    }.await;
    if let Err(error) = &read {
        let failed = Err(std::io::Error::other(error.message.clone()));
        tokio::select! {
            biased;
            _ = cancel.cancelled() => {},
            _ = sender.send(failed) => {},
        }
    }
    drop(sender);
    // Tokio's conversion waits for an already submitted blocking read. Merely
    // dropping the async File could let that read race a later lease reuse.
    drop(source.into_std().await);
    read
}

pub fn discover(shared: &Arc<Shared>, address: &str, port: u16) -> Result<u64> {
    let address = config::ipv4(address)?.to_string();
    if port == 0 { return Err(Error::new(status::INVALID_ARGUMENT, "Discovery port is zero")); }
    let (operation, cancel, terminal) = shared.admit(false, address.clone())?;
    let state = shared.clone();
    shared.tasks.spawn_on(async move {
        let result = AssertUnwindSafe(async {
            tokio::select! {
                biased;
                _ = cancel.cancelled() => Err(Error::new(status::CANCELLED, "Discovery cancelled")),
                result = probe(&state, &address, port) => result,
            }
        }).catch_unwind().await;
        let event = match result {
            Ok(Ok(device)) => HuhuEvent::new(event::DISCOVERY_END, status::OK, operation, device),
            Ok(Err(error)) => HuhuEvent::error(event::DISCOVERY_END, operation, error),
            Err(_) => {
                state.record_panic();
                HuhuEvent::error(event::DISCOVERY_END, operation, Error::new(status::PANIC, "Discovery worker panicked"))
            }
        };
        state.finish_operation(operation);
        terminal.send(event);
    }, &shared.runtime);
    Ok(operation)
}

async fn probe(shared: &Shared, address: &str, port: u16) -> Result<Value> {
    if let Some(discovery) = &shared.discovery {
        let device = discovery.discover(address, port, ProtocolType::Https).await?
            .ok_or_else(|| Error::new(status::INVALID_STATE, "Discovery resolved this device itself"))?;
        return Ok(json!({"address": address, "port": port, "alias": device.device.alias,
            "fingerprint": device.device.fingerprint, "version": device.device.version}));
    }
    let client = LsHttpClientV2::try_new(&shared.identity.private_key_pem, &shared.identity.cert_pem,
        None, Some(Duration::from_secs(2)))?;
    let response = client.register(ProtocolType::Https, address, port, shared.info.clone()).await?;
    let fingerprint = response.cert_fingerprint
        .ok_or_else(|| Error::new(status::PROTOCOL, "TLS discovery omitted the verified peer fingerprint"))?;
    if fingerprint == shared.info.fingerprint {
        return Err(Error::new(status::INVALID_STATE, "Discovery resolved this device itself"));
    }
    Ok(json!({"address": address, "port": port, "alias": response.body.alias,
        "fingerprint": fingerprint, "version": response.body.version}))
}
