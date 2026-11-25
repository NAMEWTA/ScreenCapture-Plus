#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include "app/SingleInstance.h"
#include "platform/linux/DesktopCaptureAction.h"
#include "services/desktop/DesktopCaptureAction.h"
#include "services/desktop/LaunchCommand.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using namespace HuhuCapture;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
QString entryPath(const QString& data)
{ return QDir(data).filePath(QStringLiteral("applications/io.github.NAMEWTA.huhucapture.desktop")); }
void writeEntry(const QString& data, const QString& executable, const QStringList& arguments,
    const char* extraKey = nullptr, bool extraValue = false, bool includeAction = true)
{
    g_autoptr(GKeyFile) key = g_key_file_new();
    g_key_file_set_string(key, "Desktop Entry", "Type", "Application");
    g_key_file_set_string(key, "Desktop Entry", "Name", "huhucapture native action test");
    const auto mainCommand = Desktop::desktopExecCommand(QCoreApplication::applicationFilePath(), {}).toUtf8();
    g_key_file_set_string(key, "Desktop Entry", "Exec", mainCommand.constData());
    g_key_file_set_boolean(key, "Desktop Entry", "Terminal", false);
    g_key_file_set_boolean(key, "Desktop Entry", "StartupNotify", false);
    if (extraKey) g_key_file_set_boolean(key, "Desktop Entry", extraKey, extraValue);
    if (includeAction) g_key_file_set_string(key, "Desktop Entry", "Actions", "FinishCapture;");
    g_key_file_set_string(key, "Desktop Action FinishCapture", "Name", "Stop and finish capture");
    const auto command = Desktop::desktopExecCommand(executable, arguments).toUtf8();
    g_key_file_set_string(key, "Desktop Action FinishCapture", "Exec", command.constData());
    gsize length{};
    g_autofree gchar* bytes = g_key_file_to_data(key, &length, nullptr);
    check(bytes && length, "The real GLib desktop entry could not be encoded");
    QSaveFile file(entryPath(data));
    check(file.open(QIODevice::WriteOnly) && file.write(bytes, static_cast<qint64>(length)) == static_cast<qint64>(length) && file.commit(),
        "The isolated desktop entry could not be written");
}
int client(QCoreApplication& app, Core::StartupRequest request)
{
    check(!Desktop::DesktopCaptureAction::applyProbeEnvironment(request) &&
        request.action == Core::StartupAction::VerifyCaptureAction, "The actual GIO child did not receive a valid verification environment");
    Application::SingleInstance instance(qEnvironmentVariable("HUHU_ACTION_TEST_RUNTIME"), qEnvironmentVariable("HUHU_ACTION_TEST_SCOPE"),
        [](const auto&) { check(false, "A verification child elected itself or admitted a new capture"); return false; });
    QObject::connect(&instance, &Application::SingleInstance::startupFinished, &app, [&](const Application::InstanceResult& result) {
        QJsonObject evidence{{"role", int(result.role)}, {"failure", int(result.failure)},
            {"action", int(request.action)}, {"pid", QCoreApplication::applicationPid()},
            {"verificationEnvironmentRemoved", !qEnvironmentVariableIsSet(Platform::Linux::captureActionProbeEnvironment)}};
        QSaveFile file(qEnvironmentVariable("HUHU_ACTION_TEST_RECEIPT"));
        const auto bytes = QJsonDocument(evidence).toJson();
        check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "The real child could not preserve its IPC acknowledgment");
        app.exit(result.role == Application::InstanceRole::Forwarded ? 0 : 2);
    });
    check(instance.start(request), "The real GIO verification child could not request forwarding");
    QTimer::singleShot(8000, &app, [] { check(false, "The real verification child did not receive an acknowledgment"); });
    return app.exec();
}
void registrationCases(const QString& data, const QString& root)
{
    const auto executable = QCoreApplication::applicationFilePath();
    const QStringList finish{QStringLiteral("--action=finish-capture")};
    writeEntry(data, executable, finish);
    auto result = Platform::Linux::inspectCaptureAction(executable, {});
    check(result.registration && !result.failure && result.registration->desktopPath == entryPath(data) &&
        result.registration->executable == QFileInfo(executable).canonicalFilePath(), "The actual GIO registry did not resolve the exact finish action");
    const auto original = *result.registration;
    for (const auto* key : {"Terminal", "NoDisplay", "Hidden", "DBusActivatable"}) {
        writeEntry(data, executable, finish, key, true);
        result = Platform::Linux::inspectCaptureAction(executable, {});
        check(result.failure && !result.registration, "An unusable registered desktop action was accepted");
    }
    writeEntry(data, executable, finish, nullptr, false, false);
    check(bool(Platform::Linux::inspectCaptureAction(executable, {}).failure), "A missing desktop action was accepted");
    writeEntry(data, QStringLiteral("/bin/true"), finish);
    check(bool(Platform::Linux::inspectCaptureAction(executable, {}).failure), "Another executable was accepted as this instance");
    writeEntry(data, executable, {QStringLiteral("--action=finish-capture"), QStringLiteral("--auto-quit")});
    check(bool(Platform::Linux::inspectCaptureAction(executable, {}).failure), "An unrelated action argument was accepted");
    const auto profile = QDir(root).filePath(QStringLiteral("profile"));
    check(QDir().mkpath(profile), "The isolated portable profile could not be created");
    writeEntry(data, executable, {QStringLiteral("--action=finish-capture"), QStringLiteral("--portable-dir=") + profile});
    check(bool(Platform::Linux::inspectCaptureAction(executable, {}).failure), "Another profile was accepted as this instance");
    result = Platform::Linux::inspectCaptureAction(executable, profile);
    check(result.registration && !result.failure && result.registration->portableDirectory == profile,
        "The real registered action could not bind its explicit portable profile");
    writeEntry(data, executable, finish);
    std::stop_source stop; stop.request_stop();
    check(Platform::Linux::inspectCaptureAction(executable, {}, stop.get_token()).failure.code == Desktop::Error::Cancelled,
        "Registration ignored actual prior cancellation");
    result = Platform::Linux::inspectCaptureAction(executable, {});
    check(result.registration && !result.failure && result.registration->desktopIdentity != original.desktopIdentity,
        "Replacing the real registered entry did not change its identity");
    check(QFile::remove(entryPath(data)), "The isolated desktop entry could not be removed");
    check(bool(Platform::Linux::inspectCaptureAction(executable, {}).failure), "A removed desktop entry retained availability");
    writeEntry(data, executable, finish);
    std::cout << "PASS actual GIO registry resolution, exact executable/profile, hidden/terminal/DBus/action rejection, cancellation and physical replacement\n";
}

int monitorOverride(QCoreApplication& app, const QString& data, const QString& system)
{
    const auto executable = QCoreApplication::applicationFilePath();
    const QStringList finish{QStringLiteral("--action=finish-capture")};
    check(QDir().mkpath(QFileInfo(entryPath(system)).absolutePath()), "The isolated system applications directory could not be created");
    writeEntry(system, executable, finish);
    int changes{};
    bool selectedOverride{}, restoredSystem{}, callbackDestroyedMonitor{};
    std::unique_ptr<Platform::Linux::CaptureActionMonitor> monitor;
    monitor = std::make_unique<Platform::Linux::CaptureActionMonitor>([&] {
        check(QThread::currentThread() == app.thread(), "GIO delivered its registry notification outside the actual Qt GLib context");
        ++changes;
        const auto effective = Platform::Linux::inspectCaptureAction(executable, {});
        check(effective.registration && !effective.failure, "The actual GIO cache could not be refreshed after a registry notification");
        if (!selectedOverride) {
            check(effective.registration->desktopPath == entryPath(data), "GIO did not select the newly created higher-priority desktop entry");
            selectedOverride = true;
            // The inspect above re-arms GIO's own directory monitor. Remove the
            // real override on the next GUI turn and require a second native
            // notification; no synthetic file or GObject event is delivered.
            QTimer::singleShot(0, &app, [&] {
                check(QFile::remove(entryPath(data)), "The isolated user override could not be removed");
            });
            return;
        }
        check(effective.registration->desktopPath == entryPath(system), "GIO did not restore the lower-priority entry after removal");
        restoredSystem = true;
        monitor.reset();
        callbackDestroyedMonitor = true;
        QTimer::singleShot(0, &app, [&] {
            const QJsonObject evidence{{"passed", true}, {"scope", "actual GIO registry notification; no GNOME shell UI or IPC"},
                {"selectedHigherPriorityOverride", selectedOverride}, {"restoredSystemEntry", restoredSystem},
                {"destroyedMonitorInNativeCallback", callbackDestroyedMonitor}, {"nativeChanges", changes},
                {"glib", QStringLiteral("%1.%2.%3").arg(glib_major_version).arg(glib_minor_version).arg(glib_micro_version)}};
            std::cout << QJsonDocument(evidence).toJson(QJsonDocument::Compact).toStdString() << '\n';
            app.quit();
        });
    });
    check(monitor->available(), "The native GIO registry monitor could not attach to the actual Qt GLib context");
    const auto original = Platform::Linux::inspectCaptureAction(executable, {});
    check(original.registration && !original.failure && original.registration->desktopPath == entryPath(system),
        "The real registry did not initially resolve the system entry");
    // This file did not exist at the first lookup. There is deliberately no
    // QFileSystemWatcher here: GIO's official registration notification must
    // close the cached lookup / later Qt watch installation window itself.
    writeEntry(data, executable, finish);
    QTimer::singleShot(8000, &app, [] { check(false, "Actual GIO registry invalidation did not complete within its deadline"); });
    return app.exec();
}
}

int main(int argc, char** argv)
{
    std::vector<std::string_view> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    const auto request = Core::parseStartupArguments<char>(arguments);
    if (request.action != Core::StartupAction::Launch) {
        QCoreApplication app(argc, argv);
        return client(app, request);
    }
    QTemporaryDir temporary;
    check(temporary.isValid(), "The isolated registry directory could not be created");
    const auto root = temporary.path();
    const auto data = QDir(root).filePath(QStringLiteral("data"));
    const auto system = QDir(root).filePath(QStringLiteral("system"));
    const auto runtime = QDir(root).filePath(QStringLiteral("runtime"));
    check(QDir().mkpath(QDir(data).filePath(QStringLiteral("applications"))) && QDir().mkpath(system) && QDir().mkpath(runtime),
        "The isolated desktop registry and runtime could not be created");
    qputenv("XDG_DATA_HOME", data.toUtf8()); qputenv("XDG_DATA_DIRS", system.toUtf8());
    // Controls only GIO's public OnlyShowIn matching, not a fake GNOME service.
    // This test never claims a GNOME shell menu was displayed or activated.
    qputenv("XDG_CURRENT_DESKTOP", "GNOME");
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--monitor-override"))) return monitorOverride(app, data, system);
    registrationCases(data, root);
    if (!app.arguments().contains(QStringLiteral("--gio-roundtrip"))) return 0;

    const auto scope = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto proof = QUuid::createUuid().toRfc4122();
    const auto receipt = QDir(root).filePath(QStringLiteral("child-receipt.json"));
    qputenv("HUHU_ACTION_TEST_RUNTIME", runtime.toUtf8());
    qputenv("HUHU_ACTION_TEST_SCOPE", scope.toUtf8());
    qputenv("HUHU_ACTION_TEST_RECEIPT", receipt.toUtf8());
    int received{}, stops{}, heartbeats{};
    bool workerFinished{}, childAcknowledged{}, replayStarted{};
    Platform::Linux::CaptureActionNativeResult launched;
    QFutureWatcher<Platform::Linux::CaptureActionNativeResult> worker;
    std::unique_ptr<Application::SingleInstance> replay;
    Application::SingleInstance primary(runtime, scope, [&](const Core::StartupRequest& activation) {
        if (activation.action == Core::StartupAction::FinishCapture) ++stops;
        const bool exact = activation.action == Core::StartupAction::VerifyCaptureAction &&
            std::equal(activation.verification.begin(), activation.verification.end(), reinterpret_cast<const std::uint8_t*>(proof.constData()));
        if (!exact || received) return false;
        ++received;
        return true;
    });
    QObject::connect(&primary, &Application::SingleInstance::startupFinished, &app, [&](const Application::InstanceResult& result) {
        check(result.role == Application::InstanceRole::Primary, "The real local IPC primary is unavailable in this environment");
        const auto executable = QCoreApplication::applicationFilePath();
        worker.setFuture(QtConcurrent::run([executable, proof] {
            return Platform::Linux::launchCaptureActionProbe(executable, {}, proof, {});
        }));
    });
    QObject::connect(&worker, &QFutureWatcher<Platform::Linux::CaptureActionNativeResult>::finished, &app, [&] {
        launched = worker.result(); workerFinished = true;
        if (launched.failure) std::cerr << launched.failure.detail.toStdString() << '\n';
        check(!launched.failure && launched.registration, "The real GIO action could not be launched");
    });
    QTimer progress;
    progress.setInterval(5);
    QObject::connect(&progress, &QTimer::timeout, &app, [&] {
        ++heartbeats;
        if (!childAcknowledged && QFileInfo::exists(receipt)) {
            QFile file(receipt);
            check(file.open(QIODevice::ReadOnly), "The actual child receipt could not be opened");
            const auto object = QJsonDocument::fromJson(file.readAll()).object();
            check(object.value(QStringLiteral("role")).toInt() == int(Application::InstanceRole::Forwarded) &&
                object.value(QStringLiteral("action")).toInt() == int(Core::StartupAction::VerifyCaptureAction) &&
                object.value(QStringLiteral("pid")).toInteger() > 0 &&
                object.value(QStringLiteral("pid")).toInteger() != QCoreApplication::applicationPid() &&
                object.value(QStringLiteral("verificationEnvironmentRemoved")).toBool(), "The spawned child did not receive the actual matching IPC acknowledgment");
            childAcknowledged = true;
        }
        if (!workerFinished || !childAcknowledged || replayStarted) return;
        check(received == 1 && stops == 0 && heartbeats > 0, "Route verification stopped a capture, repeated admission or blocked dispatch");
        replayStarted = true;
        replay = std::make_unique<Application::SingleInstance>(runtime, scope, [](const auto&) { return false; });
        QObject::connect(replay.get(), &Application::SingleInstance::startupFinished, &app, [&](const Application::InstanceResult& result) {
            check(result.role == Application::InstanceRole::Failed && result.failure == Application::InstanceFailure::Rejected && received == 1,
                "An already consumed verification was accepted again");
            QJsonObject evidence{{"passed", true}, {"scope", "real GIO action and primary IPC; no GNOME shell UI"},
                {"actualChildAcknowledged", childAcknowledged}, {"verificationAdmissions", received},
                {"realFinishActions", stops}, {"replayRejected", true}, {"heartbeats", heartbeats},
                {"glib", QStringLiteral("%1.%2.%3").arg(glib_major_version).arg(glib_minor_version).arg(glib_micro_version)}};
            std::cout << QJsonDocument(evidence).toJson(QJsonDocument::Compact).toStdString() << '\n';
            app.quit();
        });
        Core::StartupRequest retry; retry.action = Core::StartupAction::VerifyCaptureAction;
        std::copy(proof.begin(), proof.end(), retry.verification.begin());
        check(replay->start(retry), "The real replay peer could not start");
    });
    progress.start();
    check(primary.start({}), "The real IPC primary could not start");
    QTimer::singleShot(15000, &app, [] { check(false, "The real GIO/action/IPC route did not complete within its deadline"); });
    return app.exec();
}
