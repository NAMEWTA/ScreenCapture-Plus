// libportal is linked, not reimplemented. Its pinned downstream patch supplies
// reliable request completion and a cancellable async OpenPipeWireRemote API.
// Include GLib before Qt so the latter's optional keyword macros cannot alter it.
#include <libportal/portal.h>

#include "platform/linux/portal/PortalCapture.h"
#include "platform/linux/portal/PortalSupport.h"
#include "platform/linux/portal/PortalParent.h"

#include <QAbstractEventDispatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace HuhuCapture::Platform::Linux {
using namespace PortalSupport;

struct PortalCapture::Private : std::enable_shared_from_this<Private> {
    struct Operation {
        enum class Kind { Screenshot, ScreenCast, PipeWireRemote };
        quint64 id{};
        Kind kind{};
        QPointer<QWindow> parent;
        QMetaObject::Connection parentDestroyed;
        GCancellable* cancellable{g_cancellable_new()};
        std::shared_ptr<PortalParent> portalParent;
        XdpSession* session{};
        gulong sessionClosedHandler{};
        PortalFailure cancellation;
        QVector<PortalStream> streams;
        QString restoreToken;
        bool interactiveScreenshot{true};
        bool finished{};

        ~Operation() {
            QObject::disconnect(parentDestroyed);
            if (session) {
                g_clear_signal_handler(&sessionClosedHandler, session);
                xdp_session_close(session);
                g_object_unref(session);
            }
            g_clear_object(&cancellable);
        }
    };
    struct Callback {
        std::shared_ptr<Private> state;
        explicit Callback(std::shared_ptr<Private> value) : state(std::move(value)) { ++state->callbacks; }
        ~Callback() { --state->callbacks; state->checkDrained(); }
    };
    struct Call : Callback {
        std::shared_ptr<Operation> operation;
        Call(std::shared_ptr<Private> state, std::shared_ptr<Operation> pending)
            : Callback(std::move(state)), operation(std::move(pending)) {}
    };
    struct Probe : Callback {
        bool screenCast;
        Probe(std::shared_ptr<Private> state, bool cast) : Callback(std::move(state)), screenCast(cast) {}
    };

    QPointer<PortalCapture> owner;
    XdpPortal* portal{};
    GDBusConnection* bus{};
    GCancellable* initialization{g_cancellable_new()};
    guint nameWatch{};
    gulong busClosedHandler{};
    QByteArray busName;
    PortalCapabilities capabilities;
    std::shared_ptr<Operation> operation;
    XdpSession* activeSession{};
    gulong activeClosedHandler{};
    int pipeWireFd{-1};
    quint64 nextId{1};
    int probesPending{};
    bool initializing{};
    bool ready{};
    bool stopped{};
    bool draining{}, shutdownQueued{};
    unsigned callbacks{}, deliveries{};

    explicit Private(PortalCapture* value) : owner(value) {
        busName = qgetenv("LIBPORTAL_PORTAL_BUS_NAME");
        if (busName.isEmpty()) busName = "org.freedesktop.portal.Desktop";
    }
    ~Private() {
        if (pipeWireFd >= 0) ::close(pipeWireFd);
        if (nameWatch) g_bus_unwatch_name(nameWatch);
        if (bus) g_clear_signal_handler(&busClosedHandler, bus);
        g_clear_object(&initialization);
        g_clear_object(&bus);
        g_clear_object(&portal);
    }

    bool onOwnerThread() const {
        return owner && QThread::currentThread() == owner->thread();
    }
    PortalFailure checkContext() {
        return PortalSupport::checkContext(owner, capabilities.qtPlatform, capabilities.dispatcher);
    }

    void cancel(std::shared_ptr<Operation> pending, PortalFailure reason) {
        if (!pending || pending->finished || pending->cancellation) return;
        pending->cancellation = std::move(reason);
        g_cancellable_cancel(pending->cancellable);
    }

    void closeActive(PortalFailure reason = {}) {
        auto* session = std::exchange(activeSession, nullptr);
        if (!session) return;
        if (operation && operation->kind == Operation::Kind::PipeWireRemote)
            cancel(operation, reason ? reason : failure(Code::Unavailable, QStringLiteral("The ScreenCast session was closed")));
        g_clear_signal_handler(&activeClosedHandler, session);
        if (pipeWireFd >= 0) ::close(std::exchange(pipeWireFd, -1));
        xdp_session_close(session);
        g_object_unref(session);
        if (owner && !stopped) Q_EMIT owner->screenCastClosed(reason);
    }

    void releasePortal() {
        if (!portal) return;
        // Screenshot/Create/Start cancellation can complete its public GTask
        // before a late D-Bus method reply drops native task/session references.
        // Also retain this gate when initialization replaces an earlier portal;
        // shutdown must drain its late replies as well as the current instance.
        // Every adopted portal operation runs on the owner's GLib context.
        auto* completion = new Callback(shared_from_this());
        auto* released = std::exchange(portal, nullptr);
        g_object_weak_ref(G_OBJECT(released), +[](gpointer data, GObject*) {
            const std::unique_ptr<Callback> completion(static_cast<Callback*>(data));
            Q_ASSERT(!completion->state->owner || completion->state->onOwnerThread());
        }, completion);
        g_object_unref(released);
    }

    void checkDrained() {
        if (!draining || stopped || !owner || shutdownQueued || callbacks || deliveries || operation || activeSession || initializing) return;
        if (nameWatch) g_bus_unwatch_name(std::exchange(nameWatch, 0));
        if (bus) g_clear_signal_handler(&busClosedHandler, bus);
        if (portal) { releasePortal(); return; }
        shutdownQueued = true;
        g_clear_object(&bus);
        const auto state = shared_from_this();
        // GObject weak notification precedes finalize. Queue the acknowledgement
        // so it runs after that native destruction/callback stack has returned;
        // the shared session-bus connection is deliberately not a global gate.
        QTimer::singleShot(0, owner, [state] {
            if (!state->owner || state->stopped) return;
            if (state->callbacks || state->deliveries || state->operation || state->activeSession || state->initializing) {
                state->shutdownQueued = false;
                state->checkDrained();
                return;
            }
            Q_EMIT state->owner->shutdownFinished();
        });
    }

    void requestShutdown() {
        if (draining || stopped) return;
        draining = true; ready = false;
        g_cancellable_cancel(initialization);
        cancel(operation, failure(Code::Cancelled, QStringLiteral("Portal capture is shutting down")));
        closeActive();
        if (nameWatch) g_bus_unwatch_name(std::exchange(nameWatch, 0));
        if (bus) g_clear_signal_handler(&busClosedHandler, bus);
        checkDrained();
    }

    void destroy() {
        stopped = true;
        g_cancellable_cancel(initialization);
        cancel(operation, failure(Code::Cancelled, QStringLiteral("Portal owner destroyed")));
        closeActive();
        if (nameWatch) g_bus_unwatch_name(std::exchange(nameWatch, 0));
        if (bus) g_clear_signal_handler(&busClosedHandler, bus);
        owner = nullptr;
    }

    void finish(const std::shared_ptr<Operation>& pending, PortalFailure error, QUrl uri = {}) {
        if (pending->finished) return;
        if (pending->cancellation) error = pending->cancellation;
        pending->finished = true;
        QObject::disconnect(pending->parentDestroyed);
        pending->portalParent.reset();
        if (pending->session) {
            g_clear_signal_handler(&pending->sessionClosedHandler, pending->session);
            xdp_session_close(pending->session);
            g_clear_object(&pending->session);
        }
        if (operation == pending) operation.reset();
        if (!owner || stopped) return;
        ++deliveries;
        if (pending->kind == Operation::Kind::Screenshot)
            Q_EMIT owner->screenshotFinished(pending->id, uri, error);
        else if (pending->kind == Operation::Kind::ScreenCast)
            Q_EMIT owner->screenCastFinished(pending->id, pending->streams, pending->restoreToken, error);
        else
            Q_EMIT owner->pipeWireRemoteOpened(pending->id, error);
        --deliveries;
        checkDrained();
    }

    quint64 reject(Operation::Kind kind, PortalFailure error) {
        if (draining || stopped) return 0;
        const auto id = nextId++;
        const auto state = shared_from_this();
        ++deliveries;
        QTimer::singleShot(0, owner, [state, id, kind, error = std::move(error)] {
            if (state->owner && !state->stopped) {
                if (kind == Operation::Kind::Screenshot)
                    Q_EMIT state->owner->screenshotFinished(id, {}, error);
                else if (kind == Operation::Kind::ScreenCast)
                    Q_EMIT state->owner->screenCastFinished(id, {}, {}, error);
                else
                    Q_EMIT state->owner->pipeWireRemoteOpened(id, error);
            }
            --state->deliveries;
            state->checkDrained();
        });
        return id;
    }

    PortalFailure canRequest(Operation::Kind kind, QWindow* parent) {
        if (auto error = checkContext()) return error;
        if (!ready || stopped || draining) return failure(Code::Unavailable, QStringLiteral("Portal initialization has not succeeded or is shutting down"));
        if (operation || activeSession) return failure(Code::Busy, QStringLiteral("A Portal request or screencast is already active"));
        if ((!parent && kind == Operation::Kind::ScreenCast) || (parent && parent->thread() != owner->thread()))
            return failure(Code::ParentDestroyed, QStringLiteral("A live parent window on the GUI thread is required"));
        if (parent && (!parent->isVisible() || !parent->handle()))
            return failure(Code::InvalidState, QStringLiteral("The Portal parent window must have a visible native surface"));
        const auto& feature = kind == Operation::Kind::Screenshot ? capabilities.screenshot : capabilities.screenCast;
        if (!feature.available())
            return feature.failure ? feature.failure : failure(Code::Unsupported, QStringLiteral("Portal interface is not available"));
        return {};
    }

    std::shared_ptr<Operation> begin(Operation::Kind kind, QWindow* parent) {
        auto pending = std::make_shared<Operation>();
        pending->id = nextId++;
        pending->kind = kind;
        pending->parent = parent;
        std::weak_ptr<Private> weakState = shared_from_this();
        std::weak_ptr<Operation> weakOperation = pending;
        if (parent) pending->parentDestroyed = QObject::connect(parent, &QObject::destroyed, owner, [weakState, weakOperation] {
            if (auto state = weakState.lock()) {
                const Callback delivery(state);
                state->cancel(weakOperation.lock(), failure(Code::ParentDestroyed, QStringLiteral("Portal parent window was destroyed")));
            }
        });
        operation = pending;
        return pending;
    }

    void prepareParent(const std::shared_ptr<Operation>& pending) {
        pending->portalParent = PortalParent::create();
        std::weak_ptr<Private> weakState = shared_from_this();
        std::weak_ptr<Operation> weakPending = pending;
        QObject::connect(pending->portalParent.get(), &PortalParent::prepared, pending->portalParent.get(),
            [weakState, weakPending](const PortalFailure& error) {
                const auto state = weakState.lock();
                if (!state) return;
                const Callback delivery(state);
                const auto operation = weakPending.lock();
                if (!operation || operation->finished) return;
                if (error || operation->cancellation || state->stopped) { state->finish(operation, error); return; }
                auto* parent = operation->portalParent->nativeParent();
                if (!parent) {
                    state->finish(operation, failure(Code::ParentDestroyed, QStringLiteral("The exported Portal parent is no longer valid")));
                    return;
                }
                if (operation->kind == Operation::Kind::Screenshot)
                    xdp_portal_take_screenshot(state->portal, parent,
                        operation->interactiveScreenshot ? XDP_SCREENSHOT_FLAG_INTERACTIVE : XDP_SCREENSHOT_FLAG_NONE,
                        operation->cancellable, screenshotReady, new Call{state, operation});
                else
                    xdp_session_start(operation->session, parent, operation->cancellable,
                                      sessionStarted, new Call{state, operation});
            });
        QObject::connect(pending->portalParent.get(), &PortalParent::invalidated, pending->portalParent.get(),
            [weakState, weakPending](const PortalFailure& error) {
                if (const auto state = weakState.lock()) {
                    const Callback delivery(state);
                    state->cancel(weakPending.lock(), error);
                }
            });
        pending->portalParent->prepare(pending->parent, pending->cancellable);
    }

    static void initializedPortal(GObject*, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Callback> holder(static_cast<Callback*>(data));
        const auto state = holder->state;
        g_autoptr(GError) nativeError = nullptr;
        std::unique_ptr<InitializedPortal> initialized(static_cast<InitializedPortal*>(g_task_propagate_pointer(G_TASK(result), &nativeError)));
        if (state->stopped) return;
        if (state->draining) {
            state->initializing = false;
            if (state->owner) Q_EMIT state->owner->initialized(state->capabilities,
                failure(Code::Cancelled, QStringLiteral("Portal initialization was cancelled during shutdown")));
            return;
        }
        if (!initialized) {
            state->initializing = false;
            auto error = fromError(nativeError);
            if (error.code != Code::Cancelled) error.code = Code::Unavailable;
            if (state->owner) Q_EMIT state->owner->initialized(state->capabilities, error);
            return;
        }
        state->portal = std::exchange(initialized->portal, nullptr);
        state->bus = std::exchange(initialized->bus, nullptr);
        state->busClosedHandler = g_signal_connect(state->bus, "closed", G_CALLBACK(busClosed), state.get());
        state->probesPending = 2;
        for (const auto screenCast : {false, true}) {
            g_dbus_connection_call(state->bus, state->busName.constData(), "/org/freedesktop/portal/desktop",
                "org.freedesktop.DBus.Properties", "GetAll",
                g_variant_new("(s)", screenCast ? "org.freedesktop.portal.ScreenCast" : "org.freedesktop.portal.Screenshot"),
                G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, state->initialization,
                propertiesReady, new Probe{state, screenCast});
        }
    }

    static void propertiesReady(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Probe> probe(static_cast<Probe*>(data));
        const auto state = probe->state;
        g_autoptr(GError) error = nullptr;
        g_autoptr(GVariant) returned = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (state->stopped) return;
        if (state->draining) {
            if (--state->probesPending == 0) {
                state->initializing = false;
                if (state->owner) Q_EMIT state->owner->initialized(state->capabilities,
                    failure(Code::Cancelled, QStringLiteral("Portal initialization was cancelled during shutdown")));
            }
            return;
        }
        auto& feature = probe->screenCast ? state->capabilities.screenCast : state->capabilities.screenshot;
        if (!returned) feature.failure = fromError(error);
        else {
            g_autoptr(GVariant) values = g_variant_get_child_value(returned, 0);
            if (!g_variant_lookup(values, "version", "u", &feature.version) || feature.version == 0)
                feature.failure = failure(Code::Unsupported, QStringLiteral("Portal did not advertise a usable interface version"));
            if (probe->screenCast) {
                g_variant_lookup(values, "AvailableSourceTypes", "u", &state->capabilities.sourceTypes);
                if (!g_variant_lookup(values, "AvailableCursorModes", "u", &state->capabilities.cursorModes)) {
                    // Cursor control first appeared in ScreenCast v2. Earlier
                    // interfaces cannot honor an explicit cursor request.
                    state->capabilities.cursorModes = 0;
                }
            }
        }
        if (--state->probesPending != 0) return;
        if (g_dbus_connection_is_closed(state->bus)) {
            const auto unavailable = failure(Code::Unavailable, QStringLiteral("The session bus disconnected during initialization"));
            state->capabilities.screenshot.failure = unavailable;
            state->capabilities.screenCast.failure = unavailable;
        }
        state->initializing = false;
        state->ready = state->capabilities.screenshot.available() || state->capabilities.screenCast.available();
        PortalFailure overall;
        if (!state->ready) overall = state->capabilities.screenCast.failure;
        if (state->ready) {
            state->nameWatch = g_bus_watch_name_on_connection(state->bus, state->busName.constData(),
                G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr, portalVanished, state.get(), nullptr);
        }
        if (state->owner) Q_EMIT state->owner->initialized(state->capabilities, overall);
    }

    void unavailable(PortalFailure reason) {
        ready = false;
        capabilities.screenshot.failure = reason;
        capabilities.screenCast.failure = reason;
        cancel(operation, reason);
        closeActive(std::move(reason));
        if (owner && !stopped) Q_EMIT owner->capabilitiesChanged(capabilities);
    }
    static void portalVanished(GDBusConnection*, const gchar*, gpointer data) {
        const Callback delivery(static_cast<Private*>(data)->shared_from_this());
        const auto& state = delivery.state;
        if (!state->stopped)
            state->unavailable(failure(Code::Unavailable, QStringLiteral("The desktop Portal service disappeared")));
    }
    static void busClosed(GDBusConnection*, gboolean, GError* error, gpointer data) {
        const Callback delivery(static_cast<Private*>(data)->shared_from_this());
        const auto& state = delivery.state;
        if (state->stopped) return;
        Q_ASSERT(state->onOwnerThread());
        auto reason = error ? fromError(error) : failure(Code::Unavailable, QStringLiteral("The session bus disconnected"));
        reason.code = Code::Unavailable;
        state->unavailable(std::move(reason));
    }

    static void screenshotReady(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Call> call(static_cast<Call*>(data));
        g_autoptr(GError) nativeError = nullptr;
        g_autofree char* uri = xdp_portal_take_screenshot_finish(XDP_PORTAL(source), result, &nativeError);
        if (!uri) { call->state->finish(call->operation, fromError(nativeError)); return; }
        const auto parsed = QUrl::fromEncoded(uri, QUrl::StrictMode);
        if (!parsed.isValid() || !parsed.isLocalFile() || parsed.toLocalFile().isEmpty()) {
            call->state->finish(call->operation, failure(Code::Failed, QStringLiteral("Screenshot Portal returned an invalid file URI")));
            return;
        }
        call->state->finish(call->operation, {}, parsed);
    }

    static void sessionCreated(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Call> call(static_cast<Call*>(data));
        const auto& state = call->state;
        const auto& pending = call->operation;
        g_autoptr(GError) error = nullptr;
        pending->session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(source), result, &error);
        if (!pending->session) { state->finish(pending, fromError(error)); return; }
        if (state->stopped || pending->cancellation || !pending->parent) {
            state->finish(pending, failure(Code::ParentDestroyed, QStringLiteral("Portal parent is no longer available")));
            return;
        }
        if (xdp_session_get_session_state(pending->session) == XDP_SESSION_CLOSED) {
            state->finish(pending, failure(Code::Unavailable, QStringLiteral("ScreenCast session closed before parent export")));
            return;
        }
        pending->sessionClosedHandler = g_signal_connect(pending->session, "closed", G_CALLBACK(pendingSessionClosed), pending.get());
        state->prepareParent(pending);
    }

    static void sessionStarted(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Call> call(static_cast<Call*>(data));
        const auto& state = call->state;
        const auto& pending = call->operation;
        g_autoptr(GError) error = nullptr;
        const bool started = xdp_session_start_finish(XDP_SESSION(source), result, &error);
        if (!started) { state->finish(pending, fromError(error)); return; }
        if (state->stopped || pending->cancellation) { state->finish(pending, {}); return; }
        auto* streams = xdp_session_get_streams(pending->session);
        if (!streams || !g_variant_is_of_type(streams, G_VARIANT_TYPE("a(ua{sv})"))
            || g_variant_n_children(streams) == 0) {
            state->finish(pending, failure(Code::Failed, QStringLiteral("ScreenCast Portal returned no streams")));
            return;
        }
        QSet<quint32> nodes;
        QSet<quint64> serials;
        GVariantIter iterator;
        g_variant_iter_init(&iterator, streams);
        guint32 node;
        GVariant* properties;
        while (g_variant_iter_next(&iterator, "(u@a{sv})", &node, &properties)) {
            g_autoptr(GVariant) values = properties;
            if (nodes.contains(node)) {
                state->finish(pending, failure(Code::Failed, QStringLiteral("ScreenCast Portal returned duplicate node IDs")));
                return;
            }
            nodes.insert(node);
            PortalStream stream;
            stream.nodeId = node;
            g_autoptr(GVariant) serial = g_variant_lookup_value(values, "pipewire-serial", nullptr);
            if (serial) {
                if (!g_variant_is_of_type(serial, G_VARIANT_TYPE_UINT64) || g_variant_get_uint64(serial) == 0) {
                    state->finish(pending, failure(Code::Failed, QStringLiteral("ScreenCast Portal returned an invalid PipeWire serial")));
                    return;
                }
                const auto value = g_variant_get_uint64(serial);
                if (serials.contains(value)) {
                    state->finish(pending, failure(Code::Failed, QStringLiteral("ScreenCast Portal returned duplicate PipeWire serials")));
                    return;
                }
                serials.insert(value);
                stream.pipeWireSerial = value;
            }
            gint32 x, y;
            if (g_variant_lookup(values, "position", "(ii)", &x, &y)) stream.position = QPoint(x, y);
            if (g_variant_lookup(values, "size", "(ii)", &x, &y)) {
                if (x <= 0 || y <= 0) {
                    state->finish(pending, failure(Code::Failed, QStringLiteral("ScreenCast Portal returned an invalid logical size")));
                    return;
                }
                stream.size = QSize(x, y);
            }
            g_variant_lookup(values, "source_type", "u", &stream.sourceType);
            const char* id = nullptr;
            if (g_variant_lookup(values, "id", "&s", &id)) stream.id = QString::fromUtf8(id);
            pending->streams.push_back(std::move(stream));
        }
        g_autofree char* restore = xdp_session_get_restore_token(pending->session);
        if (restore) pending->restoreToken = QString::fromUtf8(restore);
        xdp_session_open_pipewire_remote_async(pending->session, pending->cancellable,
                                               pipeWireReady, new Call{state, pending});
    }

    static void pendingSessionClosed(XdpSession*, gpointer data) {
        auto* pending = static_cast<Operation*>(data);
        if (!pending->cancellation)
            pending->cancellation = failure(Code::Unavailable, QStringLiteral("ScreenCast session closed before its remote was ready"));
        g_autoptr(GCancellable) cancellation = G_CANCELLABLE(g_object_ref(pending->cancellable));
        g_cancellable_cancel(cancellation);
    }

    static void pipeWireReady(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Call> call(static_cast<Call*>(data));
        const auto& state = call->state;
        const auto& pending = call->operation;
        g_autoptr(GError) error = nullptr;
        const auto fd = xdp_session_open_pipewire_remote_finish(XDP_SESSION(source), result, &error);
        if (fd < 0) { state->finish(pending, fromError(error)); return; }
        if (state->stopped || pending->cancellation
            || xdp_session_get_session_state(pending->session) != XDP_SESSION_ACTIVE) {
            ::close(fd);
            state->finish(pending, failure(Code::Unavailable, QStringLiteral("ScreenCast session is no longer active")));
            return;
        }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            const auto nativeCode = errno;
            ::close(fd);
            auto failed = failure(Code::Failed, QStringLiteral("Could not set close-on-exec on PipeWire remote"));
            failed.domain = QStringLiteral("errno");
            failed.nativeCode = nativeCode;
            state->finish(pending, failed);
            return;
        }
        g_clear_signal_handler(&pending->sessionClosedHandler, pending->session);
        state->activeSession = std::exchange(pending->session, nullptr);
        state->pipeWireFd = fd;
        state->activeClosedHandler = g_signal_connect(state->activeSession, "closed", G_CALLBACK(activeSessionClosed), state.get());
        // The authorization parent no longer owns an active capture session.
        // Closing its window must not cancel a recording using the granted FD.
        state->finish(pending, {});
    }

    static void activeSessionClosed(XdpSession*, gpointer data) {
        const Callback delivery(static_cast<Private*>(data)->shared_from_this());
        const auto& state = delivery.state;
        state->closeActive(failure(Code::Unavailable, QStringLiteral("The compositor closed the ScreenCast session")));
    }

    static void remoteReopened(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Call> call(static_cast<Call*>(data));
        const auto& state = call->state;
        const auto& pending = call->operation;
        g_autoptr(GError) error = nullptr;
        const auto fd = xdp_session_open_pipewire_remote_finish(XDP_SESSION(source), result, &error);
        if (fd < 0) { state->finish(pending, fromError(error)); return; }
        if (state->stopped || state->draining || pending->cancellation ||
            state->activeSession != XDP_SESSION(source) ||
            xdp_session_get_session_state(XDP_SESSION(source)) != XDP_SESSION_ACTIVE) {
            ::close(fd);
            state->finish(pending, failure(Code::Unavailable, QStringLiteral("The ScreenCast session closed while opening its new remote")));
            return;
        }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            const auto nativeCode = errno;
            ::close(fd);
            auto failed = failure(Code::Failed, QStringLiteral("Could not set close-on-exec on the new PipeWire remote"));
            failed.domain = QStringLiteral("errno"); failed.nativeCode = nativeCode;
            state->finish(pending, failed);
            return;
        }
        // The active session remains owned by this controller. A successful
        // callback transfers only the new connection, never authorization or
        // the prior connection's buffers. GTask holds source through finish.
        state->pipeWireFd = fd;
        state->finish(pending, {});
    }
};

PortalCapture::PortalCapture(QObject* parent) : QObject(parent), d(std::make_shared<Private>(this)) {}
PortalCapture::~PortalCapture() { Q_ASSERT(d->onOwnerThread()); d->destroy(); }

void PortalCapture::initialize()
{
    Q_ASSERT(d->onOwnerThread());
    if (auto error = d->checkContext()) { Q_EMIT initialized(d->capabilities, error); return; }
    if (d->initializing || d->ready || d->operation || d->activeSession || d->draining || d->stopped) {
        Q_EMIT initialized(d->capabilities, failure(Code::InvalidState, QStringLiteral("Portal initialization was already requested")));
        return;
    }
    if (d->nameWatch) g_bus_unwatch_name(std::exchange(d->nameWatch, 0));
    if (d->bus) g_clear_signal_handler(&d->busClosedHandler, d->bus);
    d->releasePortal();
    g_clear_object(&d->bus);
    d->capabilities.screenshot = {};
    d->capabilities.screenCast = {};
    d->capabilities.sourceTypes = 0;
    d->capabilities.cursorModes = 0;
    d->initializing = true;
    g_cancellable_reset(d->initialization);
    auto* task = g_task_new(nullptr, d->initialization, Private::initializedPortal,
                            new Private::Callback(d));
    g_task_run_in_thread(task, initializeInWorker);
    g_object_unref(task);
}

quint64 PortalCapture::requestScreenshot(QWindow* parent, bool interactive)
{
    Q_ASSERT(d->onOwnerThread());
    constexpr auto kind = Private::Operation::Kind::Screenshot;
    if (d->draining || d->stopped) return 0;
    if (auto error = d->canRequest(kind, parent)) return d->reject(kind, error);
    auto pending = d->begin(kind, parent);
    pending->interactiveScreenshot = interactive;
    if (parent) d->prepareParent(pending);
    else xdp_portal_take_screenshot(d->portal, nullptr,
        interactive ? XDP_SCREENSHOT_FLAG_INTERACTIVE : XDP_SCREENSHOT_FLAG_NONE,
        pending->cancellable, Private::screenshotReady, new Private::Call{d, pending});
    return pending->id;
}

quint64 PortalCapture::requestScreenCast(QWindow* parent, const ScreenCastOptions& options)
{
    Q_ASSERT(d->onOwnerThread());
    constexpr auto kind = Private::Operation::Kind::ScreenCast;
    if (d->draining || d->stopped) return 0;
    if (auto error = d->canRequest(kind, parent)) return d->reject(kind, error);
    const auto types = (options.monitors ? XDP_OUTPUT_MONITOR : 0) | (options.windows ? XDP_OUTPUT_WINDOW : 0);
    const auto cursor = options.cursor == ScreenCastOptions::Cursor::Hidden ? XDP_CURSOR_MODE_HIDDEN
        : options.cursor == ScreenCastOptions::Cursor::Embedded ? XDP_CURSOR_MODE_EMBEDDED : XDP_CURSOR_MODE_METADATA;
    if (!types || (d->capabilities.sourceTypes & static_cast<quint32>(types)) != static_cast<quint32>(types)
        || !(d->capabilities.cursorModes & cursor))
        return d->reject(kind, failure(Code::Unsupported, QStringLiteral("Portal cannot honor the requested source or cursor mode")));
    if (d->capabilities.screenCast.version < 4
        && (options.persistence != ScreenCastOptions::Persistence::None || !options.restoreToken.isEmpty()))
        return d->reject(kind, failure(Code::Unsupported, QStringLiteral("ScreenCast v4 is required for permission persistence")));
    if (options.restoreToken.contains(QChar(u'\0')))
        return d->reject(kind, failure(Code::InvalidState, QStringLiteral("Restore token contains a NUL character")));
    const auto persist = options.persistence == ScreenCastOptions::Persistence::None ? XDP_PERSIST_MODE_NONE
        : options.persistence == ScreenCastOptions::Persistence::Application ? XDP_PERSIST_MODE_TRANSIENT : XDP_PERSIST_MODE_PERSISTENT;
    const auto token = options.restoreToken.toUtf8();
    auto pending = d->begin(kind, parent);
    xdp_portal_create_screencast_session(d->portal, static_cast<XdpOutputType>(types),
        options.multiple ? XDP_SCREENCAST_FLAG_MULTIPLE : XDP_SCREENCAST_FLAG_NONE,
        cursor, persist, token.isEmpty() ? nullptr : token.constData(), pending->cancellable,
        Private::sessionCreated, new Private::Call{d, pending});
    return pending->id;
}

void PortalCapture::cancelRequest(quint64 id)
{
    Q_ASSERT(d->onOwnerThread());
    const Private::Callback delivery(d);
    const auto& state = delivery.state;
    if (state->operation && state->operation->id == id)
        state->cancel(state->operation, failure(Code::Cancelled, QStringLiteral("Portal request canceled by caller")));
}

quint64 PortalCapture::openPipeWireRemote()
{
    Q_ASSERT(d->onOwnerThread());
    constexpr auto kind = Private::Operation::Kind::PipeWireRemote;
    if (d->draining || d->stopped) return 0;
    if (auto error = d->checkContext()) return d->reject(kind, error);
    if (!d->ready || !d->activeSession || xdp_session_get_session_state(d->activeSession) != XDP_SESSION_ACTIVE)
        return d->reject(kind, failure(Code::InvalidState, QStringLiteral("An active authorized ScreenCast session is required")));
    if (d->operation || d->pipeWireFd >= 0)
        return d->reject(kind, failure(Code::Busy, QStringLiteral("A Portal request or unclaimed PipeWire remote is already pending")));
    auto pending = d->begin(kind, nullptr);
    xdp_session_open_pipewire_remote_async(d->activeSession, pending->cancellable,
        Private::remoteReopened, new Private::Call{d, pending});
    return pending->id;
}

void PortalCapture::shutdown()
{
    Q_ASSERT(d->onOwnerThread());
    // Parent cancellation may synchronously deliver a terminal signal whose
    // host destroys this QObject or dispatches another event loop. Keep both
    // state and this invocation in the drain gate until that stack returns.
    const Private::Callback delivery(d);
    delivery.state->requestShutdown();
}

void PortalCapture::closeScreenCast()
{
    Q_ASSERT(d->onOwnerThread());
    const Private::Callback delivery(d);
    delivery.state->closeActive();
}
bool PortalCapture::hasActiveScreenCast() const { Q_ASSERT(d->onOwnerThread()); return d->activeSession != nullptr; }
int PortalCapture::takePipeWireFd() { Q_ASSERT(d->onOwnerThread()); return std::exchange(d->pipeWireFd, -1); }

} // namespace HuhuCapture::Platform::Linux
