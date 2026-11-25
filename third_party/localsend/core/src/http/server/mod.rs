// huhucapture modification; see docs/localsend-core.md and upstream.json.
pub mod common;
mod connection_limit;
pub mod internal;
mod peer_ip;
pub mod v2;
pub mod v3;
pub mod web;

pub use peer_ip::PeerIp;

use crate::crypto::cert::{fingerprint_from_cert_der, public_key_from_cert_der};
use crate::http::server::internal::{InternalConfig, InternalState};
use crate::http::server::v2::ServerEventV2;
use crate::http::server::web::{WebConfig, WebShare};
use crate::http::state::ClientInfo;
use crate::http::limits::{TransferLimits, HEADER_BYTES, HEADER_COUNT};
use common::client_cert_verifier::CustomClientCertVerifier;
use common::error::AppError;
use common::response;
use common::response::BoxedBody;
use common::session::SessionStateV2;
use connection_limit::{ConnectionLimiter, LimitReached, MAX_CONNECTIONS, MAX_CONNECTIONS_PER_IP};
use hyper::body::Incoming;
use hyper::{Method, Request, Response, StatusCode};
use hyper_util::rt::{TokioIo, TokioTimer};
use lru::LruCache;
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, PrivateKeyDer};
use std::fmt::Debug;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::num::NonZeroUsize;
use std::ops::Deref;
use std::sync::{Arc, Mutex as SyncMutex};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::sync::{mpsc, oneshot, Mutex, Semaphore};
use tokio_util::sync::CancellationToken;
use tokio_util::task::TaskTracker;
use web::WebState;

/// Configuration for the v2 (legacy) protocol endpoints.
pub struct ServerConfigV2 {
    /// Optional PIN that senders must provide via the `pin` query parameter.
    pub pin: Option<String>,

    /// Whether the SHA-256 checksums that senders provide for their files are
    /// verified after receiving. When disabled, received files are not hashed
    /// and a mismatch is not detected.
    pub verify_checksums: bool,

    /// Channel on which the server emits events that must be handled by the application.
    pub event_tx: mpsc::Sender<ServerEventV2>,
}

/// Runtime state of the v2 protocol endpoints.
pub(crate) struct V2State {
    /// Optional PIN required for prepare-upload requests.
    pub(crate) pin: Option<String>,

    /// Whether sender-provided SHA-256 checksums are verified after receiving.
    pub(crate) verify_checksums: bool,

    /// Channel on which server events are emitted to the application.
    pub(crate) event_tx: mpsc::Sender<ServerEventV2>,

    /// The single upload session slot. Only one session can be active at a time.
    // No I/O or await while this lock is held. A synchronous lock lets a
    // dropped request release its session and reserved terminal event without
    // creating detached cleanup tasks after shutdown.
    pub(crate) session: SyncMutex<Option<SessionStateV2>>,

    /// Maps client IPs to the number of failed PIN attempts.
    pub(crate) pin_attempts: Mutex<LruCache<IpAddr, u32>>,

    /// Strict-mode upload concurrency, held until the archive commit responds.
    pub(crate) uploads: Semaphore,
}

#[derive(Clone)]
pub struct AppState {
    /// Information about server's device.
    info: Arc<Mutex<ClientInfo>>,

    /// Runtime state of the browser-facing pages and web download.
    web: Arc<WebState>,

    /// State for application-internal endpoints.
    internal: Option<Arc<InternalState>>,

    /// Maps client identifiers to nonces that have been received from remote.
    received_nonce_map: Arc<Mutex<LruCache<String, Vec<u8>>>>,

    /// Maps client identifiers to nonces that are expected to be received from remote.
    generated_nonce_map: Arc<Mutex<LruCache<String, Vec<u8>>>>,

    /// State of the v2 protocol endpoints. `None` disables the v2 routes.
    v2: Option<Arc<V2State>>,

    /// Limits the connections being served, shared by the IPv4 and IPv6 listeners.
    connection_limiter: Arc<ConnectionLimiter>,

    /// Native receiver restrictions; absent for unchanged upstream callers.
    strict: Option<Arc<TransferLimits>>,
}

impl AppState {
    fn new(
        info: Arc<Mutex<ClientInfo>>,
        internal_config: Option<InternalConfig>,
        v2_config: Option<ServerConfigV2>,
        web_config: WebConfig,
    ) -> Self {
        let v2 = v2_config.map(|config| {
            Arc::new(V2State {
                pin: config.pin,
                verify_checksums: config.verify_checksums,
                event_tx: config.event_tx,
                session: SyncMutex::new(None),
                pin_attempts: Mutex::new(LruCache::new(NonZeroUsize::new(200).unwrap())),
                uploads: Semaphore::new(2),
            })
        });

        let internal = internal_config.map(|config| Arc::new(InternalState::new(config)));

        Self {
            info,
            web: Arc::new(WebState::from(web_config)),
            internal,
            received_nonce_map: Arc::new(Mutex::new(LruCache::new(
                NonZeroUsize::new(200).unwrap(),
            ))),
            generated_nonce_map: Arc::new(Mutex::new(LruCache::new(
                NonZeroUsize::new(200).unwrap(),
            ))),
            v2,
            connection_limiter: ConnectionLimiter::new(MAX_CONNECTIONS, MAX_CONNECTIONS_PER_IP),
            strict: None,
        }
    }
}

/// A handle to a running server for interactions initiated by the application
/// (as opposed to the event channels which are driven by incoming requests).
pub struct ServerHandle {
    v2: Option<Arc<V2State>>,

    /// The port the listeners are bound to.
    port: u16,

    /// Whether the IPv6 wildcard listener could be bound.
    ipv6_bound: bool,

    /// Exact listeners when wildcard binding is disabled.
    bound_addresses: Vec<SocketAddr>,

    /// The task running the accept loops. Completes after a stop has been
    /// requested, the listeners have been dropped and all connections have
    /// been closed.
    task: Mutex<Option<tokio::task::JoinHandle<()>>>,
}

impl ServerHandle {
    /// The port the listeners are bound to. Relevant when the server was
    /// started with port 0, where the OS picks the port.
    pub fn port(&self) -> u16 {
        self.port
    }

    /// The socket addresses this server can be reached at: every address of
    /// the non-loopback interfaces, restricted to the address families that
    /// are actually bound. The listeners themselves only know the wildcard
    /// addresses, so the concrete addresses come from interface enumeration.
    ///
    /// Link-local IPv6 addresses are skipped: peers can only use them together
    /// with their own scope, which this device cannot know.
    ///
    /// Empty when the interfaces cannot be enumerated.
    pub fn local_addresses(&self) -> Vec<SocketAddr> {
        if !self.bound_addresses.is_empty() {
            return self.bound_addresses.clone();
        }
        let Ok(interfaces) = if_addrs::get_if_addrs() else {
            return Vec::new();
        };
        let mut addresses: Vec<SocketAddr> = interfaces
            .into_iter()
            .filter(|interface| !interface.is_loopback())
            .filter_map(|interface| match interface.ip() {
                IpAddr::V4(address) => Some(SocketAddr::new(address.into(), self.port)),
                IpAddr::V6(address) if self.ipv6_bound && !address.is_unicast_link_local() => {
                    Some(SocketAddr::new(address.into(), self.port))
                }
                IpAddr::V6(_) => None,
            })
            .collect();
        addresses.sort();
        addresses.dedup();
        addresses
    }

    /// Waits until the server task has terminated, the listeners are closed
    /// and all connections have been dropped, so that the port can be bound again.
    /// Must be called after requesting a stop via the stop channel.
    pub async fn wait_stopped(&self) {
        if let Some(task) = self.task.lock().await.take() {
            let _ = task.await;
        }
    }

    /// Cancels the active v2 upload session if it matches `session_id`,
    /// e.g. because the user aborted the transfer on the receiving side.
    ///
    /// Interrupts in-progress verified streams and rejects subsequent uploads.
    /// The archive adapter must recheck cancellation before publishing a file.
    /// No [ServerEventV2::SessionEnd] is emitted: the application initiated
    /// the cancellation itself.
    ///
    /// Returns `true` when a session was cancelled.
    pub async fn cancel_v2_session(&self, session_id: &str) -> bool {
        let Some(v2) = &self.v2 else {
            return false;
        };
        let mut slot = v2.session.lock().unwrap();
        match slot.as_ref() {
            Some(SessionStateV2::Active(session)) if session.session_id == session_id => {
                session.cancel.cancel();
                *slot = None;
                true
            }
            _ => false,
        }
    }
}

/// Binds the server to the specified port on both IPv4 and IPv6 addresses.
pub async fn start_with_port(
    port: u16,
    tls_config: Option<TlsConfig>,
    info: ClientInfo,
    internal_config: Option<InternalConfig>,
    v2_config: Option<ServerConfigV2>,
    web_config: WebConfig,
    stop_rx: oneshot::Receiver<()>,
) -> anyhow::Result<ServerHandle> {
    // Installed before returning, so that a client built right after (which
    // skips the install when a provider exists) does not race the accept task.
    let _ = rustls::crypto::ring::default_provider().install_default();

    let ipv4_socket_addr = SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), port);
    let info = Arc::new(Mutex::new(info));
    let state = AppState::new(info.clone(), internal_config, v2_config, web_config);

    let ipv4_listener = tokio::net::TcpListener::bind(ipv4_socket_addr).await?;
    // With port 0, the IPv6 listener must reuse the port the IPv4 listener got.
    let bound_port = ipv4_listener.local_addr()?.port();
    let ipv6_socket_addr = SocketAddr::new(Ipv6Addr::UNSPECIFIED.into(), bound_port);
    let ipv6_listener = match bind_ipv6_only(ipv6_socket_addr) {
        Ok(listener) => Some(listener),
        Err(err) => {
            tracing::warn!("Failed to start server on {}: {err:#}", ipv6_socket_addr);
            None
        }
    };
    let ipv6_bound = ipv6_listener.is_some();

    let cancel = CancellationToken::new();
    let connections = TaskTracker::new();

    let task = tokio::spawn({
        let state = state.clone();
        let cancel = cancel.clone();
        let connections = connections.clone();
        let v2_event_tx = state.v2.as_ref().map(|v2| v2.event_tx.clone());
        async move {
            tokio::select! {
                result = start_server_with_listener(ipv4_listener, tls_config.clone(), state.clone(), cancel.clone(), connections.clone()) => {
                    if let Err(err) = result {
                        tracing::error!("Server listener failed on {}: {err:#}", ipv4_socket_addr);
                        // Tell the application, so it can restart the server.
                        // `try_send` because this task must reach its end even
                        // when nobody consumes events anymore, so that
                        // `wait_stopped` cannot hang.
                        if let Some(event_tx) = v2_event_tx {
                            let _ = event_tx.try_send(ServerEventV2::ListenerFailed {
                                error: format!("{err:#}"),
                            });
                        }
                    }
                    tracing::info!("Server stopped on: {}", ipv4_socket_addr);
                }
                _ = async {
                    if let Some(listener) = ipv6_listener {
                        if let Err(err) = start_server_with_listener(listener, tls_config, state, cancel.clone(), connections.clone()).await {
                            tracing::error!("IPv6 server listener failed on {}: {err:#}", ipv6_socket_addr);
                        }
                    }

                    // Keep the future running forever, so we continue using "ipv4 only" even if ipv6 fails.
                    tokio::time::sleep(std::time::Duration::from_secs(u64::MAX)).await;
                } => {}
                _ = stop_rx => {}
            }

            // Hard-drop connections that are still being served, so that no
            // client keeps talking to the stopped server.
            cancel.cancel();
            connections.close();
            connections.wait().await;
        }
    });

    Ok(ServerHandle {
        v2: state.v2.clone(),
        port: bound_port,
        ipv6_bound,
        bound_addresses: Vec::new(),
        task: Mutex::new(Some(task)),
    })
}

/// Starts the native v2.2 receiver on explicitly selected IPv4 addresses.
///
/// No wildcard, web sharing, internal control or v3 routes are enabled. TLS and
/// checksum verification are mandatory. The same session state serves every
/// selected interface. Loopback is allowed for integration tests; the desktop
/// service selects its permitted LAN interfaces before calling this API.
pub async fn start_bound_v2(
    mut addresses: Vec<Ipv4Addr>,
    port: u16,
    tls_config: TlsConfig,
    info: ClientInfo,
    v2_config: ServerConfigV2,
    limits: TransferLimits,
    stop_rx: oneshot::Receiver<()>,
) -> anyhow::Result<ServerHandle> {
    limits.validate()?;
    anyhow::ensure!(!addresses.is_empty(), "no interface selected");
    anyhow::ensure!(addresses.len() <= 16, "too many selected interfaces");
    anyhow::ensure!(addresses.iter().all(|address|
        !address.is_unspecified() && !address.is_multicast() && !address.is_broadcast()),
        "a concrete unicast IPv4 address is required");
    anyhow::ensure!(v2_config.verify_checksums, "checksum verification is mandatory");
    anyhow::ensure!(v2_config.pin.as_ref().is_none_or(|pin| pin.len() <= 128 && !pin.contains('\0')),
        "PIN exceeds the metadata limit");
    anyhow::ensure!(v2_config.event_tx.max_capacity() <= limits.max_event_queue,
        "event channel exceeds its capacity budget");
    anyhow::ensure!(v2_config.event_tx.max_capacity() >= 3,
        "event channel needs request, terminal and listener-failure slots");
    // This slot cannot be consumed by discovery/progress noise. A listener
    // failure remains deliverable even if the consumer temporarily stalls.
    let listener_failure = v2_config.event_tx.clone().try_reserve_owned()
        .map_err(|_| anyhow::anyhow!("event consumer is unavailable"))?;
    addresses.sort();
    addresses.dedup();
    let _ = rustls::crypto::ring::default_provider().install_default();
    // Fail synchronously on an invalid identity, before reporting a listener.
    create_tls_config(&tls_config, true)?;

    let mut state = AppState::new(Arc::new(Mutex::new(info)), None,
        Some(v2_config), WebConfig::default());
    if limits.concurrent_uploads == 1 {
        state.v2.as_ref().unwrap().uploads.forget_permits(1);
    }
    state.strict = Some(Arc::new(limits));

    let mut bound_port = port;
    let mut listeners = Vec::with_capacity(addresses.len());
    let mut bound_addresses = Vec::with_capacity(addresses.len());
    for address in addresses {
        let listener = tokio::net::TcpListener::bind(SocketAddr::new(address.into(), bound_port)).await?;
        let address = listener.local_addr()?;
        bound_port = address.port();
        bound_addresses.push(address);
        listeners.push(listener);
    }

    let task = tokio::spawn({
        let state = state.clone();
        async move {
            let cancel = CancellationToken::new();
            let connections = TaskTracker::new();
            let mut accept_tasks = tokio::task::JoinSet::new();
            for listener in listeners {
                accept_tasks.spawn(start_server_with_listener(listener, Some(tls_config.clone()),
                    state.clone(), cancel.clone(), connections.clone()));
            }
            tokio::select! {
                _ = stop_rx => {},
                result = accept_tasks.join_next() => {
                    let error = match result {
                        Some(Ok(Err(error))) => format!("{error:#}"),
                        Some(Err(error)) => format!("{error:#}"),
                        _ => "listener stopped unexpectedly".to_string(),
                    };
                    listener_failure.send(ServerEventV2::ListenerFailed { error });
                }
            }
            // No selected listener may continue serving after another failed.
            cancel.cancel();
            if let Some(v2) = &state.v2 {
                let mut slot = v2.session.lock().unwrap();
                match slot.take() {
                    Some(SessionStateV2::Active(session)) => {
                        session.cancel.cancel();
                        if let Some(terminal) = session.terminal {
                            terminal.send(ServerEventV2::SessionEnd { session_id: session.session_id,
                                reason: v2::SessionEndReasonV2::Cancelled });
                        }
                    }
                    Some(SessionStateV2::Pending(session)) => {
                        session.cancel.cancel();
                        if let Some(terminal) = session.terminal {
                            terminal.send(ServerEventV2::PrepareUploadAborted { session_id: session.session_id });
                        }
                    }
                    None => {},
                }
            }
            accept_tasks.abort_all();
            while accept_tasks.join_next().await.is_some() {}
            connections.close();
            connections.wait().await;
        }
    });
    Ok(ServerHandle { v2: state.v2, port: bound_port, ipv6_bound: false,
        bound_addresses, task: Mutex::new(Some(task)) })
}

/// Binds an IPv6 listener with `IPV6_V6ONLY` enabled.
///
/// Without this flag, some systems (e.g. macOS) bind IPv6 wildcard sockets in
/// dual-stack mode, which conflicts with the separate IPv4 listener on the same port.
fn bind_ipv6_only(socket_addr: SocketAddr) -> anyhow::Result<tokio::net::TcpListener> {
    let socket = socket2::Socket::new(
        socket2::Domain::IPV6,
        socket2::Type::STREAM,
        Some(socket2::Protocol::TCP),
    )?;
    socket.set_only_v6(true)?;
    #[cfg(not(windows))]
    socket.set_reuse_address(true)?;
    socket.set_nonblocking(true)?;
    socket.bind(&socket_addr.into())?;
    socket.listen(1024)?;
    Ok(tokio::net::TcpListener::from_std(socket.into())?)
}

#[derive(Clone, Debug)]
pub struct TlsConfig {
    pub cert: String,
    pub private_key: String,
}

/// How long the accept loop waits after a failure that would repeat
/// immediately, doubling up to [`ACCEPT_BACKOFF_MAX`].
const ACCEPT_BACKOFF_MIN: std::time::Duration = std::time::Duration::from_millis(50);
const ACCEPT_BACKOFF_MAX: std::time::Duration = std::time::Duration::from_secs(1);

/// How many times in a row accepting may fail with a per-connection error
/// before the listener itself is considered broken. A healthy listener
/// interleaves such errors with successful accepts; only a dead one (e.g. a
/// socket the OS invalidated during app suspension, whose exact error code is
/// OS-specific) produces them in an endless, immediate sequence.
const ACCEPT_FAILURE_LIMIT: u32 = 100;

/// Detects peers that vanished without closing their connections (e.g. left
/// the network mid-transfer), which would otherwise count towards the
/// connection limits forever.
const TCP_KEEPALIVE: socket2::TcpKeepalive = socket2::TcpKeepalive::new()
    .with_time(std::time::Duration::from_secs(60))
    .with_interval(std::time::Duration::from_secs(10));

/// Whether the failed accept concerned only the connection being accepted, so
/// the next one can be attempted right away.
fn is_transient_accept_error(err: &std::io::Error) -> bool {
    matches!(
        err.kind(),
        std::io::ErrorKind::ConnectionAborted
            | std::io::ErrorKind::ConnectionReset
            | std::io::ErrorKind::Interrupted
    )
}

/// Whether the failed accept means the process momentarily ran out of
/// resources (a subnet scan opens a few hundred sockets), which resolves once
/// they are freed again.
fn is_resource_accept_error(err: &std::io::Error) -> bool {
    if err.kind() == std::io::ErrorKind::OutOfMemory {
        return true;
    }
    #[cfg(unix)]
    return matches!(
        err.raw_os_error(),
        Some(libc::EMFILE | libc::ENFILE | libc::ENOBUFS | libc::ENOMEM)
    );
    #[cfg(windows)]
    return matches!(
        err.raw_os_error(),
        Some(10024 /* WSAEMFILE */ | 10055 /* WSAENOBUFS */)
    );
    #[allow(unreachable_code)]
    false
}

async fn start_server_with_listener(
    incoming: tokio::net::TcpListener,
    tls_config: Option<TlsConfig>,
    app_state: AppState,
    cancel: CancellationToken,
    connections: TaskTracker,
) -> anyhow::Result<()> {
    // Browsers have no client certificate, so presenting one is optional while
    // the web pages are served. A certificate that is presented is still verified.
    let mandatory_client_auth = matches!(app_state.web.share, WebShare::Disabled);

    let tls_acceptor = match tls_config {
        Some(tls_config) => Some(
            create_tls_config(&tls_config, mandatory_client_auth).inspect_err(|err| {
                tracing::error!("failed to create tls config: {err:#}");
            })?,
        ),
        None => None,
    };

    tracing::info!(
        "Started server on {} (TLS: {})",
        incoming.local_addr()?,
        tls_acceptor.is_some()
    );

    let mut accept_backoff = ACCEPT_BACKOFF_MIN;
    let mut accept_failures = 0u32;
    let mut at_connection_limit = false;
    loop {
        let (tcp_stream, remote_addr) = match incoming.accept().await {
            Ok(accepted) => {
                accept_backoff = ACCEPT_BACKOFF_MIN;
                accept_failures = 0;
                // Disable Nagle: it delays small responses (reqwest already does this on the client side).
                let _ = accepted.0.set_nodelay(true);
                let _ = socket2::SockRef::from(&accepted.0).set_tcp_keepalive(&TCP_KEEPALIVE);
                accepted
            }
            // Accepting fails for two kinds of reasons that say nothing about
            // the listener, which must both keep the loop alive: the peer went
            // away before the handshake completed (retried right away, but
            // bounded by [ACCEPT_FAILURE_LIMIT] because a listener the OS
            // invalidated during app suspension may report an OS-specific
            // error that looks per-connection), or the process momentarily ran
            // out of resources (a subnet scan opens a few hundred sockets).
            // Exhaustion would otherwise spin: the pending connection stays in
            // the backlog and fails again immediately, so back off before
            // retrying, without a limit — it says nothing about the listener,
            // however long it lasts.
            //
            // Every other error means the listening socket itself is broken.
            // Retrying that forever would leave the application believing it
            // can still receive, so give up and let the caller report it.
            Err(err) => {
                if is_resource_accept_error(&err) {
                    tracing::warn!("Could not accept a connection: {err:#}");
                    tokio::time::sleep(accept_backoff).await;
                    accept_backoff = (accept_backoff * 2).min(ACCEPT_BACKOFF_MAX);
                    continue;
                }
                accept_failures += 1;
                if is_transient_accept_error(&err) && accept_failures < ACCEPT_FAILURE_LIMIT {
                    tracing::warn!("Could not accept a connection: {err:#}");
                    continue;
                }
                return Err(anyhow::Error::from(err).context(format!(
                    "accepting connections failed {accept_failures} time(s) in a row"
                )));
            }
        };

        // Dropping the stream closes the connection.
        let permit = match app_state.connection_limiter.try_acquire(remote_addr.ip()) {
            Ok(permit) => permit,
            Err(LimitReached::Total) => {
                if !at_connection_limit {
                    tracing::warn!(
                        "Refusing connections: {MAX_CONNECTIONS} connections are being served"
                    );
                    at_connection_limit = true;
                }
                continue;
            }
            // Debug only: the peer decides how often this happens.
            Err(LimitReached::PerIp) => {
                tracing::debug!(
                    "Refusing connection from {}: {MAX_CONNECTIONS_PER_IP} of its connections are being served",
                    remote_addr.ip()
                );
                continue;
            }
        };
        at_connection_limit = false;

        let tls_acceptor = tls_acceptor.clone();
        let app_state = app_state.clone();
        let cancel = cancel.clone();
        connections.spawn(async move {
            let _permit = permit;
            let serve = serve_connection(tcp_stream, remote_addr, tls_acceptor, app_state);
            tokio::select! {
                _ = serve => {}
                // Hard-drop the connection when the server is stopped.
                _ = cancel.cancelled() => {}
            }
        });
    }
}

async fn serve_connection(
    tcp_stream: tokio::net::TcpStream,
    remote_addr: SocketAddr,
    tls_acceptor: Option<tokio_rustls::TlsAcceptor>,
    app_state: AppState,
) {
    let res = match tls_acceptor {
        Some(tls_acceptor) => {
            let tls_stream = match tokio::time::timeout(
                std::time::Duration::from_secs(10), tls_acceptor.accept(tcp_stream)).await {
                Ok(Ok(tls_stream)) => tls_stream,
                Ok(Err(err)) => {
                    tracing::warn!("TLS handshake error: {err:#}");
                    return;
                }
                Err(_) => return,
            };

            let client_info = {
                let (_, server_connection) = tls_stream.get_ref();
                RequestClientInfo {
                    ip: PeerIp::from_remote_addr(&remote_addr),
                    tls: true,
                    // No certificate when client auth is optional (web pages served)
                    // and the client (e.g. a browser) did not present one.
                    cert: server_connection
                        .deref()
                        .deref()
                        .peer_certificates()
                        .and_then(|certs| certs.first().map(|cert| cert.to_vec())),
                }
            };

            serve_http(tls_stream, client_info, app_state).await
        }
        None => {
            let client_info = RequestClientInfo {
                ip: PeerIp::from_remote_addr(&remote_addr),
                tls: false,
                cert: None,
            };
            serve_http(tcp_stream, client_info, app_state).await
        }
    };

    if let Err(err) = res {
        tracing::warn!("Failed to serve connection: {err:#}");
    }
}

/// Serves HTTP/1.1 only: HTTP/2 multiplexes many requests over one connection,
/// so the connection limits would no longer bound the requests of a peer.
async fn serve_http<I>(
    io: I,
    client_info: RequestClientInfo,
    app_state: AppState,
) -> hyper::Result<()>
where
    I: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    hyper::server::conn::http1::Builder::new()
        .timer(TokioTimer::new())
        .header_read_timeout(std::time::Duration::from_secs(10))
        .max_buf_size(HEADER_BYTES)
        .max_headers(HEADER_COUNT)
        .serve_connection(
            TokioIo::new(io),
            hyper::service::service_fn(move |mut req: Request<Incoming>| {
                req.extensions_mut()
                    .insert::<RequestClientInfo>(client_info.clone());
                req.extensions_mut().insert::<AppState>(app_state.clone());
                handle_request(req)
            }),
        )
        .await
}

fn create_tls_config(
    tls_config: &TlsConfig,
    mandatory_client_auth: bool,
) -> anyhow::Result<tokio_rustls::TlsAcceptor> {
    let config = {
        let certs = vec![CertificateDer::from_pem_slice(&tls_config.cert.as_bytes())?];
        let key = PrivateKeyDer::from_pem_slice(&tls_config.private_key.as_bytes())?;

        rustls::ServerConfig::builder()
            .with_client_cert_verifier(Arc::new(CustomClientCertVerifier::try_new(
                &tls_config.cert,
                mandatory_client_auth,
            )?))
            .with_single_cert(certs, key)?
    };
    Ok(tokio_rustls::TlsAcceptor::from(Arc::new(config)))
}

#[derive(Clone, Debug)]
pub struct RequestClientInfo {
    /// The IP address of the client, including the IPv6 scope when present.
    ip: PeerIp,

    /// Whether the connection uses TLS.
    tls: bool,

    /// The client certificate in DER format.
    /// `None` without TLS, or on TLS when the client presented no certificate
    /// (only possible while the web pages are served).
    cert: Option<Vec<u8>>,
}

impl RequestClientInfo {
    /// The fingerprint identifying the client, given the one it `claimed`:
    /// - TLS with a client certificate: the SHA-256 fingerprint (uppercase hex)
    ///   of that certificate, verified during the mTLS handshake.
    /// - Plain HTTP: the claimed fingerprint, as there is nothing to verify it
    ///   against (encryption is turned off).
    /// - TLS without a client certificate (e.g. a browser while the web pages
    ///   are served): `None`, the client is anonymous. Its claimed fingerprint
    ///   must not be trusted, otherwise it could impersonate any device.
    fn sender_fingerprint(&self, claimed: &str) -> Option<String> {
        match (&self.cert, self.tls) {
            (Some(cert), _) => Some(fingerprint_from_cert_der(cert)),
            (None, false) => Some(claimed.to_string()),
            (None, true) => None,
        }
    }

    fn extract_public_key(&self) -> Option<String> {
        match &self.cert {
            Some(cert) => match public_key_from_cert_der(cert) {
                Ok(public_key) => Some(public_key),
                Err(err) => {
                    tracing::warn!("Failed to extract public key from certificate: {err:#}");
                    None
                }
            },
            None => None,
        }
    }

    fn identifier(&self) -> String {
        self.extract_public_key()
            .unwrap_or_else(|| self.ip.to_string())
    }
}

async fn handle_request(req: Request<Incoming>) -> Result<Response<BoxedBody>, hyper::Error> {
    Ok(handle_request_inner(req).await.unwrap_or_else(|err| {
        tracing::error!("Error handling request: {err:?}");
        err.to_response()
    }))
}

async fn handle_request_inner(mut req: Request<Incoming>) -> Result<Response<BoxedBody>, AppError> {
    let Some(state) = req.extensions_mut().remove::<AppState>() else {
        return Err(AppError::Status(StatusCode::INTERNAL_SERVER_ERROR));
    };

    let Some(client_info) = req.extensions_mut().remove::<RequestClientInfo>() else {
        return Err(AppError::Status(StatusCode::INTERNAL_SERVER_ERROR));
    };

    let v2_enabled = state.v2.is_some();

    if state.strict.is_some() && !matches!((req.method(), req.uri().path()),
        (&Method::GET, "/api/localsend/v2/info")
        | (&Method::POST, "/api/localsend/v2/register")
        | (&Method::POST, "/api/localsend/v2/prepare-upload")
        | (&Method::POST, "/api/localsend/v2/upload")
        | (&Method::POST, "/api/localsend/v2/cancel"))
    {
        return Err(AppError::Status(StatusCode::NOT_FOUND));
    }

    match (req.method(), req.uri().path()) {
        (&Method::GET, "/") => Ok(web::index(&state)),
        (&Method::GET, "/i18n.json") => web::i18n(&state),
        (&Method::POST, "/api/localsend/v2/prepare-download") => {
            web::prepare_download(req, state, client_info).await
        }
        (&Method::GET, "/api/localsend/v2/download") => {
            web::download(req, state, client_info).await
        }
        (&Method::POST, "/api/localsend/v2/register") => {
            if !v2_enabled {
                return Err(AppError::Status(StatusCode::NOT_FOUND));
            }

            Ok(v2::register(req.into_body(), state, client_info)
                .await?
                .into_response())
        }
        // Old clients (v1.17 and earlier) probe unknown peers on the v1 route
        (&Method::GET, "/api/localsend/v1/info") | (&Method::GET, "/api/localsend/v2/info") => {
            if !v2_enabled {
                return Err(AppError::Status(StatusCode::NOT_FOUND));
            }

            Ok(v2::info(state).await?.into_response())
        }
        (&Method::POST, "/api/localsend/v2/prepare-upload") => {
            if !v2_enabled {
                return Err(AppError::Status(StatusCode::NOT_FOUND));
            }

            v2::prepare_upload(req, state, client_info).await
        }
        (&Method::POST, "/api/localsend/v2/upload") => {
            if !v2_enabled {
                return Err(AppError::Status(StatusCode::NOT_FOUND));
            }

            v2::upload(req, state, client_info).await
        }
        (&Method::POST, "/api/localsend/v2/cancel") => {
            if !v2_enabled {
                return Err(AppError::Status(StatusCode::NOT_FOUND));
            }

            v2::cancel(req, state, client_info).await
        }
        // The versioned path is retained for compatibility, but this endpoint is internal.
        (&Method::POST, "/api/localsend/v2/show") => internal::show(req, state).await,
        (&Method::POST, "/api/localsend/v3/nonce") => {
            Ok(v3::nonce_exchange(req.into_body(), state, client_info)
                .await?
                .into_response())
        }
        (&Method::POST, "/api/localsend/v3/register") => {
            Ok(v3::register(req.into_body(), state, client_info)
                .await?
                .into_response())
        }
        _ => {
            let mut res = Response::new(response::empty_body());
            *res.status_mut() = StatusCode::NOT_FOUND;
            Ok(res)
        }
    }
}
