//! Exercises the exported ABI, actual TLS sockets and stable OS files. The
//! Qt integration probe owns full decoding and durable FileCommitter success.
use huhucapture_native::*;
use serde_json::{json, Value};
use std::ptr;
use std::time::{Duration, Instant};

const PNG: &[u8] = include_bytes!("../../../../third_party/localsend/core/tests/fixtures/one-pixel.png");
const OK: i32 = 0;
const TIMEOUT: i32 = 1;
const END: i32 = 2;
const INVALID_ARGUMENT: i32 = -1;
const INVALID_STATE: i32 = -2;
const IO: i32 = -3;
const CANCELLED: i32 = -4;
const CHECKSUM_MISMATCH: i32 = -7;
const CLOSED: i32 = -11;
const LIMIT_EXCEEDED: i32 = -13;

struct Engine(*mut HuhuTransfer, Value);

impl Engine {
    fn new() -> Self {
        unsafe {
            let mut identity = ptr::null_mut();
            assert_eq!(huhu_identity_generate(&mut identity), OK);
            let mut view = HuhuSlice { data: ptr::null(), length: 0 };
            assert_eq!(huhu_buffer_view(identity, &mut view), OK);
            let mut config: Value = serde_json::from_slice(std::slice::from_raw_parts(view.data, view.length)).unwrap();
            huhu_buffer_free(identity);
            config["alias"] = json!("native ABI test");
            config["addresses"] = json!(["127.0.0.1"]);
            config["port"] = json!(0);
            config["discovery"] = json!(false);
            config["decisionTimeoutMs"] = json!(10000);
            config["idleTimeoutMs"] = json!(10000);
            config["commitTimeoutMs"] = json!(10000);
            let config = config.to_string();
            let mut engine = ptr::null_mut();
            assert_eq!(huhu_transfer_create(config.as_ptr(), config.len(), &mut engine), OK);
            let mut owner = Self(engine, Value::Null);
            let mut info = ptr::null_mut();
            assert_eq!(huhu_transfer_info(engine, &mut info), OK);
            assert_eq!(huhu_buffer_view(info, &mut view), OK);
            owner.1 = serde_json::from_slice(std::slice::from_raw_parts(view.data, view.length)).unwrap();
            huhu_buffer_free(info);
            owner
        }
    }

    fn peer(&self) -> Vec<u8> {
        serde_json::to_vec(&json!({"address":"127.0.0.1", "port": self.1["port"],
            "fingerprint": self.1["fingerprint"]})).unwrap()
    }

    fn event(&self) -> Option<Event> {
        unsafe {
            let mut event = ptr::null_mut();
            let result = huhu_transfer_poll(self.0, 10, &mut event);
            if result == TIMEOUT || result == CLOSED { assert!(event.is_null()); return None; }
            assert_eq!(result, OK);
            Some(Event(event))
        }
    }

    fn next(&self, kind: u32) -> Event {
        let end = Instant::now() + Duration::from_secs(20);
        while Instant::now() < end {
            if let Some(event) = self.event() {
                if event.view().kind == kind { return event; }
                assert_ne!(event.view().kind, 8, "Listener failure: {}", event.json());
            }
        }
        panic!("Timed out waiting for ABI event {kind}");
    }

    fn send(&self, receiver: &Engine, lease: &Lease) -> u64 {
        let peer = receiver.peer();
        let leases = [lease.0 as *const HuhuMediaLease];
        let mut operation = 0;
        unsafe { assert_eq!(huhu_transfer_send(self.0, peer.as_ptr(), peer.len(), leases.as_ptr(), 1, &mut operation), OK); }
        assert_ne!(operation, 0);
        operation
    }

    fn stop(&self) -> Vec<(u32, i32, u64)> {
        unsafe { assert_eq!(huhu_transfer_stop(self.0), OK); }
        let mut result = Vec::new();
        loop {
            let mut raw = ptr::null_mut();
            let code = unsafe { huhu_transfer_poll(self.0, 0, &mut raw) };
            if code == CLOSED { assert!(raw.is_null()); break; }
            assert_eq!(code, OK);
            let event = Event(raw);
            let view = event.view();
            result.push((view.kind, view.status, view.operation));
        }
        assert_eq!(result.iter().filter(|event| event.0 == 9).count(), 1);
        assert_eq!(result.last().unwrap().0, 9);
        unsafe { assert_eq!(huhu_transfer_stop(self.0), OK); }
        result
    }
}

impl Drop for Engine { fn drop(&mut self) { unsafe { huhu_transfer_free(self.0); } } }

struct Event(*mut HuhuEvent);
impl Event {
    fn view(&self) -> HuhuEventView {
        let mut view = HuhuEventView { kind: 0, status: 0, operation: 0,
            json: HuhuSlice { data: ptr::null(), length: 0 } };
        unsafe { assert_eq!(huhu_event_view(self.0, &mut view), OK); }
        view
    }
    fn json(&self) -> Value {
        let view = self.view();
        unsafe { serde_json::from_slice(std::slice::from_raw_parts(view.json.data, view.json.length)).unwrap() }
    }
}
impl Drop for Event { fn drop(&mut self) { unsafe { huhu_event_free(self.0); } } }

struct Lease(*mut HuhuMediaLease, std::path::PathBuf);
impl Lease {
    fn new() -> Self {
        Self::from_bytes("png", PNG)
    }
    fn from_bytes(extension: &str, bytes: &[u8]) -> Self {
        let path = std::env::temp_dir().join(format!("huhu-abi-{}-{}.{extension}", std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()));
        std::fs::File::options().create_new(true).write(true).open(&path).unwrap();
        std::fs::write(&path, bytes).unwrap();
        let text = path.to_str().unwrap();
        let mut lease = ptr::null_mut();
        unsafe { assert_eq!(huhu_media_lease_open(text.as_ptr(), text.len(), &mut lease), OK); }
        Self(lease, path)
    }
}
impl Drop for Lease {
    fn drop(&mut self) { unsafe { huhu_media_lease_free(self.0); } let _ = std::fs::remove_file(&self.1); }
}

struct Receive(*mut HuhuReceive);
impl Drop for Receive { fn drop(&mut self) { unsafe { huhu_receive_free(self.0); } } }

fn accept(receiver: &Engine, offer: &Event) {
    let offer = offer.json();
    let session = offer["sessionId"].as_str().unwrap();
    let ids = serde_json::to_vec(&vec![offer["files"][0]["id"].as_str().unwrap()]).unwrap();
    unsafe {
        assert_eq!(huhu_transfer_accept(receiver.0, session.as_ptr(), session.len(), ids.as_ptr(), ids.len()), OK);
    }
}

#[test]
fn abi_rejects_invalid_inputs_without_ownership_changes() {
    assert_eq!(huhu_native_abi_version(), 1);
    unsafe {
        assert_eq!(huhu_identity_generate(ptr::null_mut()), INVALID_ARGUMENT);
        let mut engine = ptr::dangling_mut();
        assert_eq!(huhu_transfer_create(ptr::null(), 7, &mut engine), INVALID_ARGUMENT);
        assert!(engine.is_null());
        let mut lease = ptr::dangling_mut();
        assert_eq!(huhu_media_lease_open(b"x\0.png".as_ptr(), 6, &mut lease), INVALID_ARGUMENT);
        assert!(lease.is_null());
        huhu_buffer_free(ptr::null_mut());
        huhu_event_free(ptr::null_mut());
        huhu_media_lease_free(ptr::null_mut());
        huhu_receive_free(ptr::null_mut());
        huhu_transfer_free(ptr::null_mut());
    }
}

#[test]
fn pending_receiver_cancel_then_verified_io_failure_keep_real_terminal_results() {
    let receiver = Engine::new();
    let sender = Engine::new();
    let lease = Lease::new();
    let operation = sender.send(&receiver, &lease);
    let offer = receiver.next(1);
    let json = offer.json();
    let session = json["sessionId"].as_str().unwrap();
    unsafe { assert_eq!(huhu_transfer_cancel_receive(receiver.0, session.as_ptr(), session.len()), OK); }
    assert_eq!(receiver.next(3).view().status, CANCELLED);
    let ended = sender.next(5).view();
    assert_eq!((ended.operation, ended.status), (operation, CANCELLED));

    // The cancelled Pending slot must be reusable, without a late acceptance
    // reviving it. Reuse the exact same stable media lease and engine pair.
    let operation = sender.send(&receiver, &lease);
    let offer = receiver.next(1);
    accept(&receiver, &offer);
    let body = receiver.next(2);
    let mut raw = ptr::null_mut();
    unsafe { assert_eq!(huhu_event_take_receive(body.0, &mut raw), OK); }
    let receive = Receive(raw);
    let mut verification = HuhuVerification::default();
    unsafe {
        assert_eq!(huhu_receive_complete(raw, OK, 1), INVALID_STATE);
        assert_eq!(huhu_receive_verify(raw, 0, &mut verification), INVALID_STATE);
    }
    let mut actual = Vec::new();
    let mut buffer = [0; 17];
    let end = Instant::now() + Duration::from_secs(20);
    loop {
        assert!(Instant::now() < end);
        let mut length = 0;
        let code = unsafe { huhu_receive_read(raw, buffer.as_mut_ptr(), buffer.len(), &mut length, 100) };
        if code == TIMEOUT { continue; }
        if code == END { assert_eq!(length, 0); break; }
        assert_eq!(code, OK);
        assert!(length > 0 && length <= buffer.len());
        actual.extend_from_slice(&buffer[..length]);
    }
    assert_eq!(actual, PNG);
    loop {
        assert!(Instant::now() < end);
        let code = unsafe { huhu_receive_verify(raw, 100, &mut verification) };
        if code == TIMEOUT { continue; }
        assert_eq!(code, OK);
        break;
    }
    assert_eq!(verification.received_bytes, PNG.len() as u64);
    assert_eq!(verification.sha256.as_slice(), localsend::crypto::hash::sha256(PNG));
    assert_eq!(verification.sender_checksum_matched, 1);
    assert_eq!(verification.media_kind, 1);
    unsafe {
        assert_eq!(huhu_receive_is_cancelled(raw), 0);
        // Simulate an application archive I/O failure after real transfer and
        // verification; no success receipt, mock server or published file.
        assert_eq!(huhu_receive_complete(raw, IO, 1), OK);
        assert_eq!(huhu_receive_complete(raw, IO, 1), INVALID_STATE);
    }
    drop(receive);
    assert_eq!(receiver.next(3).view().status, IO);
    let ended = sender.next(5).view();
    assert_eq!(ended.operation, operation);
    assert_ne!(ended.status, OK);
    assert_eq!(std::fs::read(&lease.1).unwrap(), PNG);
    sender.stop();
    receiver.stop();
}

#[test]
fn stop_joins_pending_real_tls_operation_and_preserves_its_terminal_event() {
    let receiver = Engine::new();
    let sender = Engine::new();
    let lease = Lease::new();
    let operation = sender.send(&receiver, &lease);
    receiver.next(1); // Keep the real HTTP request pending on a user decision.
    let drained = sender.stop();
    assert_eq!(drained.iter().filter(|event| event.0 == 5).copied().collect::<Vec<_>>(),
        vec![(5, CANCELLED, operation)]);
    assert_eq!(receiver.next(3).view().status, CANCELLED);
    receiver.stop();
}

#[test]
fn explicit_tls_discovery_returns_verified_peer_identity() {
    let receiver = Engine::new();
    let sender = Engine::new();
    let mut operation = 0;
    unsafe { assert_eq!(huhu_transfer_discover(sender.0, b"127.0.0.1".as_ptr(), 9,
        receiver.1["port"].as_u64().unwrap() as u16, &mut operation), OK); }
    let discovered = sender.next(7);
    assert_eq!((discovered.view().operation, discovered.view().status), (operation, OK));
    assert_eq!(discovered.json()["fingerprint"], receiver.1["fingerprint"]);
    sender.stop();
    receiver.stop();
}

#[test]
fn late_failed_receive_cannot_overwrite_a_real_tls_retry_result() {
    use localsend::crypto::{cert::generate_self_signed, hash::sha256_hex};
    use localsend::http::client::{ClientError, LsHttpClientV2};
    use localsend::http::dto_v2::{PrepareUploadRequestDtoV2, RegisterDtoV2};
    use localsend::model::{discovery::ProtocolType, transfer::FileDto};
    use std::collections::HashMap;
    use std::sync::Arc;
    use tokio_util::sync::CancellationToken;

    fn take(receiver: &Engine, id: &str) -> Receive {
        let event = receiver.next(2);
        assert_eq!(event.json()["fileId"], id);
        let mut raw = ptr::null_mut();
        unsafe { assert_eq!(huhu_event_take_receive(event.0, &mut raw), OK); }
        Receive(raw)
    }

    fn consume(receive: &Receive, expected: &[u8], verification_status: i32) {
        let deadline = Instant::now() + Duration::from_secs(20);
        let mut actual = Vec::new();
        let mut bytes = [0; 17];
        loop {
            assert!(Instant::now() < deadline);
            let mut count = 0;
            let code = unsafe { huhu_receive_read(receive.0, bytes.as_mut_ptr(), bytes.len(), &mut count, 100) };
            if code == TIMEOUT { continue; }
            if code == END { assert_eq!(count, 0); break; }
            assert_eq!(code, OK);
            assert!(count > 0 && count <= bytes.len());
            actual.extend_from_slice(&bytes[..count]);
        }
        assert_eq!(actual, expected);
        let mut verification = HuhuVerification::default();
        loop {
            assert!(Instant::now() < deadline);
            let code = unsafe { huhu_receive_verify(receive.0, 100, &mut verification) };
            if code == TIMEOUT { continue; }
            assert_eq!(code, verification_status);
            break;
        }
        if verification_status == OK {
            assert_eq!(verification.received_bytes, expected.len() as u64);
            assert_eq!(verification.sender_checksum_matched, 1);
            assert_eq!(verification.media_kind, 1);
        }
    }

    fn expect_http(runtime: &tokio::runtime::Runtime,
        task: tokio::task::JoinHandle<Result<(), ClientError>>, status: u16) {
        let result = runtime.block_on(async {
            tokio::time::timeout(Duration::from_secs(20), task).await.unwrap().unwrap()
        });
        assert!(matches!(&result, Err(ClientError::StatusCode(error)) if error.status == status),
            "expected HTTP {status}, received {result:?}");
    }

    let receiver = Engine::new();
    let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().unwrap();
    let identity = generate_self_signed().unwrap();
    let client = {
        let _entered = runtime.enter();
        Arc::new(LsHttpClientV2::try_new(&identity.private_key_pem, &identity.certificate_pem,
            Some(receiver.1["fingerprint"].as_str().unwrap().to_owned()), Some(Duration::from_secs(20))).unwrap())
    };
    let port = receiver.1["port"].as_u64().unwrap() as u16;
    let files: HashMap<_, _> = ["retry", "last"].into_iter().map(|id| (id.to_owned(), FileDto {
        id: id.to_owned(), file_name: format!("{id}.png"), size: PNG.len() as u64,
        file_type: "image/png".into(), sha256: Some(sha256_hex(PNG)), preview: None, metadata: None,
    })).collect();
    let prepare = runtime.spawn({
        let client = client.clone();
        async move {
            client.prepare_upload(ProtocolType::Https, "127.0.0.1", port, None,
                PrepareUploadRequestDtoV2 { info: RegisterDtoV2 {
                    alias: "real retry sender".into(), version: "2.2".into(), device_model: None,
                    device_type: None, fingerprint: identity.fingerprint, port: 53317,
                    protocol: ProtocolType::Https, download: false,
                }, files }, None, CancellationToken::new()).await
        }
    });
    let offer = receiver.next(1).json();
    assert_eq!(offer["files"].as_array().unwrap().len(), 2);
    let session_id = offer["sessionId"].as_str().unwrap();
    let selected = br#"["retry","last"]"#;
    unsafe { assert_eq!(huhu_transfer_accept(receiver.0, session_id.as_ptr(), session_id.len(),
        selected.as_ptr(), selected.len()), OK); }
    let session = runtime.block_on(async {
        tokio::time::timeout(Duration::from_secs(20), prepare).await.unwrap().unwrap().unwrap().response.unwrap()
    });
    let upload = |id: &str, body: Vec<u8>| {
        let client = client.clone();
        let token = session.files[id].clone();
        let session_id = session.session_id.clone();
        let id = id.to_owned();
        runtime.spawn(async move {
            client.upload(ProtocolType::Https, "127.0.0.1", port, None, &session_id, &id,
                &token, body.into(), CancellationToken::new()).await
        })
    };

    // Corrupt the real request body without changing its length or PNG magic.
    // The HTTP 422 keeps this file/token retryable; retain its C consumer.
    let mut corrupt = PNG.to_vec();
    *corrupt.last_mut().unwrap() ^= 1;
    let first = upload("retry", corrupt.clone());
    let old = take(&receiver, "retry");
    consume(&old, &corrupt, CHECKSUM_MISMATCH);
    expect_http(&runtime, first, 422);

    let second = upload("retry", PNG.to_vec());
    let retry = take(&receiver, "retry");
    consume(&retry, PNG, OK);
    // The archive reports a real failure, so there is no fabricated durable
    // receipt. A second offered file keeps RECEIVE_END from racing this drop.
    unsafe { assert_eq!(huhu_receive_complete(retry.0, IO, 1), OK); }
    expect_http(&runtime, second, 500);
    drop(retry);
    drop(old); // Must not replace the retry's IO with the old checksum error.

    let last = upload("last", PNG.to_vec());
    let receive = take(&receiver, "last");
    consume(&receive, PNG, OK);
    unsafe { assert_eq!(huhu_receive_complete(receive.0, IO, 1), OK); }
    expect_http(&runtime, last, 500);
    let ended = receiver.next(3);
    assert_eq!(ended.view().status, IO);
    assert_eq!(ended.json()["files"], json!({"retry": IO, "last": IO}));
    receiver.stop();
}

#[test]
fn strict_gif_cancel_and_decode_budget_failure_cross_real_tls_without_ack() {
    let mut encoder = gif::Encoder::new(Vec::new(), 1, 1, &[0, 0, 0, 255, 255, 255]).unwrap();
    for value in [0, 1] {
        encoder.write_frame(&gif::Frame { width: 1, height: 1,
            buffer: std::borrow::Cow::Owned(vec![value]), ..gif::Frame::default() }).unwrap();
    }
    let bytes = encoder.into_inner().unwrap();
    let lease = Lease::from_bytes("gif", &bytes);
    let limits = HuhuGifLimits { max_input_bytes: 1024 * 1024, max_canvas_pixels: 100,
        max_total_pixels: 100, max_frames: 1, max_metadata_bytes: 1024 };
    let receiver = Engine::new();
    let sender = Engine::new();
    let source = lease.1.to_str().unwrap();

    let first = sender.send(&receiver, &lease);
    let offer = receiver.next(1);
    accept(&receiver, &offer);
    let event = receiver.next(2);
    let mut raw = ptr::null_mut();
    unsafe { assert_eq!(huhu_event_take_receive(event.0, &mut raw), OK); }
    let cancelled = Receive(raw);
    let offer = offer.json();
    let session = offer["sessionId"].as_str().unwrap();
    let mut info = HuhuGifInfo::default();
    unsafe {
        assert_eq!(huhu_transfer_cancel_receive(receiver.0, session.as_ptr(), session.len()), OK);
        assert_eq!(huhu_gif_validate_file(source.as_ptr(), source.len(), &limits, cancelled.0, ptr::null(), &mut info), CANCELLED);
    }
    assert_eq!(receiver.next(3).view().status, CANCELLED);
    let ended = sender.next(5).view();
    assert_eq!((ended.operation, ended.status), (first, CANCELLED));
    drop(cancelled);

    let second = sender.send(&receiver, &lease);
    let offer = receiver.next(1);
    accept(&receiver, &offer);
    let event = receiver.next(2);
    unsafe { assert_eq!(huhu_event_take_receive(event.0, &mut raw), OK); }
    let receive = Receive(raw);
    let deadline = Instant::now() + Duration::from_secs(20);
    let mut actual = Vec::new();
    let mut chunk = [0; 17];
    loop {
        assert!(Instant::now() < deadline);
        let mut count = 0;
        let code = unsafe { huhu_receive_read(raw, chunk.as_mut_ptr(), chunk.len(), &mut count, 100) };
        if code == TIMEOUT { continue; }
        if code == END { break; }
        assert_eq!(code, OK);
        assert!(count > 0);
        actual.extend_from_slice(&chunk[..count]);
    }
    assert_eq!(actual, bytes);
    let mut verified = HuhuVerification::default();
    loop {
        assert!(Instant::now() < deadline);
        let code = unsafe { huhu_receive_verify(raw, 100, &mut verified) };
        if code == TIMEOUT { continue; }
        assert_eq!(code, OK);
        break;
    }
    unsafe {
        // The received bytes equal this immutable real file; the official
        // decoder encounters the second frame beyond the explicit one-frame
        // receiver budget. No incomplete file is committed or acknowledged.
        assert_eq!(huhu_gif_validate_file(source.as_ptr(), source.len(), &limits, raw, ptr::null(), &mut info), LIMIT_EXCEEDED);
        assert_eq!(huhu_receive_complete(raw, LIMIT_EXCEEDED, 2), OK);
    }
    assert_eq!(receiver.next(3).view().status, LIMIT_EXCEEDED);
    let ended = sender.next(5);
    assert_eq!((ended.view().operation, ended.view().status), (second, LIMIT_EXCEEDED));
    assert_eq!(ended.json()["httpStatus"], 413);
    drop(receive);
    sender.stop();
    receiver.stop();
}
