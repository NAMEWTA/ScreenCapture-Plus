#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include "platform/linux/DesktopCaptureAction.h"
#include "services/desktop/LaunchCommand.h"
#include "services/files/QtPaths.h"

#include <QCryptographicHash>
#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QThread>
#include <algorithm>
#include <memory>
#include <utility>

namespace HuhuCapture::Platform::Linux {
namespace {
using Desktop::Error;
using Desktop::Failure;
constexpr qsizetype maximumDesktopBytes = 64 * 1024;
constexpr char desktopGroup[] = "Desktop Entry";
constexpr char actionGroup[] = "Desktop Action FinishCapture";
Failure failed(Error code, QString detail) { return {code, 0, std::move(detail)}; }
Failure cancelled() { return failed(Error::Cancelled, QStringLiteral("Desktop capture-action verification was cancelled")); }
struct KeyFileDelete { void operator()(GKeyFile* file) const { g_key_file_unref(file); } };
using KeyFile = std::unique_ptr<GKeyFile, KeyFileDelete>;
struct AppDelete { void operator()(GDesktopAppInfo* app) const { g_object_unref(app); } };
using AppInfo = std::unique_ptr<GDesktopAppInfo, AppDelete>;

struct Located {
    CaptureActionRegistration registration;
    AppInfo app;
    Failure failure;
};

QString canonical(const QString& value)
{
    return QDir::isAbsolutePath(value) && !value.contains(QChar::Null)
        ? QFileInfo(value).canonicalFilePath() : QString();
}

// Let GIO choose the effective desktop ID and GLib parse the Exec argument
// grammar. This action deliberately accepts no filename/URI expansion: the
// only supported field escape is %% (a literal percent in a persistent path).
QStringList actionArguments(const gchar* command)
{
    if (!command) return {};
    QByteArray expanded;
    const QByteArray input(command);
    expanded.reserve(input.size());
    for (qsizetype index = 0; index < input.size(); ++index) {
        const auto value = input[index];
        if (value == '%') {
            if (++index == input.size() || input[index] != '%') return {};
        }
        expanded.append(value);
    }
    gint count{};
    g_auto(GStrv) values = nullptr;
    if (!g_shell_parse_argv(expanded.constData(), &count, &values, nullptr) || count < 2 || count > 3) return {};
    QStringList result;
    for (gint index = 0; index < count; ++index) result.push_back(QString::fromUtf8(values[index]));
    return result;
}

Located locate(const QString& requestedExecutable, const QString& requestedProfile, std::stop_token stop)
{
    Located result;
    const auto refuse = [&](Error code, const QString& detail) {
        result.failure = failed(code, detail);
    };
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    if (!Desktop::persistentLaunchExecutable(requestedExecutable)) {
        refuse(Error::Unavailable, QStringLiteral("A persistent executable is required for the desktop finish action")); return result;
    }
    const auto executable = canonical(requestedExecutable);
    const auto profile = requestedProfile.isEmpty() ? QString() : canonical(requestedProfile);
    if (executable.isEmpty() || (!requestedProfile.isEmpty() && (profile.isEmpty() || !QFileInfo(profile).isDir()))) {
        refuse(Error::InvalidArgument, QStringLiteral("The desktop finish action requires the current executable and profile")); return result;
    }
    AppInfo effective(g_desktop_app_info_new(captureActionDesktopId));
    if (!effective || !g_app_info_should_show(G_APP_INFO(effective.get()))) {
        refuse(Error::Unavailable, QStringLiteral("The desktop has no visible registered huhucapture application entry")); return result;
    }
    const auto* nativePath = g_desktop_app_info_get_filename(effective.get());
    const auto desktopPath = nativePath ? QString::fromUtf8(nativePath) : QString();
    const auto physicalPath = canonical(desktopPath);
    if (physicalPath.isEmpty()) {
        refuse(Error::Unavailable, QStringLiteral("The registered desktop entry has no persistent file")); return result;
    }
    std::error_code nativeError;
    auto handle = openRegularFileReadOnly(Files::nativePath(physicalPath), nativeError);
    const auto before = handle ? readFileDescriptorSnapshot(handle.get(), nativeError) : std::nullopt;
    if (!before || before->bytes == 0 || before->bytes > maximumDesktopBytes) {
        refuse(Error::Read, QStringLiteral("The registered desktop entry could not be read within its size limit")); return result;
    }
    QFile file;
    if (!file.open(handle.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
        refuse(Error::Read, QStringLiteral("The registered desktop entry could not be opened")); return result;
    }
    const auto bytes = file.read(maximumDesktopBytes + 1);
    const auto after = readFileDescriptorSnapshot(handle.get(), nativeError);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(bytes);
    if (file.error() != QFileDevice::NoError || !file.atEnd() || bytes.size() > maximumDesktopBytes ||
        !after || *after != *before || bytes.size() != static_cast<qsizetype>(before->bytes) ||
        decoder.hasError() || decoded.contains(QChar::Null)) {
        refuse(Error::ChangedOnDisk, QStringLiteral("The registered desktop entry changed or is not complete UTF-8")); return result;
    }
    KeyFile document(g_key_file_new());
    if (!g_key_file_load_from_data(document.get(), bytes.constData(), static_cast<gsize>(bytes.size()),
            G_KEY_FILE_NONE, nullptr)) {
        refuse(Error::Read, QStringLiteral("The registered desktop entry is invalid")); return result;
    }
    result.app.reset(g_desktop_app_info_new_from_keyfile(document.get()));
    if (!result.app || !g_app_info_should_show(G_APP_INFO(result.app.get())) ||
        g_key_file_get_boolean(document.get(), desktopGroup, "Terminal", nullptr) ||
        g_key_file_get_boolean(document.get(), desktopGroup, "DBusActivatable", nullptr)) {
        refuse(Error::Unavailable, QStringLiteral("The desktop finish action is not a direct background command")); return result;
    }
    const gchar* const* actions = g_desktop_app_info_list_actions(result.app.get());
    if (!g_strv_contains(actions, captureActionName)) {
        refuse(Error::Unavailable, QStringLiteral("The registered application does not expose Stop and finish capture")); return result;
    }
    g_autofree gchar* command = g_key_file_get_string(document.get(), actionGroup, "Exec", nullptr);
    const auto arguments = actionArguments(command);
    if (arguments.isEmpty()) {
        refuse(Error::Unavailable, QStringLiteral("The desktop finish action has unsupported command arguments")); return result;
    }
    const auto commandBytes = arguments.front().toUtf8();
    g_autofree gchar* resolved = g_find_program_in_path(commandBytes.constData());
    if (!resolved || canonical(QString::fromUtf8(resolved)) != executable) {
        refuse(Error::Unavailable, QStringLiteral("The desktop finish action points to another executable")); return result;
    }
    bool finish{}, selectedProfile{};
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (!finish && argument == QStringLiteral("--action=finish-capture")) { finish = true; continue; }
        if (!selectedProfile && !profile.isEmpty() && argument.startsWith(QStringLiteral("--portable-dir=")) &&
            canonical(argument.sliced(15)) == profile) { selectedProfile = true; continue; }
        refuse(Error::Unavailable, QStringLiteral("The desktop finish action does not target this exact application profile")); return result;
    }
    if (!finish || selectedProfile != !profile.isEmpty()) {
        refuse(Error::Unavailable, QStringLiteral("The desktop finish action does not target this exact application profile")); return result;
    }
    const auto executableIdentity = readFileSnapshot(Files::nativePath(executable), nativeError);
    const auto currentDesktop = readFileSnapshot(Files::nativePath(physicalPath), nativeError);
    if (!executableIdentity || !currentDesktop || *currentDesktop != *before || canonical(desktopPath) != physicalPath) {
        refuse(Error::ChangedOnDisk, QStringLiteral("The desktop entry or executable changed during verification")); return result;
    }
    g_autofree gchar* label = g_desktop_app_info_get_action_name(result.app.get(), captureActionName);
    result.registration = {desktopPath, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), *before,
        executable, *executableIdentity, profile, label ? QString::fromUtf8(label) : QString(), {}};
    auto& paths = result.registration.watchPaths;
    paths = {desktopPath, physicalPath, QFileInfo(desktopPath).absolutePath(), QFileInfo(physicalPath).absolutePath(),
        executable, QFileInfo(executable).absolutePath()};
    const auto directories = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    if (directories.size() > 32) {
        refuse(Error::Unavailable, QStringLiteral("Too many desktop-entry search directories")); return result;
    }
    for (const auto& directory : directories) {
        if (!QDir::isAbsolutePath(directory)) continue;
        auto path = QDir(directory).filePath(QStringLiteral("applications"));
        while (!QFileInfo(path).isDir()) {
            const auto parent = QFileInfo(path).absolutePath();
            if (parent == path) break;
            path = parent;
        }
        if (QFileInfo(path).isDir()) paths.push_back(path);
    }
    paths.removeDuplicates();
    if (stop.stop_requested()) result.failure = cancelled();
    return result;
}
}

struct CaptureActionMonitor::Private {
    GAppInfoMonitor* monitor{};
    gulong handler{};
    QThread* thread{};
    ~Private()
    {
        Q_ASSERT(!thread || QThread::currentThread() == thread);
        if (monitor) g_clear_signal_handler(&handler, monitor);
        g_clear_object(&monitor);
    }
};

CaptureActionMonitor::CaptureActionMonitor(std::function<void()> changed) : d(std::make_unique<Private>())
{
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() || !changed) return;
    const auto* dispatcher = QAbstractEventDispatcher::instance();
    auto* context = g_main_context_get_thread_default();
    if (!dispatcher || !dispatcher->inherits("QEventDispatcherGlib") || (context && context != g_main_context_default())) return;
    d->thread = QThread::currentThread();
    d->monitor = g_app_info_monitor_get();
    if (!d->monitor) return;
    using Callback = std::shared_ptr<std::function<void()>>;
    auto callback = std::make_unique<Callback>(std::make_shared<std::function<void()>>(std::move(changed)));
    d->handler = g_signal_connect_data(d->monitor, "changed", G_CALLBACK(+[](GAppInfoMonitor* monitor, gpointer data) {
        // A host may destroy the service/monitor from this notification. Keep
        // both the callback and native emitter alive until the call returns.
        const auto callback = *static_cast<Callback*>(data);
        g_autoptr(GAppInfoMonitor) keep = G_APP_INFO_MONITOR(g_object_ref(monitor));
        (*callback)();
    }), callback.get(), +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); }, G_CONNECT_DEFAULT);
    if (d->handler) callback.release();
}
CaptureActionMonitor::~CaptureActionMonitor() = default;
bool CaptureActionMonitor::available() const noexcept { return d->monitor && d->handler; }

CaptureActionNativeResult inspectCaptureAction(const QString& executable, const QString& profile, std::stop_token stop)
{
    auto located = locate(executable, profile, stop);
    if (located.failure) return {{}, std::move(located.failure)};
    return {std::move(located.registration), {}};
}

CaptureActionNativeResult launchCaptureActionProbe(const QString& executable, const QString& profile,
    const QByteArray& challenge, std::stop_token stop)
{
    if (challenge.size() != 16 || std::all_of(challenge.begin(), challenge.end(), [](char byte) { return byte == 0; }))
        return {{}, failed(Error::InvalidArgument, QStringLiteral("The desktop-action proof must be a nonzero 16-byte challenge"))};
    auto located = locate(executable, profile, stop);
    if (located.failure) return {{}, std::move(located.failure)};
    g_autoptr(GCancellable) cancellation = g_cancellable_new();
    std::stop_callback onStop(stop, [cancellation] { g_cancellable_cancel(cancellation); });
    g_autoptr(GError) nativeError = nullptr;
    // GIO's public launch_action obtains this same cached bus internally. Do
    // connection setup on the worker with cancellation, never on the GUI.
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellation, &nativeError);
    if (!bus) return {{}, stop.stop_requested() ? cancelled() : failed(Error::Unavailable,
        nativeError ? QString::fromUtf8(nativeError->message) : QStringLiteral("The desktop session bus is unavailable"))};
    g_dbus_connection_set_exit_on_close(bus, false);
    if (stop.stop_requested()) return {{}, cancelled()};
    g_autoptr(GAppLaunchContext) context = g_app_launch_context_new();
    const auto encoded = challenge.toHex();
    g_app_launch_context_setenv(context, captureActionProbeEnvironment, encoded.constData());
    // This API intentionally returns void. Only the primary's matching one-use
    // IPC receipt can confirm a route; this call alone never advertises one.
    g_desktop_app_info_launch_action(located.app.get(), captureActionName, context);
    if (stop.stop_requested()) return {{}, cancelled()};
    auto current = locate(executable, profile, stop);
    if (current.failure) return {{}, std::move(current.failure)};
    if (current.registration != located.registration)
        return {{}, failed(Error::ChangedOnDisk, QStringLiteral("The desktop finish action changed while it was launched"))};
    return {std::move(current.registration), {}};
}

} // namespace HuhuCapture::Platform::Linux
