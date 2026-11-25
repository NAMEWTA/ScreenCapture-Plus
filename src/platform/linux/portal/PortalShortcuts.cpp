#include <libportal/portal.h>

#include "platform/linux/portal/PortalShortcuts.h"
#include "platform/linux/portal/PortalSupport.h"
#include "platform/linux/portal/PortalParent.h"

#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWindow>
#include <utility>

namespace HuhuCapture::Platform::Linux {
using namespace PortalSupport;
namespace {

PortalFailure readAssignments(GPtrArray* values, QVector<PortalShortcutBinding>& output)
{
    if (!values) return failure(Code::Failed, QStringLiteral("Portal returned no shortcut assignments"));
    QVector<PortalShortcutBinding> assignments;
    QSet<QString> ids;
    for (guint index = 0; index < values->len; ++index) {
        const auto* native = static_cast<const XdpGlobalShortcutAssigned*>(g_ptr_array_index(values, index));
        if (!native) return failure(Code::Failed, QStringLiteral("Portal returned an invalid shortcut assignment"));
        PortalShortcutBinding binding;
        binding.id = QString::fromUtf8(xdp_global_shortcut_assigned_get_shortcut_id(native));
        binding.description = QString::fromUtf8(xdp_global_shortcut_assigned_get_description(native));
        binding.triggerDescription = QString::fromUtf8(xdp_global_shortcut_assigned_get_trigger_description(native));
        if (binding.id.isEmpty() || ids.contains(binding.id))
            return failure(Code::Failed, QStringLiteral("Portal returned empty or duplicate shortcut IDs"));
        ids.insert(binding.id);
        assignments.push_back(std::move(binding));
    }
    output = std::move(assignments);
    return {};
}

} // namespace

struct PortalShortcuts::Private : std::enable_shared_from_this<Private> {
    struct Operation {
        quint64 id{};
        QPointer<QWindow> parent;
        QMetaObject::Connection parentDestroyed;
        GCancellable* cancellable{g_cancellable_new()};
        std::shared_ptr<PortalParent> portalParent;
        XdpGlobalShortcutsSession* candidate{};
        gulong candidateClosedHandler{};
        QVector<PortalShortcutRequest> requested;
        QVector<PortalShortcutBinding> assigned;
        PortalFailure cancellation;
        bool finished{};

        ~Operation() {
            QObject::disconnect(parentDestroyed);
            if (candidate) {
                auto* session = xdp_global_shortcuts_session_get_session(candidate);
                g_clear_signal_handler(&candidateClosedHandler, session);
                xdp_global_shortcuts_session_close(candidate);
                g_object_unref(candidate);
            }
            g_clear_object(&cancellable);
        }
    };
    struct Call {
        std::shared_ptr<Private> state;
        std::shared_ptr<Operation> operation;
    };
    struct Refresh {
        std::shared_ptr<Private> state;
        XdpGlobalShortcutsSession* session{};
        quint64 generation{};
        ~Refresh() { g_object_unref(session); }
    };

    QPointer<PortalShortcuts> owner;
    XdpPortal* portal{};
    GDBusConnection* bus{};
    GCancellable* initialization{g_cancellable_new()};
    guint nameWatch{};
    gulong busClosedHandler{};
    QByteArray busName;
    PortalShortcutCapabilities capabilities;
    std::shared_ptr<Operation> operation;
    XdpGlobalShortcutsSession* active{};
    gulong activeClosedHandler{};
    gulong activatedHandler{};
    gulong deactivatedHandler{};
    gulong changedHandler{};
    QVector<PortalShortcutBinding> assigned;
    GCancellable* refreshCancellation{};
    quint64 generation{};
    quint64 nextId{1};
    bool refreshAgain{};
    bool initializing{};
    bool ready{};
    bool stopped{};

    explicit Private(PortalShortcuts* value) : owner(value) {
        busName = qgetenv("LIBPORTAL_PORTAL_BUS_NAME");
        if (busName.isEmpty()) busName = "org.freedesktop.portal.Desktop";
    }
    ~Private() {
        if (nameWatch) g_bus_unwatch_name(nameWatch);
        if (bus) g_clear_signal_handler(&busClosedHandler, bus);
        g_clear_object(&initialization);
        g_clear_object(&refreshCancellation);
        g_clear_object(&bus);
        g_clear_object(&portal);
    }

    bool onOwnerThread() const { return owner && QThread::currentThread() == owner->thread(); }
    PortalFailure checkContext() {
        return PortalSupport::checkContext(owner, capabilities.qtPlatform, capabilities.dispatcher);
    }

    void cancel(std::shared_ptr<Operation> pending, PortalFailure reason) {
        if (!pending || pending->finished || pending->cancellation) return;
        pending->cancellation = std::move(reason);
        g_cancellable_cancel(pending->cancellable);
    }

    void closeActive(PortalFailure reason = {}, bool notify = true) {
        auto* session = std::exchange(active, nullptr);
        ++generation;
        if (refreshCancellation) g_cancellable_cancel(refreshCancellation);
        g_clear_object(&refreshCancellation);
        refreshAgain = false;
        assigned.clear();
        if (!session) return;
        g_clear_signal_handler(&activeClosedHandler, xdp_global_shortcuts_session_get_session(session));
        g_clear_signal_handler(&activatedHandler, session);
        g_clear_signal_handler(&deactivatedHandler, session);
        g_clear_signal_handler(&changedHandler, session);
        xdp_global_shortcuts_session_close(session);
        g_object_unref(session);
        if (notify && owner && !stopped) Q_EMIT owner->bindingsClosed(reason);
    }

    void shutdown() {
        stopped = true;
        g_cancellable_cancel(initialization);
        cancel(operation, failure(Code::Cancelled, QStringLiteral("Portal shortcut owner destroyed")));
        closeActive();
        if (nameWatch) g_bus_unwatch_name(std::exchange(nameWatch, 0));
        if (bus) g_clear_signal_handler(&busClosedHandler, bus);
        owner = nullptr;
    }

    void finish(const std::shared_ptr<Operation>& pending, PortalFailure error) {
        if (pending->finished) return;
        if (pending->cancellation) error = pending->cancellation;
        pending->finished = true;
        QObject::disconnect(pending->parentDestroyed);
        pending->portalParent.reset();
        if (pending->candidate) {
            auto* session = xdp_global_shortcuts_session_get_session(pending->candidate);
            g_clear_signal_handler(&pending->candidateClosedHandler, session);
            xdp_global_shortcuts_session_close(pending->candidate);
            g_clear_object(&pending->candidate);
        }
        if (operation == pending) operation.reset();
        if (owner && !stopped) Q_EMIT owner->bindingFinished(pending->id, pending->assigned, error);
    }

    quint64 reject(PortalFailure error) {
        const auto id = nextId++;
        const auto state = shared_from_this();
        QTimer::singleShot(0, owner, [state, id, error = std::move(error)] {
            if (state->owner && !state->stopped) Q_EMIT state->owner->bindingFinished(id, {}, error);
        });
        return id;
    }

    PortalFailure canBind(QWindow* parent, const QVector<PortalShortcutRequest>& shortcuts) {
        if (auto error = checkContext()) return error;
        if (!ready || stopped) return failure(Code::Unavailable, QStringLiteral("GlobalShortcuts initialization has not succeeded"));
        if (operation) return failure(Code::Busy, QStringLiteral("A shortcut binding request is already active"));
        if (!parent || parent->thread() != owner->thread())
            return failure(Code::ParentDestroyed, QStringLiteral("A live parent window on the GUI thread is required"));
        if (!parent->isVisible() || !parent->handle())
            return failure(Code::InvalidState, QStringLiteral("The shortcut parent must have a visible native surface"));
        if (!capabilities.globalShortcuts.available())
            return capabilities.globalShortcuts.failure;
        if (shortcuts.isEmpty())
            return failure(Code::InvalidState, QStringLiteral("At least one shortcut is required; use closeBindings to disable them"));
        QSet<QString> ids;
        for (const auto& shortcut : shortcuts) {
            if (shortcut.id.isEmpty() || shortcut.description.isEmpty() || ids.contains(shortcut.id)
                || shortcut.id.contains(QChar(u'\0')) || shortcut.description.contains(QChar(u'\0'))
                || shortcut.preferredTrigger.contains(QChar(u'\0')))
                return failure(Code::InvalidState, QStringLiteral("Shortcut IDs must be unique and descriptions nonempty, without NUL characters"));
            ids.insert(shortcut.id);
        }
        return {};
    }

    static void initializedPortal(GObject*, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<std::shared_ptr<Private>> holder(static_cast<std::shared_ptr<Private>*>(data));
        const auto state = *holder;
        g_autoptr(GError) error = nullptr;
        std::unique_ptr<InitializedPortal> initialized(static_cast<InitializedPortal*>(g_task_propagate_pointer(G_TASK(result), &error)));
        if (state->stopped) return;
        if (!initialized) {
            state->initializing = false;
            auto failure = fromError(error);
            if (failure.code != Code::Cancelled) failure.code = Code::Unavailable;
            if (state->owner) Q_EMIT state->owner->initialized(state->capabilities, failure);
            return;
        }
        state->portal = std::exchange(initialized->portal, nullptr);
        state->bus = std::exchange(initialized->bus, nullptr);
        state->busClosedHandler = g_signal_connect(state->bus, "closed", G_CALLBACK(busClosed), state.get());
        g_dbus_connection_call(state->bus, state->busName.constData(), "/org/freedesktop/portal/desktop",
            "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", "org.freedesktop.portal.GlobalShortcuts"),
            G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, state->initialization,
            propertiesReady, new std::shared_ptr<Private>(state));
    }

    static void propertiesReady(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<std::shared_ptr<Private>> holder(static_cast<std::shared_ptr<Private>*>(data));
        const auto state = *holder;
        g_autoptr(GError) error = nullptr;
        g_autoptr(GVariant) returned = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (state->stopped) return;
        auto& feature = state->capabilities.globalShortcuts;
        if (!returned) feature.failure = fromError(error);
        else {
            g_autoptr(GVariant) values = g_variant_get_child_value(returned, 0);
            if (!g_variant_lookup(values, "version", "u", &feature.version) || feature.version == 0)
                feature.failure = failure(Code::Unsupported, QStringLiteral("Portal did not advertise a usable GlobalShortcuts version"));
            if (g_dbus_connection_is_closed(state->bus))
                feature.failure = failure(Code::Unavailable, QStringLiteral("The session bus disconnected during initialization"));
        }
        state->initializing = false;
        state->ready = feature.available();
        if (state->ready)
            state->nameWatch = g_bus_watch_name_on_connection(state->bus, state->busName.constData(),
                G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr, portalVanished, state.get(), nullptr);
        if (state->owner) Q_EMIT state->owner->initialized(state->capabilities, feature.failure);
    }

    void unavailable(PortalFailure reason) {
        ready = false;
        capabilities.globalShortcuts.failure = reason;
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

    static void candidateClosed(XdpSession*, gpointer data) {
        auto* pending = static_cast<Operation*>(data);
        if (pending->finished || pending->cancellation) return;
        pending->cancellation = failure(Code::Unavailable, QStringLiteral("The pending shortcut session closed"));
        g_autoptr(GCancellable) cancellation = G_CANCELLABLE(g_object_ref(pending->cancellable));
        g_cancellable_cancel(cancellation);
    }
    static void sessionCreated(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Call> call(static_cast<Call*>(data));
        const auto& state = call->state;
        const auto& pending = call->operation;
        g_autoptr(GError) error = nullptr;
        pending->candidate = xdp_portal_create_global_shortcuts_session_finish(XDP_PORTAL(source), result, &error);
        if (!pending->candidate) { state->finish(pending, fromError(error)); return; }
        if (state->stopped || pending->cancellation || !pending->parent) {
            state->finish(pending, failure(Code::ParentDestroyed, QStringLiteral("Shortcut parent is no longer available")));
            return;
        }
        auto* session = xdp_global_shortcuts_session_get_session(pending->candidate);
        if (xdp_session_get_session_state(session) == XDP_SESSION_CLOSED) {
            state->finish(pending, failure(Code::Unavailable, QStringLiteral("Shortcut session closed before parent export")));
            return;
        }
        pending->candidateClosedHandler = g_signal_connect(session, "closed", G_CALLBACK(candidateClosed), pending.get());
        state->prepareParent(pending);
    }

    void prepareParent(const std::shared_ptr<Operation>& pending) {
        pending->portalParent = PortalParent::create();
        std::weak_ptr<Private> weakState = shared_from_this();
        std::weak_ptr<Operation> weakPending = pending;
        QObject::connect(pending->portalParent.get(), &PortalParent::prepared, pending->portalParent.get(),
            [weakState, weakPending](const PortalFailure& error) {
                const auto state = weakState.lock();
                const auto operation = weakPending.lock();
                if (!state || !operation || operation->finished) return;
                if (error || operation->cancellation || state->stopped) { state->finish(operation, error); return; }
                if (!operation->portalParent->nativeParent()) {
                    state->finish(operation, failure(Code::ParentDestroyed, QStringLiteral("The exported shortcut parent is no longer valid")));
                    return;
                }
                state->bindCandidate(operation);
            });
        QObject::connect(pending->portalParent.get(), &PortalParent::invalidated, pending->portalParent.get(),
            [weakState, weakPending](const PortalFailure& error) {
                if (const auto state = weakState.lock()) state->cancel(weakPending.lock(), error);
            });
        pending->portalParent->prepare(pending->parent, pending->cancellable);
    }

    void bindCandidate(const std::shared_ptr<Operation>& pending) {
        g_autoptr(GPtrArray) shortcuts = g_ptr_array_new_with_free_func(reinterpret_cast<GDestroyNotify>(xdp_global_shortcut_free));
        for (const auto& shortcut : pending->requested) {
            const auto id = shortcut.id.toUtf8();
            const auto description = shortcut.description.toUtf8();
            const auto trigger = shortcut.preferredTrigger.toUtf8();
            g_ptr_array_add(shortcuts, xdp_global_shortcut_new(id.constData(), description.constData(),
                trigger.isEmpty() ? nullptr : trigger.constData()));
        }
        xdp_global_shortcuts_session_bind_shortcuts_with_parent(pending->candidate, shortcuts, pending->portalParent->nativeParent(),
            pending->cancellable, shortcutsBound, new Call{shared_from_this(), pending});
    }

    static void shortcutsBound(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Call> call(static_cast<Call*>(data));
        const auto& state = call->state;
        const auto& pending = call->operation;
        g_autoptr(GError) error = nullptr;
        g_autoptr(GPtrArray) values = xdp_global_shortcuts_session_bind_shortcuts_finish(XDP_GLOBAL_SHORTCUTS_SESSION(source), result, &error);
        if (!values) { state->finish(pending, fromError(error)); return; }
        if (state->stopped || pending->cancellation) { state->finish(pending, {}); return; }
        if (auto invalid = readAssignments(values, pending->assigned)) { state->finish(pending, invalid); return; }
        QSet<QString> requested;
        for (const auto& shortcut : pending->requested) requested.insert(shortcut.id);
        for (const auto& shortcut : pending->assigned) {
            if (shortcut.triggerDescription.isEmpty() || !requested.remove(shortcut.id)) {
                state->finish(pending, failure(Code::Failed, QStringLiteral("Portal returned an unassigned or unexpected shortcut")));
                return;
            }
        }
        if (!requested.isEmpty()) {
            state->finish(pending, failure(Code::Failed, QStringLiteral("Portal did not bind every requested shortcut")));
            return;
        }
        auto* native = xdp_global_shortcuts_session_get_session(pending->candidate);
        if (xdp_session_get_session_state(native) == XDP_SESSION_CLOSED) {
            state->finish(pending, failure(Code::Unavailable, QStringLiteral("Shortcut session closed before binding completed")));
            return;
        }
        // BindShortcuts is permitted only once per session. Commit the fully
        // granted candidate now; any failure above leaves the old session live.
        g_clear_signal_handler(&pending->candidateClosedHandler, native);
        state->closeActive({}, false);
        state->active = std::exchange(pending->candidate, nullptr);
        state->assigned = pending->assigned;
        state->activeClosedHandler = g_signal_connect(native, "closed", G_CALLBACK(activeClosed), state.get());
        state->activatedHandler = g_signal_connect(state->active, "activated", G_CALLBACK(shortcutActivated), state.get());
        state->deactivatedHandler = g_signal_connect(state->active, "deactivated", G_CALLBACK(shortcutDeactivated), state.get());
        state->changedHandler = g_signal_connect(state->active, "shortcuts-changed", G_CALLBACK(shortcutsChanged), state.get());
        state->finish(pending, {});
        if (state->owner && !state->stopped && state->active) {
            const auto snapshot = state->assigned;
            Q_EMIT state->owner->bindingsChanged(snapshot);
        }
    }

    static void activeClosed(XdpSession*, gpointer data) {
        const auto state = static_cast<Private*>(data)->shared_from_this();
        state->closeActive(failure(Code::Unavailable, QStringLiteral("The active shortcut session closed")));
    }
    void activation(XdpGlobalShortcutsSession* session, const char* id, guint64 timestamp, GVariant* options, bool pressed) {
        if (session != active || stopped || !owner) return;
        const auto shortcutId = QString::fromUtf8(id);
        bool known = false;
        for (const auto& shortcut : assigned) if (shortcut.id == shortcutId) { known = true; break; }
        if (!known) return;
        const char* nativeToken = nullptr;
        if (options) g_variant_lookup(options, "activation_token", "&s", &nativeToken);
        const auto token = QString::fromUtf8(nativeToken);
        if (pressed) Q_EMIT owner->activated(shortcutId, timestamp, token);
        else Q_EMIT owner->deactivated(shortcutId, timestamp, token);
    }
    static void shortcutActivated(XdpGlobalShortcutsSession* session, const char* id, guint64 time, GVariant* options, gpointer data) {
        const auto state = static_cast<Private*>(data)->shared_from_this();
        state->activation(session, id, time, options, true);
    }
    static void shortcutDeactivated(XdpGlobalShortcutsSession* session, const char* id, guint64 time, GVariant* options, gpointer data) {
        const auto state = static_cast<Private*>(data)->shared_from_this();
        state->activation(session, id, time, options, false);
    }
    static void shortcutsChanged(XdpGlobalShortcutsSession* session, GPtrArray*, gpointer data) {
        const auto state = static_cast<Private*>(data)->shared_from_this();
        if (session == state->active && !state->stopped) state->refreshAssignments();
    }
    void refreshAssignments() {
        if (!active || stopped) return;
        if (refreshCancellation) { refreshAgain = true; return; }
        refreshCancellation = g_cancellable_new();
        xdp_global_shortcuts_session_list_shortcuts(active, refreshCancellation, assignmentsListed,
            new Refresh{shared_from_this(), static_cast<XdpGlobalShortcutsSession*>(g_object_ref(active)), generation});
    }
    static void assignmentsListed(GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Refresh> call(static_cast<Refresh*>(data));
        const auto& state = call->state;
        g_autoptr(GError) error = nullptr;
        g_autoptr(GPtrArray) values = xdp_global_shortcuts_session_list_shortcuts_finish(XDP_GLOBAL_SHORTCUTS_SESSION(source), result, &error);
        if (state->stopped || call->session != state->active || call->generation != state->generation) return;
        g_clear_object(&state->refreshCancellation);
        const bool again = std::exchange(state->refreshAgain, false);
        QVector<PortalShortcutBinding> assignments;
        const auto failure = values ? readAssignments(values, assignments) : fromError(error);
        if (failure) {
            if (state->owner) Q_EMIT state->owner->bindingsRefreshFailed(failure);
        } else {
            state->assigned = std::move(assignments);
            const auto snapshot = state->assigned;
            if (state->owner) Q_EMIT state->owner->bindingsChanged(snapshot);
        }
        if (again && !state->stopped && call->session == state->active && call->generation == state->generation)
            state->refreshAssignments();
    }
};

PortalShortcuts::PortalShortcuts(QObject* parent) : QObject(parent), d(std::make_shared<Private>(this)) {}
PortalShortcuts::~PortalShortcuts() { Q_ASSERT(d->onOwnerThread()); d->shutdown(); }

void PortalShortcuts::initialize()
{
    Q_ASSERT(d->onOwnerThread());
    if (auto error = d->checkContext()) { Q_EMIT initialized(d->capabilities, error); return; }
    if (d->initializing || d->ready || d->operation || d->active) {
        Q_EMIT initialized(d->capabilities, failure(Code::InvalidState, QStringLiteral("Shortcut initialization was already requested")));
        return;
    }
    if (d->nameWatch) g_bus_unwatch_name(std::exchange(d->nameWatch, 0));
    if (d->bus) g_clear_signal_handler(&d->busClosedHandler, d->bus);
    g_clear_object(&d->portal);
    g_clear_object(&d->bus);
    d->capabilities.globalShortcuts = {};
    d->initializing = true;
    g_cancellable_reset(d->initialization);
    auto* task = g_task_new(nullptr, d->initialization, Private::initializedPortal, new std::shared_ptr<Private>(d));
    g_task_run_in_thread(task, initializeInWorker);
    g_object_unref(task);
}

quint64 PortalShortcuts::bindShortcuts(QWindow* parent, const QVector<PortalShortcutRequest>& shortcuts)
{
    Q_ASSERT(d->onOwnerThread());
    if (auto error = d->canBind(parent, shortcuts)) return d->reject(error);
    auto pending = std::make_shared<Private::Operation>();
    pending->id = d->nextId++;
    pending->parent = parent;
    pending->requested = shortcuts;
    std::weak_ptr<Private> weakState = d;
    std::weak_ptr<Private::Operation> weakPending = pending;
    pending->parentDestroyed = QObject::connect(parent, &QObject::destroyed, this, [weakState, weakPending] {
        if (auto state = weakState.lock())
            state->cancel(weakPending.lock(), failure(Code::ParentDestroyed, QStringLiteral("Shortcut parent window was destroyed")));
    });
    d->operation = pending;
    xdp_portal_create_global_shortcuts_session(d->portal, pending->cancellable,
        Private::sessionCreated, new Private::Call{d, pending});
    return pending->id;
}

void PortalShortcuts::cancelRequest(quint64 id)
{
    Q_ASSERT(d->onOwnerThread());
    if (d->operation && d->operation->id == id)
        d->cancel(d->operation, failure(Code::Cancelled, QStringLiteral("Shortcut binding canceled by caller")));
}

void PortalShortcuts::closeBindings()
{
    Q_ASSERT(d->onOwnerThread());
    d->cancel(d->operation, failure(Code::Cancelled, QStringLiteral("Shortcut bindings closed by caller")));
    d->closeActive();
}

bool PortalShortcuts::hasActiveBindings() const { Q_ASSERT(d->onOwnerThread()); return d->active != nullptr; }
QVector<PortalShortcutBinding> PortalShortcuts::bindings() const { Q_ASSERT(d->onOwnerThread()); return d->assigned; }

} // namespace HuhuCapture::Platform::Linux
