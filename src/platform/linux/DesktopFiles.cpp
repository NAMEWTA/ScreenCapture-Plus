// GIO headers precede Qt's optional signals/slots keywords.
#include <gio/gio.h>
#include "platform/linux/DesktopFiles.h"

#include <QAbstractEventDispatcher>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <new>
#include <utility>

namespace HuhuCapture::Platform::Linux {
namespace {
Desktop::Failure errorFrom(const GError* error)
{
    if (!error) return {Desktop::Error::FileOperation, 0, QStringLiteral("The file manager returned no result")};
    auto code = Desktop::Error::FileOperation;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) code = Desktop::Error::Cancelled;
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
        g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)) code = Desktop::Error::Unavailable;
    return {code, error->code, QString::fromUtf8(error->message)};
}
}

struct DesktopFiles::State {
    struct Request {
        QPointer<DesktopFiles> owner;
        quint64 token{};
        QByteArray uri, folderUri;
        bool select{}, settled{};
        GCancellable* cancellable{g_cancellable_new()};
        GDBusConnection* bus{};
        ~Request() { g_clear_object(&bus); g_clear_object(&cancellable); }
    };
    using Shared = std::shared_ptr<Request>;
    Shared active;
    QTimer timeout;

    explicit State(DesktopFiles* owner)
    {
        timeout.setSingleShot(true); timeout.setInterval(15000);
        QObject::connect(&timeout, &QTimer::timeout, owner, [this, owner] {
            if (active) owner->finish(active->token,
                {Desktop::Error::Timeout, 0, QStringLiteral("The file manager did not finish opening this location")}, false);
        });
    }
    ~State()
    {
        timeout.stop();
        if (active) { active->settled = true; g_cancellable_cancel(active->cancellable); }
    }
    static bool live(const Shared& request) { return request->owner && !request->settled; }
    static void failure(const Shared& request, const char* text) noexcept
    {
        try {
            if (live(request)) request->owner->finish(request->token,
                {Desktop::Error::Allocation, 0, QString::fromUtf8(text)}, false);
        } catch (...) { /* The existing timeout remains the terminal fallback. */ }
    }
    static void launchFinished(GObject*, GAsyncResult* result, gpointer data) noexcept
    {
        std::unique_ptr<Shared> context(static_cast<Shared*>(data)); const auto request = *context;
        g_autoptr(GError) error = nullptr;
        const bool success = g_app_info_launch_default_for_uri_finish(result, &error);
        try {
            if (!live(request)) return;
            request->owner->finish(request->token, success ? Desktop::Failure{} : errorFrom(error), request->select && success);
        } catch (...) { failure(request, "Cannot finish the file-manager launch"); }
    }
    static void launchFolder(const Shared& request)
    {
        if (!live(request)) return;
        // Unlike QDesktopServices' Wayland activation path, this finish API
        // reports the real launcher/DBus activation result instead of returning
        // true before a deferred callback has even attempted the launch.
        auto context = std::make_unique<Shared>(request);
        g_app_info_launch_default_for_uri_async(request->folderUri.constData(), nullptr,
            request->cancellable, launchFinished, context.release());
    }
    static void showFinished(GObject* object, GAsyncResult* result, gpointer data) noexcept
    {
        std::unique_ptr<Shared> context(static_cast<Shared*>(data)); const auto request = *context;
        g_autoptr(GError) error = nullptr;
        g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
        try {
            if (!live(request)) return;
            if (reply) { request->owner->finish(request->token, {}, false); return; }
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                request->owner->finish(request->token, errorFrom(error), false); return;
            }
            launchFolder(request);
        } catch (...) { failure(request, "Cannot prepare the file-manager fallback"); }
    }
    static void busReady(GObject*, GAsyncResult* result, gpointer data) noexcept
    {
        std::unique_ptr<Shared> context(static_cast<Shared*>(data)); const auto request = *context;
        g_autoptr(GError) error = nullptr;
        auto* bus = g_bus_get_finish(result, &error);
        if (bus) g_dbus_connection_set_exit_on_close(bus, FALSE);
        request->bus = bus;
        try {
            if (!live(request)) return;
            if (!bus) {
                if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) request->owner->finish(request->token, errorFrom(error), false);
                else launchFolder(request);
                return;
            }
            const char* uris[]{request->uri.constData(), nullptr};
            auto next = std::make_unique<Shared>(request);
            g_dbus_connection_call(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                "org.freedesktop.FileManager1", request->select ? "ShowItems" : "ShowFolders",
                g_variant_new("(^ass)", uris, ""), G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, 2500,
                request->cancellable, showFinished, next.release());
        } catch (...) { failure(request, "Cannot prepare the file-manager request"); }
    }
};

DesktopFiles::DesktopFiles(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}
DesktopFiles::~DesktopFiles() = default;

Desktop::Failure DesktopFiles::start(quint64 token, const QString& path, bool select)
{
    if (!qGuiApp || QThread::currentThread() != thread() || thread() != qGuiApp->thread())
        return {Desktop::Error::WrongThread, 0, QStringLiteral("File-manager control requires the GUI thread")};
    if (state_->active) return {Desktop::Error::Busy, 0, QStringLiteral("Another file-manager request is pending")};
    auto* dispatcher = QAbstractEventDispatcher::instance();
    auto* context = g_main_context_get_thread_default();
    if (!dispatcher || !dispatcher->inherits("QEventDispatcherGlib") || (context && context != g_main_context_default()))
        return {Desktop::Error::Unavailable, 0, QStringLiteral("The file manager requires Qt's default GLib event dispatcher")};
    if (!token || path.isEmpty() || !QDir::isAbsolutePath(path) || path.contains(QChar::Null))
        return {Desktop::Error::InvalidArgument, 0, QStringLiteral("An absolute local file-manager path is required")};
    try {
        auto request = std::make_shared<State::Request>(); request->owner = this; request->token = token; request->select = select;
        request->uri = QUrl::fromLocalFile(path).toEncoded();
        // QFileInfo's path() is lexical; disk identity was already checked by
        // the service worker. This GUI adapter never stats or decodes a file.
        request->folderUri = QUrl::fromLocalFile(select ? QFileInfo(path).path() : path).toEncoded();
        auto callback = std::make_unique<State::Shared>(request);
        state_->active = std::move(request); state_->timeout.start();
        g_bus_get(G_BUS_TYPE_SESSION, state_->active->cancellable, State::busReady, callback.release());
        return {};
    } catch (const std::bad_alloc&) {
        if (state_->active) { state_->active->settled = true; g_cancellable_cancel(state_->active->cancellable); state_->active.reset(); }
        state_->timeout.stop(); return {Desktop::Error::Allocation, 0, QStringLiteral("Cannot allocate a file-manager request")};
    }
}

void DesktopFiles::finish(quint64 token, Desktop::Failure failure, bool parentFallback)
{
    if (!state_->active || state_->active->token != token || state_->active->settled) return;
    // Queue application delivery before releasing state; C callback stacks may
    // finish safely even when a connected host destroys this service.
    QTimer::singleShot(0, this, [this, token, failure = std::move(failure), parentFallback] {
        Q_EMIT completed(token, failure, parentFallback);
    });
    state_->active->settled = true; g_cancellable_cancel(state_->active->cancellable);
    state_->active.reset(); state_->timeout.stop();
}
void DesktopFiles::cancel()
{
    if (state_->active) finish(state_->active->token,
        {Desktop::Error::Cancelled, 0, QStringLiteral("The file-manager request was cancelled")}, false);
}

} // namespace HuhuCapture::Platform::Linux
