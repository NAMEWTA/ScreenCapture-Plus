#pragma once

// Internal support shared by the independent capture and shortcut controllers.
// Include GLib before Qt so Qt's optional keyword macros cannot alter it.
#include <libportal/portal.h>
#include "platform/linux/portal/PortalTypes.h"

#include <QAbstractEventDispatcher>
#include <QGuiApplication>
#include <QThread>
#include <memory>
#include <utility>

namespace HuhuCapture::Platform::Linux::PortalSupport {

using Code = PortalFailure::Code;

inline PortalFailure failure(Code code, QString message)
{
    PortalFailure result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

inline PortalFailure fromError(const GError* error)
{
    if (!error) return failure(Code::Failed, QStringLiteral("Portal returned no result or error"));
    auto result = failure(Code::Failed, QString::fromUtf8(error->message));
    result.domain = QString::fromUtf8(g_quark_to_string(error->domain));
    result.nativeCode = error->code;
    g_autofree char* remote = g_dbus_error_get_remote_error(error);
    if (remote) result.remoteError = QString::fromUtf8(remote);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        result.code = Code::Cancelled;
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)
             || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED)
             || result.remoteError == QStringLiteral("org.freedesktop.portal.Error.NotAllowed"))
        result.code = Code::Denied;
    else if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
             || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)
             || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_SERVER)
             || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED)
             || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED))
        result.code = Code::Unavailable;
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
             || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE)
             || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        result.code = Code::Unsupported;
    return result;
}

inline PortalFailure checkContext(const QObject* owner, QString& qtPlatform, QString& dispatcherName)
{
    if (!qGuiApp || !owner || QThread::currentThread() != owner->thread()
        || owner->thread() != qGuiApp->thread())
        return failure(Code::InvalidState, QStringLiteral("Portal control must run on the QApplication thread"));
    auto* dispatcher = QAbstractEventDispatcher::instance();
    qtPlatform = QGuiApplication::platformName();
    dispatcherName = dispatcher ? QString::fromLatin1(dispatcher->metaObject()->className()) : QString();
    if (!dispatcher || !dispatcher->inherits("QEventDispatcherGlib"))
        return failure(Code::Unsupported, QStringLiteral("Qt's GLib event dispatcher is required"));
    auto* context = g_main_context_get_thread_default();
    if (context && context != g_main_context_default())
        return failure(Code::Unsupported, QStringLiteral("Portal and Qt must share the default GLib context"));
    return {};
}

struct InitializedPortal {
    XdpPortal* portal{};
    GDBusConnection* bus{};
    ~InitializedPortal() { g_clear_object(&bus); g_clear_object(&portal); }
};

inline void initializeInWorker(GTask* task, gpointer, gpointer, GCancellable* cancellable)
{
    // In 0.11.0 even the fallible initializer obtains the bus synchronously.
    // Construct on GIO's worker pool and hand ownership back to the Qt thread.
    // No worker owns or dispatches a GMainLoop.
    if (g_task_return_error_if_cancelled(task)) return;
    try {
        auto result = std::make_unique<InitializedPortal>();
        g_autoptr(GError) error = nullptr;
        result->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, &error);
        if (!result->bus) { g_task_return_error(task, g_steal_pointer(&error)); return; }
        // GIO's shared bus connection otherwise terminates the process when
        // the session bus disappears, bypassing media and file cleanup.
        g_dbus_connection_set_exit_on_close(result->bus, FALSE);
        result->portal = xdp_portal_initable_new(&error);
        if (!result->portal) { g_task_return_error(task, g_steal_pointer(&error)); return; }
        g_task_return_pointer(task, result.release(), [](gpointer value) {
            delete static_cast<InitializedPortal*>(value);
        });
    }
    catch (...) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Could not allocate Portal initialization state");
    }
}

} // namespace HuhuCapture::Platform::Linux::PortalSupport
