use crate::config::{self, Config};
use crate::event::{self, HuhuEvent};
use crate::receive::{HuhuReceive, ReceiveReport};
use crate::status::{self, Error, Result};
use futures_util::FutureExt;
use localsend::discovery::{self, DeviceChannel, DeviceIdentity, DiscoveredDevice,
    DiscoveryConfig, DiscoveryEvent, DiscoveryHandle, HttpChannel, StatefulDevice};
use localsend::http::dto_v2::RegisterDtoV2;
use localsend::http::server::common::verified_upload::verified_upload_channel;
use localsend::http::server::v2::{PrepareUploadDecisionV2, ServerEventV2, SessionEndReasonV2};
use localsend::http::server::{self, ServerConfigV2, ServerHandle, TlsConfig};
use localsend::model::discovery::ProtocolType;
use localsend::model::transfer::FileDto;
use localsend::multicast::{self, MulticastDevice};
use localsend::util::interface::InterfaceFilter;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::panic::AssertUnwindSafe;
use std::sync::{Arc, Mutex, atomic::{AtomicI32, AtomicU64, Ordering}};
use std::time::Duration;
use tokio::runtime::{Handle, Runtime};
use tokio::sync::{mpsc, oneshot};
use tokio_util::sync::CancellationToken;
use tokio_util::task::TaskTracker;

pub struct Operation {
    pub cancel: CancellationToken,
    pub sending: bool,
    pub address: String,
    pub remote_session: Option<String>,
}

struct Incoming {
    id: String,
    decision: Option<oneshot::Sender<PrepareUploadDecisionV2>>,
    files: HashMap<String, FileDto>,
    accepted: HashSet<String>,
    report: ReceiveReport,
    cancel: CancellationToken,
    terminal: mpsc::OwnedPermit<HuhuEvent>,
}

struct Stops {
    server: Option<oneshot::Sender<()>>,
    discovery: Option<oneshot::Sender<()>>,
}

pub struct Shared {
    pub events: mpsc::Sender<HuhuEvent>,
    pub cancel: CancellationToken,
    pub identity: DeviceIdentity,
    pub info: RegisterDtoV2,
    pub max_file_bytes: u64,
    pub decision_timeout: Duration,
    pub idle_timeout: Duration,
    pub commit_timeout: Duration,
    pub runtime: Handle,
    pub server: Arc<ServerHandle>,
    pub discovery: Option<Arc<DiscoveryHandle>>,
    pub operations: Mutex<HashMap<u64, Operation>>,
    pub tasks: TaskTracker,
    incoming: Mutex<Option<Incoming>>,
    next_id: AtomicU64,
    stops: Mutex<Stops>,
    fatal_status: AtomicI32,
}

pub struct HuhuTransfer {
    runtime: Option<Runtime>,
    pub shared: Arc<Shared>,
    receiver: Mutex<mpsc::Receiver<HuhuEvent>>,
    pump: Option<tokio::task::JoinHandle<()>>,
    stop_event: Option<mpsc::OwnedPermit<HuhuEvent>>,
    stop_status: i32,
}

impl Shared {
    pub fn record_panic(&self) {
        self.fatal_status.store(status::PANIC, Ordering::Release);
        self.request_stop();
    }

    pub fn request_stop(&self) {
        self.cancel.cancel();
        let mut stops = self.stops.lock().unwrap_or_else(|error| error.into_inner());
        if let Some(stop) = stops.server.take() { let _ = stop.send(()); }
        if let Some(stop) = stops.discovery.take() { let _ = stop.send(()); }
    }

    pub fn live(&self) -> Result<()> {
        if self.cancel.is_cancelled() { Err(Error::new(status::CLOSED, "Transfer engine is stopping")) }
        else { Ok(()) }
    }

    pub fn update(&self, event: HuhuEvent) {
        // Leave headroom for offers/file handoffs. Progress is a snapshot;
        // terminal results use owned reservations and cannot be displaced.
        if !self.cancel.is_cancelled() && self.events.capacity() > 8 {
            let _ = self.events.try_send(event);
        }
    }

    pub fn admit(&self, sending: bool, address: String)
        -> Result<(u64, CancellationToken, mpsc::OwnedPermit<HuhuEvent>)> {
        self.live()?;
        let mut operations = self.operations.lock().map_err(|_| Error::new(status::PANIC, "Operation lock was poisoned"))?;
        if operations.len() >= 3 || (sending && operations.values().any(|operation| operation.sending)) {
            return Err(Error::new(status::BUSY, "Transfer operation budget is full"));
        }
        let terminal = self.events.clone().try_reserve_owned()
            .map_err(|_| Error::new(status::BUSY, "Event queue cannot reserve a terminal result"))?;
        let id = self.next_id.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(1))
            .map_err(|_| Error::new(status::INVALID_STATE, "Operation identifier space exhausted"))?;
        let cancel = self.cancel.child_token();
        operations.insert(id, Operation { cancel: cancel.clone(), sending, address, remote_session: None });
        Ok((id, cancel, terminal))
    }

    pub fn finish_operation(&self, id: u64) {
        self.operations.lock().unwrap_or_else(|error| error.into_inner()).remove(&id);
    }

    fn finish_incoming(&self, id: &str, reason: i32) {
        let incoming = {
            let mut incoming = self.incoming.lock().unwrap_or_else(|error| error.into_inner());
            if incoming.as_ref().is_some_and(|incoming| incoming.id == id) { incoming.take() }
            else { None }
        };
        let Some(incoming) = incoming else { return; };
        let mut report = incoming.report.lock().unwrap_or_else(|error| error.into_inner()).clone();
        for file in &incoming.accepted { report.entry(file.clone()).or_insert(status::IO); }
        let code = if reason != status::OK { reason }
            else if incoming.accepted.is_empty() { status::CANCELLED }
            else { report.values().copied().find(|code| *code != status::OK).unwrap_or(status::OK) };
        // Terminal is an observation, not a reason to revoke a successful
        // archive after its receipt. Failures stop any outstanding consumers.
        if code != status::OK { incoming.cancel.cancel(); }
        incoming.terminal.send(HuhuEvent::new(event::RECEIVE_END, code, 0,
            json!({"sessionId": incoming.id, "files": report})));
    }

    pub fn accept(&self, session: &str, ids: Vec<String>) -> Result<()> {
        self.live()?;
        if ids.len() > 64 || ids.iter().any(|id| !status::text_is_bounded(id, 128)) {
            return Err(Error::new(status::INVALID_ARGUMENT, "Invalid accepted file identifiers"));
        }
        let mut slot = self.incoming.lock().map_err(|_| Error::new(status::PANIC, "Receive session lock was poisoned"))?;
        let incoming = slot.as_mut().filter(|incoming| incoming.id == session)
            .ok_or_else(|| Error::new(status::CLOSED, "Receive offer no longer exists"))?;
        let ids: HashSet<_> = ids.into_iter().collect();
        if ids.iter().any(|id| !incoming.files.contains_key(id)) {
            return Err(Error::new(status::INVALID_ARGUMENT, "Accepted file was not offered"));
        }
        let decision = incoming.decision.take()
            .ok_or_else(|| Error::new(status::INVALID_STATE, "Receive offer was already answered"))?;
        incoming.accepted = ids.clone();
        let declined = ids.is_empty();
        let sent = decision.send(if declined { PrepareUploadDecisionV2::Decline }
            else { PrepareUploadDecisionV2::Accept(ids) });
        drop(slot);
        if declined || sent.is_err() { self.finish_incoming(session, status::CANCELLED); }
        sent.map_err(|_| Error::new(status::CLOSED, "Sender stopped waiting for the receive decision"))
    }

    pub fn cancel_receive(&self, session: &str) -> Result<()> {
        let mut slot = self.incoming.lock().map_err(|_| Error::new(status::PANIC, "Receive session lock was poisoned"))?;
        let incoming = slot.as_mut().filter(|incoming| incoming.id == session)
            .ok_or_else(|| Error::new(status::CLOSED, "Receive session no longer exists"))?;
        incoming.cancel.cancel();
        if let Some(decision) = incoming.decision.take() { let _ = decision.send(PrepareUploadDecisionV2::Decline); }
        drop(slot);
        self.runtime.block_on(self.server.cancel_v2_session(session));
        self.finish_incoming(session, status::CANCELLED);
        Ok(())
    }
}

impl HuhuTransfer {
    pub fn create(config: Config) -> Result<Self> {
        let config = config.validate()?;
        let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(2)
            .max_blocking_threads(4).thread_name("huhu-lan").enable_all().build()?;
        let (events, receiver) = mpsc::channel(event::EVENT_CAPACITY);
        let stop_event = events.clone().try_reserve_owned().map_err(|_| Error::new(status::BUSY, "Cannot reserve stop event"))?;
        let fatal_event = events.clone().try_reserve_owned().map_err(|_| Error::new(status::BUSY, "Cannot reserve listener event"))?;
        let (server_events, server_rx) = mpsc::channel(32);
        let (server_stop, server_stop_rx) = oneshot::channel();
        let max_file_bytes = config.limits.max_file_bytes;
        let decision_timeout = config.limits.decision_timeout;
        let idle_timeout = config.limits.idle_timeout;
        let commit_timeout = config.limits.commit_timeout;
        let server = Arc::new(runtime.block_on(server::start_bound_v2(config.addresses.clone(), config.port,
            TlsConfig { cert: config.identity.cert_pem.clone(), private_key: config.identity.private_key_pem.clone() },
            config.info.clone(), ServerConfigV2 { pin: config.pin, verify_checksums: true, event_tx: server_events },
            config.limits, server_stop_rx)).map_err(|error| Error::new(status::IO, error.to_string()))?);
        let info = config::register(&config.info, server.port());
        let (discovery_events, discovery_rx) = mpsc::channel(16);
        let mut discovery_stop = None;
        let discovery = if config.discovery {
            let (stop, stop_rx) = oneshot::channel();
            discovery_stop = Some(stop);
            let discovered = runtime.block_on(discovery::start_secure(DiscoveryConfig {
                group: multicast::DEFAULT_MULTICAST_GROUP, group_v6: None, port: multicast::DEFAULT_PORT,
                interface_filter: InterfaceFilter { whitelist: Some(config.addresses.iter().map(ToString::to_string).collect()), blacklist: None },
                device: MulticastDevice { alias: info.alias.clone(), version: info.version.clone(),
                    device_model: info.device_model.clone(), device_type: info.device_type.clone(),
                    fingerprint: info.fingerprint.clone(), port: info.port, protocol: ProtocolType::Https, download: false },
                identity: config.identity.clone(), timeout: Duration::from_secs(2), event_tx: Some(discovery_events),
            }, stop_rx));
            match discovered {
                Ok(discovery) => Some(Arc::new(discovery)),
                Err(error) => {
                    let _ = server_stop.send(());
                    runtime.block_on(server.wait_stopped());
                    return Err(Error::new(status::IO, error.to_string()));
                }
            }
        } else { drop(discovery_events); None };
        let shared = Arc::new(Shared { events, cancel: CancellationToken::new(), identity: config.identity,
            info, max_file_bytes, decision_timeout, idle_timeout, commit_timeout,
            runtime: runtime.handle().clone(), server, discovery,
            operations: Mutex::new(HashMap::new()), tasks: TaskTracker::new(),
            incoming: Mutex::new(None), next_id: AtomicU64::new(1),
            stops: Mutex::new(Stops { server: Some(server_stop), discovery: discovery_stop }),
            fatal_status: AtomicI32::new(status::OK) });
        if shared.discovery.as_ref().is_some_and(|discovery| discovery.multicast_error().is_some()) {
            shared.update(HuhuEvent::new(event::DISCOVERY_CHANGED, status::IO, 0,
                json!({"multicastAvailable": false, "error": "Multicast unavailable; manual TLS discovery remains available"})));
        }
        let pump_shared = shared.clone();
        let pump = runtime.spawn(async move {
            let mut fatal_event = Some(fatal_event);
            let result = AssertUnwindSafe(pump_events(pump_shared.clone(), server_rx, discovery_rx, &mut fatal_event))
                .catch_unwind().await;
            if result.is_err() {
                pump_shared.fatal_status.store(status::PANIC, Ordering::Release);
                pump_shared.request_stop();
                if let Some(terminal) = fatal_event.take() {
                    terminal.send(HuhuEvent::error(event::LISTENER_ERROR, 0, Error::new(status::PANIC, "LocalSend event worker panicked")));
                }
            }
            let session = pump_shared.incoming.lock().unwrap_or_else(|error| error.into_inner()).as_ref().map(|incoming| incoming.id.clone());
            if let Some(session) = session { pump_shared.finish_incoming(&session, status::CANCELLED); }
        });
        if let Some(discovery) = shared.discovery.clone() {
            let cancel = shared.cancel.clone();
            shared.tasks.spawn_on(async move {
                tokio::select! {
                    biased;
                    _ = cancel.cancelled() => {},
                    _ = discovery.announce() => {},
                }
            }, runtime.handle());
        }
        Ok(Self { runtime: Some(runtime), shared, receiver: Mutex::new(receiver), pump: Some(pump),
            stop_event: Some(stop_event), stop_status: status::OK })
    }

    pub fn info(&self) -> Value {
        json!({"port": self.shared.server.port(), "fingerprint": self.shared.info.fingerprint,
            "addresses": self.shared.server.local_addresses().iter().map(|address| address.ip().to_string()).collect::<Vec<_>>(),
            "multicastAvailable": self.shared.discovery.as_ref().is_some_and(|discovery|
                discovery.multicast_error().is_none() && !discovery.multicast_failed())})
    }

    pub fn poll(&self, timeout_ms: u32) -> Result<HuhuEvent> {
        let wait = event::timeout_duration(timeout_ms)?;
        let mut receiver = self.receiver.lock().map_err(|_| Error::new(status::PANIC, "Event receiver lock was poisoned"))?;
        match receiver.try_recv() {
            Ok(event) => return Ok(event),
            Err(mpsc::error::TryRecvError::Disconnected) => return Err(Error::new(status::CLOSED, "Event queue closed")),
            Err(mpsc::error::TryRecvError::Empty) => {}
        }
        let Some(runtime) = &self.runtime else { return Err(Error::new(status::CLOSED, "Transfer engine stopped")); };
        runtime.block_on(async { tokio::time::timeout(wait, receiver.recv()).await })
            .map_err(|_| Error::new(status::TIMEOUT, "No event available"))?
            .ok_or_else(|| Error::new(status::CLOSED, "Event queue closed"))
    }

    pub fn stop(&mut self) -> i32 {
        let Some(runtime) = self.runtime.take() else { return self.stop_status; };
        self.shared.request_stop();
        self.shared.tasks.close();
        runtime.block_on(async {
            self.shared.server.wait_stopped().await;
            if let Some(discovery) = &self.shared.discovery { discovery.wait_stopped().await; }
            self.shared.tasks.wait().await;
            if let Some(pump) = self.pump.take() {
                if pump.await.is_err() { self.shared.fatal_status.store(status::PANIC, Ordering::Release); }
            }
        });
        // Drop joins the runtime's blocking workers as well. A timeout/detach
        // would leave native file operations behind and violate ownership.
        drop(runtime);
        self.stop_status = self.shared.fatal_status.load(Ordering::Acquire);
        if let Some(event) = self.stop_event.take() {
            event.send(HuhuEvent::new(event::STOPPED, self.stop_status, 0, json!({})));
        }
        self.stop_status
    }
}

impl Drop for HuhuTransfer {
    fn drop(&mut self) { self.stop(); }
}

async fn pump_events(shared: Arc<Shared>, mut server_events: mpsc::Receiver<ServerEventV2>,
    mut discovery_events: mpsc::Receiver<DiscoveryEvent>, fatal_event: &mut Option<mpsc::OwnedPermit<HuhuEvent>>) {
    let mut discovery_closed = false;
    loop {
        tokio::select! {
            biased;
            _ = shared.cancel.cancelled() => break,
            received = server_events.recv() => {
                let Some(received) = received else { break; };
                match received {
                    ServerEventV2::PrepareUpload { session_id, ip, info, sender_fingerprint, files, decision_tx } => {
                        if sender_fingerprint.is_none() { let _ = decision_tx.send(PrepareUploadDecisionV2::Decline); continue; }
                        let Ok(terminal) = shared.events.clone().try_reserve_owned() else {
                            let _ = decision_tx.send(PrepareUploadDecisionV2::Decline); continue;
                        };
                        let event = HuhuEvent::new(event::RECEIVE_OFFER, status::OK, 0,
                            json!({"sessionId": session_id, "peerIp": ip.to_string(), "alias": info.alias,
                                "fingerprint": sender_fingerprint, "files": files.values().collect::<Vec<_>>()}));
                        let mut incoming = shared.incoming.lock().unwrap_or_else(|error| error.into_inner());
                        if incoming.is_some() { let _ = decision_tx.send(PrepareUploadDecisionV2::Decline); continue; }
                        // Store BEFORE the event can be polled and answered.
                        *incoming = Some(Incoming { id: session_id.clone(), decision: Some(decision_tx), files,
                            accepted: HashSet::new(), report: Arc::new(Mutex::new(HashMap::new())),
                            cancel: shared.cancel.child_token(), terminal });
                        if shared.events.try_send(event).is_err() {
                            if let Some(mut rejected) = incoming.take() {
                                if let Some(decision) = rejected.decision.take() { let _ = decision.send(PrepareUploadDecisionV2::Decline); }
                            }
                        }
                    }
                    ServerEventV2::FileUpload { session_id, file_id, file, target_tx } => {
                        let session = {
                            let incoming = shared.incoming.lock().unwrap_or_else(|error| error.into_inner());
                            incoming.as_ref().filter(|incoming| incoming.id == session_id && incoming.accepted.contains(&file_id))
                                .map(|incoming| (incoming.report.clone(), incoming.cancel.clone()))
                        };
                        let Some((report, cancel)) = session else { drop(target_tx); continue; };
                        let (target, receiver) = verified_upload_channel();
                        let receive = HuhuReceive::new(shared.runtime.clone(), shared.cancel.clone(), cancel,
                            file_id.clone(), &file.file_type, report, receiver);
                        let Ok(receive) = receive else { drop(target_tx); continue; };
                        let mut event = HuhuEvent::new(event::RECEIVE_FILE, status::OK, 0,
                            json!({"sessionId": session_id, "fileId": file_id, "file": file}));
                        event.receive = Some(Box::new(receive));
                        // If the application cannot accept the handoff, dropping
                        // it reports failure and the core cannot ACK success.
                        if shared.events.try_send(event).is_ok() { let _ = target_tx.send(target); }
                    }
                    ServerEventV2::SessionEnd { session_id, reason } => {
                        shared.finish_incoming(&session_id, if reason == SessionEndReasonV2::Finished { status::OK } else { status::CANCELLED });
                    }
                    ServerEventV2::PrepareUploadAborted { session_id } => shared.finish_incoming(&session_id, status::CANCELLED),
                    ServerEventV2::CancelReceived { ip, session_id } => {
                        for operation in shared.operations.lock().unwrap_or_else(|error| error.into_inner()).values() {
                            if operation.address == ip.to_string() && operation.remote_session.as_deref() == Some(session_id.as_str()) {
                                operation.cancel.cancel();
                            }
                        }
                    }
                    ServerEventV2::Register { ip, info } => {
                        if let Some(discovery) = &shared.discovery {
                            discovery.observe_inbound(DiscoveredDevice { alias: info.alias, version: info.version,
                                device_model: info.device_model, device_type: info.device_type, fingerprint: info.fingerprint,
                                channel: DeviceChannel::Http(HttpChannel { host: ip.to_string(), port: info.port, protocol: ProtocolType::Https }),
                                download: false }).await;
                        }
                    }
                    ServerEventV2::ListenerFailed { error } => {
                        shared.fatal_status.store(status::IO, Ordering::Release);
                        if let Some(terminal) = fatal_event.take() {
                            terminal.send(HuhuEvent::error(event::LISTENER_ERROR, 0, Error::new(status::IO, error)));
                        }
                        shared.request_stop();
                    }
                }
            }
            received = discovery_events.recv(), if !discovery_closed => {
                match received {
                    Some(DiscoveryEvent::MulticastFailed) => shared.update(HuhuEvent::new(event::DISCOVERY_CHANGED,
                        status::IO, 0, json!({"multicastAvailable": false}))),
                    Some(_) => shared.update(HuhuEvent::new(event::DISCOVERY_CHANGED, status::OK, 0, json!({}))),
                    None => discovery_closed = true,
                }
            }
        }
    }
}

pub fn device_json(device: &StatefulDevice) -> Value {
    json!({"alias": device.device.alias, "version": device.device.version,
        "fingerprint": device.device.fingerprint, "confirmed": device.confirmed, "verified": device.verified,
        "channels": device.get_ranked_channels().iter().filter_map(|channel| channel.http()).map(|channel|
            json!({"address": channel.host, "port": channel.port, "protocol": channel.protocol.as_str()})).collect::<Vec<_>>()})
}
