#![cfg(feature = "http")]

//! Actual loopback TLS and filesystem tests for the native receive boundary.
//! The archive consumer accepts one complete known PNG fixture, not arbitrary
//! media. Complete Qt/FFmpeg decoding belongs to the C++ integration tests.

use bytes::Bytes;
use localsend::crypto::cert::{generate_self_signed, SelfSignedCert};
use localsend::crypto::hash::sha256_hex;
use localsend::http::client::{ClientError, LsHttpClientV2};
use localsend::http::dto_v2::{PrepareUploadRequestDtoV2, PrepareUploadResponseDtoV2, RegisterDtoV2};
use localsend::http::limits::TransferLimits;
use localsend::http::server::common::verified_upload::{
    verified_upload_channel, CommittedMedia, UploadFailure, UploadVerification,
    VerifiedUploadReceiver,
};
use localsend::http::server::v2::{PrepareUploadDecisionV2, ServerEventV2};
use localsend::http::server::{start_bound_v2, ServerConfigV2, ServerHandle, TlsConfig};
use localsend::http::state::ClientInfo;
use localsend::model::discovery::ProtocolType;
use localsend::model::transfer::{FileContent, FileDto};
use std::collections::HashMap;
use std::net::Ipv4Addr;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;
use tokio::io::AsyncWriteExt;
use tokio::sync::{mpsc, oneshot, Semaphore};
use tokio_util::sync::CancellationToken;

const PNG: &[u8] = include_bytes!("fixtures/one-pixel.png");
const WAIT: Duration = Duration::from_secs(10);

#[derive(Clone)]
enum SinkAction {
    Commit,
    Hold(Arc<Semaphore>),
    FailIo,
    DurabilityUnknown,
    EarlyCompletion,
}

#[derive(Debug)]
enum Observation {
    Started,
    Verified(UploadVerification),
    Rejected(UploadFailure),
}

struct Receiver {
    handle: ServerHandle,
    identity: SelfSignedCert,
    sender: SelfSignedCert,
    root: PathBuf,
    observations: mpsc::Receiver<Observation>,
    stop: Option<oneshot::Sender<()>>,
    application: Option<tokio::task::JoinHandle<()>>,
}

impl Receiver {
    async fn start(action: SinkAction, limits: TransferLimits) -> Self {
        let identity = generate_self_signed().unwrap();
        let sender = generate_self_signed().unwrap();
        let root = std::env::temp_dir().join(format!("huhucapture-lan-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&root).unwrap();
        let (events, mut events_rx) = mpsc::channel(16);
        let (observations_tx, observations) = mpsc::channel(8);
        let application = tokio::spawn({
            let root = root.clone();
            async move {
                let mut workers = tokio::task::JoinSet::new();
                while let Some(event) = events_rx.recv().await {
                    match event {
                        ServerEventV2::PrepareUpload { files, decision_tx, .. } => {
                            let _ = decision_tx.send(PrepareUploadDecisionV2::Accept(files.into_keys().collect()));
                        }
                        ServerEventV2::FileUpload { target_tx, .. } => {
                            let (target, receiver) = verified_upload_channel();
                            target_tx.send(target).unwrap();
                            workers.spawn(archive_fixture(receiver, root.clone(), action.clone(), observations_tx.clone()));
                        }
                        _ => {}
                    }
                    while workers.try_join_next().is_some() {}
                }
                workers.shutdown().await;
            }
        });
        let (stop, stop_rx) = oneshot::channel();
        let handle = start_bound_v2(vec![Ipv4Addr::LOCALHOST], 0,
            TlsConfig { cert: identity.certificate_pem.clone(), private_key: identity.private_key_pem.clone() },
            ClientInfo { alias: "huhucapture test".into(), version: "2.2".into(),
                device_model: None, device_type: None, token: identity.fingerprint.clone() },
            ServerConfigV2 { pin: None, verify_checksums: true, event_tx: events }, limits, stop_rx)
            .await.unwrap();
        Self { handle, identity, sender, root, observations, stop: Some(stop), application: Some(application) }
    }

    fn client(&self) -> LsHttpClientV2 {
        LsHttpClientV2::try_new(&self.sender.private_key_pem, &self.sender.certificate_pem,
            Some(self.identity.fingerprint.clone()), None).unwrap()
    }

    fn raw_client(&self) -> localsend::reqwest::Client {
        let pem = format!("{}{}", self.sender.certificate_pem, self.sender.private_key_pem);
        // These parser/route tests already know the endpoint. Authentication
        // tests use the upstream pinned client and its existing TLS test suite.
        localsend::reqwest::Client::builder().use_rustls_tls()
            .identity(localsend::reqwest::Identity::from_pem(pem.as_bytes()).unwrap())
            .danger_accept_invalid_certs(true).no_proxy()
            .redirect(localsend::reqwest::redirect::Policy::none())
            .timeout(WAIT).build().unwrap()
    }

    fn payload(&self, file: FileDto) -> PrepareUploadRequestDtoV2 {
        PrepareUploadRequestDtoV2 {
            info: RegisterDtoV2 { alias: "sender".into(), version: "2.2".into(),
                device_model: None, device_type: None, fingerprint: self.sender.fingerprint.clone(),
                port: 53317, protocol: ProtocolType::Https, download: false },
            files: HashMap::from([(file.id.clone(), file)]),
        }
    }

    async fn prepare(&self, file: FileDto) -> PrepareUploadResponseDtoV2 {
        tokio::time::timeout(WAIT, self.client().prepare_upload(ProtocolType::Https,
            "127.0.0.1", self.handle.port(), None, self.payload(file), None,
            CancellationToken::new())).await.unwrap().unwrap().response.unwrap()
    }

    fn upload(&self, session: PrepareUploadResponseDtoV2, body: localsend::reqwest::Body)
        -> tokio::task::JoinHandle<Result<(), ClientError>> {
        let client = self.client();
        let port = self.handle.port();
        tokio::spawn(async move {
            tokio::time::timeout(WAIT, client.upload(ProtocolType::Https, "127.0.0.1", port,
                None, &session.session_id, "file", &session.files["file"], body,
                CancellationToken::new())).await.unwrap()
        })
    }

    async fn next(&mut self) -> Observation {
        tokio::time::timeout(WAIT, self.observations.recv()).await.unwrap().unwrap()
    }

    async fn shutdown(&mut self) {
        if let Some(stop) = self.stop.take() { let _ = stop.send(()); }
        tokio::time::timeout(WAIT, self.handle.wait_stopped()).await.unwrap();
        if let Some(application) = self.application.take() { application.abort(); let _ = application.await; }
    }
}

impl Drop for Receiver {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() { let _ = stop.send(()); }
        if let Some(application) = self.application.take() { application.abort(); }
        for name in ["staging.part", "committed.png", "lease.png", "moved.png"] {
            let _ = std::fs::remove_file(self.root.join(name));
        }
        let _ = std::fs::remove_dir(&self.root);
    }
}

fn media() -> FileDto {
    FileDto { id: "file".into(), file_name: "截图.png".into(), size: PNG.len() as u64,
        file_type: "image/png".into(), sha256: Some(sha256_hex(PNG)), preview: None, metadata: None }
}

async fn archive_fixture(mut receiver: VerifiedUploadReceiver, root: PathBuf,
    action: SinkAction, observations: mpsc::Sender<Observation>) {
    observations.send(Observation::Started).await.unwrap();
    if matches!(action, SinkAction::EarlyCompletion) {
        receiver.completion.send(Ok(CommittedMedia::Png)).unwrap();
        while receiver.chunks.recv().await.is_some() {}
        if let Ok(Err(error)) = receiver.verification.await {
            observations.send(Observation::Rejected(error)).await.unwrap();
        }
        return;
    }
    let staging = root.join("staging.part");
    let mut output = tokio::fs::OpenOptions::new().write(true).create_new(true).open(&staging).await.unwrap();
    while let Some(chunk) = receiver.chunks.recv().await {
        output.write_all(&chunk).await.unwrap();
    }
    output.flush().await.unwrap();
    let verification = match receiver.verification.await {
        Ok(Ok(verification)) => verification,
        result => {
            drop(output);
            let _ = tokio::fs::remove_file(&staging).await;
            let error = result.ok().and_then(Result::err).unwrap_or(UploadFailure::Cancelled);
            observations.send(Observation::Rejected(error)).await.unwrap();
            return;
        }
    };
    observations.send(Observation::Verified(verification.clone())).await.unwrap();
    if let SinkAction::Hold(gate) = &action { gate.acquire().await.unwrap().forget(); }
    let result = if verification.cancellation.is_cancelled() {
        Err(UploadFailure::Cancelled)
    } else if tokio::fs::read(&staging).await.unwrap() != PNG {
        // A fixture-specific complete-file check. This is intentionally not a
        // replacement for the production codec validator.
        Err(UploadFailure::InvalidMedia)
    } else if matches!(action, SinkAction::FailIo) {
        Err(UploadFailure::Io)
    } else {
        output.sync_all().await.unwrap();
        drop(output);
        let result = tokio::task::spawn_blocking({
            let staging = staging.clone();
            let destination = root.join("committed.png");
            move || durable_commit(&staging, &destination)
        }).await.unwrap();
        let result = result.map(|()| CommittedMedia::Png);
        let result = if result.is_ok() && matches!(action, SinkAction::DurabilityUnknown) {
            // Explicit post-publication failure injection: preserve the actual
            // file and ensure no success ACK can be produced for this state.
            Err(UploadFailure::DurabilityUnknown)
        } else { result };
        let _ = receiver.completion.send(result);
        return;
    };
    drop(output);
    let _ = tokio::fs::remove_file(staging).await;
    let _ = receiver.completion.send(result);
}

#[cfg(unix)]
fn durable_commit(source: &Path, destination: &Path) -> Result<(), UploadFailure> {
    std::fs::hard_link(source, destination).map_err(|_| UploadFailure::Io)?;
    let directory = std::fs::File::open(destination.parent().unwrap()).map_err(|_| UploadFailure::DurabilityUnknown)?;
    directory.sync_all().map_err(|_| UploadFailure::DurabilityUnknown)?;
    std::fs::remove_file(source).map_err(|_| UploadFailure::DurabilityUnknown)?;
    directory.sync_all().map_err(|_| UploadFailure::DurabilityUnknown)
}

#[cfg(windows)]
fn durable_commit(source: &Path, destination: &Path) -> Result<(), UploadFailure> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" { fn MoveFileExW(source: *const u16, destination: *const u16, flags: u32) -> i32; }
    let source: Vec<u16> = source.as_os_str().encode_wide().chain(Some(0)).collect();
    let destination: Vec<u16> = destination.as_os_str().encode_wide().chain(Some(0)).collect();
    // The exclusively owned staging file was synced and closed above. No
    // REPLACE_EXISTING or COPY_ALLOWED flags; same-directory, write-through move.
    if unsafe { MoveFileExW(source.as_ptr(), destination.as_ptr(), 8) } == 0 {
        Err(UploadFailure::Io)
    } else { Ok(()) }
}

fn expect_status(result: Result<(), ClientError>, status: u16) {
    assert!(matches!(&result, Err(ClientError::StatusCode(error)) if error.status == status),
        "expected status {status}, got {result:?}");
}

#[tokio::test]
async fn tls_ack_waits_for_verification_and_durable_commit() {
    let gate = Arc::new(Semaphore::new(0));
    let mut receiver = Receiver::start(SinkAction::Hold(gate.clone()), TransferLimits::default()).await;
    let session = receiver.prepare(media()).await;
    let upload = receiver.upload(session, PNG.to_vec().into());
    assert!(matches!(receiver.next().await, Observation::Started));
    let Observation::Verified(verified) = receiver.next().await else { panic!("not verified"); };
    assert!(verified.sender_checksum_matched);
    assert_eq!(verified.actual_sha256, sha256_hex(PNG));
    assert_eq!(verified.received_bytes, PNG.len() as u64);
    assert!(!upload.is_finished());
    assert!(!receiver.root.join("committed.png").exists());
    gate.add_permits(1);
    upload.await.unwrap().unwrap();
    assert_eq!(std::fs::read(receiver.root.join("committed.png")).unwrap(), PNG);
    assert!(!receiver.root.join("staging.part").exists());
    let port = receiver.handle.port();
    receiver.shutdown().await;
    let listener = tokio::net::TcpListener::bind((Ipv4Addr::LOCALHOST, port)).await.unwrap();
    drop(listener);
}

#[tokio::test]
async fn missing_optional_digest_is_not_claimed_as_sender_verification() {
    let mut receiver = Receiver::start(SinkAction::Commit, TransferLimits::default()).await;
    let mut file = media();
    file.sha256 = None;
    let session = receiver.prepare(file).await;
    let upload = receiver.upload(session, PNG.to_vec().into());
    assert!(matches!(receiver.next().await, Observation::Started));
    let Observation::Verified(verified) = receiver.next().await else { panic!("not verified"); };
    assert!(!verified.sender_checksum_matched);
    assert_eq!(verified.actual_sha256, sha256_hex(PNG));
    upload.await.unwrap().unwrap();
    receiver.shutdown().await;
}

#[tokio::test]
async fn bad_checksum_length_or_type_never_publishes() {
    for scenario in 0..4 {
        let mut receiver = Receiver::start(SinkAction::Commit, TransferLimits::default()).await;
        let mut file = media();
        let mut data = PNG.to_vec();
        let (status, failure) = match scenario {
            0 => { file.sha256 = Some("0".repeat(64)); (422, UploadFailure::ChecksumMismatch) }
            1 => { file.size += 1; (400, UploadFailure::LengthMismatch) }
            2 => { file.size -= 1; (400, UploadFailure::LengthMismatch) }
            _ => { data[0] = 0; file.sha256 = Some(sha256_hex(&data)); (415, UploadFailure::InvalidMedia) }
        };
        let session = receiver.prepare(file).await;
        // Unknown body length exercises cumulative bounds rather than merely
        // mismatching Content-Length headers.
        let body = localsend::reqwest::Body::wrap_stream(futures_util::stream::iter(
            vec![Ok::<_, std::io::Error>(Bytes::from(data))]));
        let upload = receiver.upload(session, body);
        assert!(matches!(receiver.next().await, Observation::Started));
        let Observation::Rejected(error) = receiver.next().await else { panic!("invalid body verified"); };
        assert_eq!(error, failure);
        expect_status(upload.await.unwrap(), status);
        assert!(!receiver.root.join("committed.png").exists());
        receiver.shutdown().await;
    }
}

#[tokio::test]
async fn commit_failures_and_premature_receipts_do_not_ack() {
    for action in [SinkAction::FailIo, SinkAction::DurabilityUnknown, SinkAction::EarlyCompletion] {
        let keep_destination = matches!(action, SinkAction::DurabilityUnknown);
        let mut receiver = Receiver::start(action, TransferLimits::default()).await;
        let session = receiver.prepare(media()).await;
        expect_status(receiver.upload(session, PNG.to_vec().into()).await.unwrap(), 500);
        assert_eq!(receiver.root.join("committed.png").exists(), keep_destination);
        if keep_destination { assert_eq!(std::fs::read(receiver.root.join("committed.png")).unwrap(), PNG); }
        receiver.shutdown().await;
    }
}

#[tokio::test]
async fn an_existing_destination_is_never_replaced() {
    let mut receiver = Receiver::start(SinkAction::Commit, TransferLimits::default()).await;
    let destination = receiver.root.join("committed.png");
    std::fs::write(&destination, b"existing media").unwrap();
    let session = receiver.prepare(media()).await;
    expect_status(receiver.upload(session, PNG.to_vec().into()).await.unwrap(), 500);
    assert_eq!(std::fs::read(destination).unwrap(), b"existing media");
    assert_eq!(std::fs::read(receiver.root.join("staging.part")).unwrap(), PNG);
    receiver.shutdown().await;
}

#[tokio::test]
async fn reading_failure_is_distinct_from_eof_and_cancellation_interrupts_body() {
    let mut receiver = Receiver::start(SinkAction::Commit, TransferLimits::default()).await;
    let session = receiver.prepare(media()).await;
    let (source, content) = FileContent::result_channel();
    source.send(Ok(Bytes::copy_from_slice(&PNG[..8]))).await.unwrap();
    source.send(Err(std::io::Error::other("injected source read failure"))).await.unwrap();
    drop(source);
    let result = receiver.upload(session, localsend::reqwest::Body::wrap_stream(content.into_stream())).await.unwrap();
    assert!(result.is_err());
    assert!(!receiver.root.join("committed.png").exists());
    receiver.shutdown().await;

    let mut receiver = Receiver::start(SinkAction::Commit, TransferLimits::default()).await;
    let session = receiver.prepare(media()).await;
    let session_id = session.session_id.clone();
    let (source, content) = FileContent::result_channel();
    source.send(Ok(Bytes::copy_from_slice(&PNG[..8]))).await.unwrap();
    let upload = receiver.upload(session, localsend::reqwest::Body::wrap_stream(content.into_stream()));
    assert!(matches!(receiver.next().await, Observation::Started));
    assert!(receiver.handle.cancel_v2_session(&session_id).await);
    assert!(upload.await.unwrap().is_err());
    assert!(matches!(receiver.next().await, Observation::Rejected(UploadFailure::Cancelled)));
    assert!(!receiver.root.join("committed.png").exists());
    drop(source);
    receiver.shutdown().await;
}

#[tokio::test]
async fn an_open_media_lease_survives_source_path_replacement() {
    let mut receiver = Receiver::start(SinkAction::Commit, TransferLimits::default()).await;
    let path = receiver.root.join("lease.png");
    std::fs::write(&path, PNG).unwrap();
    let lease = std::fs::File::open(&path).unwrap();
    std::fs::rename(&path, receiver.root.join("moved.png")).unwrap();
    std::fs::write(&path, b"replacement must not be sent").unwrap();
    let session = receiver.prepare(media()).await;
    let body = localsend::reqwest::Body::wrap_stream(FileContent::File(lease).into_stream());
    receiver.upload(session, body).await.unwrap().unwrap();
    assert_eq!(std::fs::read(receiver.root.join("committed.png")).unwrap(), PNG);
    assert_eq!(std::fs::read(&path).unwrap(), b"replacement must not be sent");
    receiver.shutdown().await;
}

#[tokio::test]
async fn selected_listener_routes_and_control_bodies_are_bounded() {
    let mut limits = TransferLimits::default();
    limits.control_bytes = 1024;
    let mut receiver = Receiver::start(SinkAction::Commit, limits).await;
    assert_eq!(receiver.handle.local_addresses(), vec![(Ipv4Addr::LOCALHOST, receiver.handle.port()).into()]);
    let raw = receiver.raw_client();
    let base = format!("https://127.0.0.1:{}", receiver.handle.port());
    for path in ["/", "/i18n.json", "/api/localsend/v1/info", "/api/localsend/v2/show",
        "/api/localsend/v2/prepare-download", "/api/localsend/v2/download",
        "/api/localsend/v3/nonce", "/api/localsend/v3/register"] {
        for method in [localsend::reqwest::Method::GET, localsend::reqwest::Method::POST] {
            assert_eq!(raw.request(method, format!("{base}{path}")).send().await.unwrap().status().as_u16(), 404);
        }
    }
    assert_eq!(raw.get(format!("{base}/api/localsend/v2/info")).send().await.unwrap().status().as_u16(), 200);
    let route = format!("{base}/api/localsend/v2/prepare-upload");
    assert_eq!(raw.post(&route).body(vec![b' '; 2048]).send().await.unwrap().status().as_u16(), 413);
    let chunked = localsend::reqwest::Body::wrap_stream(futures_util::stream::iter(
        (0..8).map(|_| Ok::<_, std::io::Error>(Bytes::from(vec![b' '; 256])))));
    assert_eq!(raw.post(&route).body(chunked).send().await.unwrap().status().as_u16(), 413);
    assert!(receiver.observations.try_recv().is_err());
    receiver.shutdown().await;
}

#[tokio::test]
async fn invalid_file_metadata_is_rejected_before_application_events() {
    let mut receiver = Receiver::start(SinkAction::Commit, TransferLimits::default()).await;
    let raw = receiver.raw_client();
    let route = format!("https://127.0.0.1:{}/api/localsend/v2/prepare-upload", receiver.handle.port());
    for scenario in 0..7 {
        let mut payload = receiver.payload(media());
        let mut expected_status = 400;
        match scenario {
            0 => payload.info.alias = "x".repeat(257),
            1 => payload.files.get_mut("file").unwrap().file_name = "../escape.png".into(),
            2 => payload.files.get_mut("file").unwrap().sha256 = Some("not a digest".into()),
            3 => payload.files.get_mut("file").unwrap().size = 0,
            4 => { payload.files.get_mut("file").unwrap().file_type = "text/plain".into(); expected_status = 415; }
            5 => payload.files.get_mut("file").unwrap().preview = Some("not accepted".into()),
            _ => { payload.files.clear(); for i in 0..65 { let mut file = media(); file.id = format!("f{i}"); payload.files.insert(file.id.clone(), file); } }
        }
        assert_eq!(raw.post(&route).json(&payload).send().await.unwrap().status().as_u16(), expected_status);
    }
    assert!(receiver.observations.try_recv().is_err());
    receiver.shutdown().await;
}
