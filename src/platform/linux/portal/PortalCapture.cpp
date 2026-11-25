// libportal is linked, not reimplemented. Its pinned downstream patch supplies
// reliable request completion and a cancellable async OpenPipeWireRemote API.
// Include GLib before Qt so the latter's optional keyword macros cannot alter it.
#include <libportal/portal.h>
#include <libportal-qt6/portal-qt6.h>

#include "platform/linux/portal/PortalCapture.h"
#include "platform/linux/portal/PortalSupport.h"

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
        enum class Kind { Screenshot, ScreenCast };
        quint64 id{};
        Kind kind{};
        QPointer<QWindow> parent;
        QMetaObject::Connection parentDestroyed;
        GCancellable* cancellable{g_cancellable_new()};
        XdpParent* portalParent{};
        XdpSession* session{};
        gulong sessionClosedHandler{};
        PortalFailure cancellation;
        QVector<PortalStream> streams;
        QString restoreToken;
        bool finished{};

        ~Operation() {
            QObject::disconnect(parentDestroyed);
            if (session) {
                g_clear_signal_handler(&sessionClosedHandler, session);
                xdp_session_close(session);
                g_object_unref(session);
            }
            if (portalParent) xdp_parent_free(portalParent);
            g_clear_object(&cancellable);
        }
    };
    struct Call {
        std::shared_ptr<Private> state;
        std::shared_ptr<Operation> operation;
    };
    struct Probe {
        std::shared_ptr<Private> state;
        bool screenCast;
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

    void cancel(const std::shared_ptr<Operation>& pending, PortalFailure reason) {
        if (!pending || pending->finished || pending->cancellation) return;
        pending->cancellation = std::move(reason);
        g_cancellable_cancel(pending->cancellable);
    }

    void closeActive(PortalFailure reason = {}) {
        auto* session = std::exchange(activeSession, nullptr);
        if (!session) return;
        g_clear_signal_handler(&activeClosedHandler, session);
        if (pipeWireFd >= 0) ::close(std::exchange(pipeWireFd, -1));
        xdp_session_close(session);
        g_object_unref(session);
        if (owner && !stopped) Q_EMIT owner->screenCastClosed(reason);
    }

    void shutdown() {
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
        if (pending->portalParent) xdp_parent_free(std::exchange(pending->portalParent, nullptr));
        if (pending->session) {
            g_clear_signal_handler(&pending->sessionClosedHandler, pending->session);
            xdp_session_close(pending->session);
            g_clear_object(&pending->session);
        }
        if (operation == pending) operation.reset();
        if (!owner || stopped) return;
        if (pending->kind == Operation::Kind::Screenshot)
            Q_EMIT owner->screenshotFinished(pending->id, uri, error);
        else
            Q_EMIT owner->screenCastFinished(pending->id, pending->streams, pending->restoreToken, error);
    }

    quint64 reject(Operation::Kind kind, PortalFailure error) {
        const auto id = nextId++;
        const auto state = shared_from_this();
        QTimer::singleShot(0, owner, [state, id, kind, error = std::move(error)] {
            if (!state->owner || state->stopped) return;
            if (kind == Operation::Kind::Screenshot)
                Q_EMIT state->owner->screenshotFinished(id, {}, error);
            else
                Q_EMIT state->owner->screenCastFinished(id, {}, {}, error);
        });
        return id;
    }

    PortalFailure canRequest(Operation::Kind kind, QWindow* parent) {
        if (auto error = checkContext()) return error;
        if (!ready || stopped) return failure(Code::Unavailable, QStringLiteral("Portal initialization has not succeeded"));
        if (operation || activeSession) return failure(Code::Busy, QStringLiteral("A Portal request or screencast is already active"));
        if (!parent || parent->thread() != owner->thread())
            return failure(Code::ParentDestroyed, QStringLiteral("A live parent window on the GUI thread is required"));
        if (!parent->isVisible() || !parent->handle())
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
        pending->parentDestroyed = QObject::connect(parent, &QObject::destroyed, owner, [weakState, weakOperation] {
            if (auto state = weakState.lock())
                state->cancel(weakOperation.lock(), failure(Code::ParentDestroyed, QStringLiteral("Portal parent window was destroyed")));
        });
        operation = pending;
        return pending;
    }

    static void initializedPortal(GObject*, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<std::shared_ptr<Private>> holder(static_cast<std::shared_ptr<Private>*>(data));
        const auto state = *holder;
        g_autoptr(GError) nativeError = nullptr;
        std::unique_ptr<InitializedPortal> initialized(static_cast<InitializedPortal*>(g_task_propagate_pointer(G_TASK(result), &nativeError)));
        if (state->stopped) return;
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
    }
    static void portalVanished(GDBusConnection*, const gchar*, gpointer data) {
        const auto state = static_cast<Private*>(data)->shared_from_this();
        if (!state->stopped)
            state->unavailable(failure(Code::Unavailable, QStringLiteral("The desktop Portal service disappeared")));
    }
    static void busClosed(GDBusConnection*, gboolean, GError* error, gpointer data) {
        const auto state = static_cast<Private*>(data)->shared_from_this();
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
        pending->portalParent = xdp_parent_new_qt(pending->parent);
        xdp_session_start(pending->session, pending->portalParent, pending->cancellable,
                          sessionStarted, new Call{state, pending});
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
        pending->sessionClosedHandler = g_signal_connect(pending->session, "closed", G_CALLBACK(pendingSessionClosed), pending.get());
        xdp_session_open_pipewire_remote_async(pending->session, pending->cancellable,
                                               pipeWireReady, new Call{state, pending});
    }

    static void pendingSessionClosed(XdpSession*, gpointer data) {
        auto* pending = static_cast<Operation*>(data);
        if (!pending->cancellation)
            pending->cancellation = failure(Code::Unavailable, QStringLiteral("ScreenCast session closed before its remote was ready"));
        g_cancellable_cancel(pending->cancellable);
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
        auto* raw = static_cast<Private*>(data);
        const auto state = raw->shared_from_this();
        state->closeActive(failure(Code::Unavailable, QStringLiteral("The compositor closed the ScreenCast session")));
    }
};

PortalCapture::PortalCapture(QObject* parent) : QObject(parent), d(std::make_shared<Private>(this)) {}
PortalCapture::~PortalCapture() { Q_ASSERT(d->onOwnerThread()); d->shutdown(); }

void PortalCapture::initialize()
{
    Q_ASSERT(d->onOwnerThread());
    if (auto error = d->checkContext()) { Q_EMIT initialized(d->capabilities, error); return; }
    if (d->initializing || d->ready || d->operation || d->activeSession) {
        Q_EMIT initialized(d->capabilities, failure(Code::InvalidState, QStringLiteral("Portal initialization was already requested")));
        return;
    }
    if (d->nameWatch) g_bus_unwatch_name(std::exchange(d->nameWatch, 0));
    if (d->bus) g_clear_signal_handler(&d->busClosedHandler, d->bus);
    g_clear_object(&d->portal);
    g_clear_object(&d->bus);
    d->capabilities.screenshot = {};
    d->capabilities.screenCast = {};
    d->capabilities.sourceTypes = 0;
    d->capabilities.cursorModes = 0;
    d->initializing = true;
    g_cancellable_reset(d->initialization);
    auto* task = g_task_new(nullptr, d->initialization, Private::initializedPortal,
                            new std::shared_ptr<Private>(d));
    g_task_run_in_thread(task, initializeInWorker);
    g_object_unref(task);
}

quint64 PortalCapture::requestScreenshot(QWindow* parent, bool interactive)
{
    Q_ASSERT(d->onOwnerThread());
    constexpr auto kind = Private::Operation::Kind::Screenshot;
    if (auto error = d->canRequest(kind, parent)) return d->reject(kind, error);
    auto pending = d->begin(kind, parent);
    pending->portalParent = xdp_parent_new_qt(parent);
    xdp_portal_take_screenshot(d->portal, pending->portalParent,
        interactive ? XDP_SCREENSHOT_FLAG_INTERACTIVE : XDP_SCREENSHOT_FLAG_NONE,
        pending->cancellable, Private::screenshotReady, new Private::Call{d, pending});
    return pending->id;
}

quint64 PortalCapture::requestScreenCast(QWindow* parent, const ScreenCastOptions& options)
{
    Q_ASSERT(d->onOwnerThread());
    constexpr auto kind = Private::Operation::Kind::ScreenCast;
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
    if (d->operation && d->operation->id == id)
        d->cancel(d->operation, failure(Code::Cancelled, QStringLiteral("Portal request canceled by caller")));
}

void PortalCapture::closeScreenCast() { Q_ASSERT(d->onOwnerThread()); d->closeActive(); }
bool PortalCapture::hasActiveScreenCast() const { Q_ASSERT(d->onOwnerThread()); return d->activeSession != nullptr; }
int PortalCapture::takePipeWireFd() { Q_ASSERT(d->onOwnerThread()); return std::exchange(d->pipeWireFd, -1); }

} // namespace HuhuCapture::Platform::Linux
