#include "app/DesktopApplication.h"
#include "app/SingleInstance.h"
#include "app/LanController.h"
#include "app/MediaWorkspace.h"
#include "app/RecordingController.h"
#include "app/ScrollController.h"
#include "services/capture/DesktopCapture.h"
#include "app/SettingsController.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "services/localization/LanguageCatalog.h"
#include "services/media/MediaArchiveService.h"
#include "services/update/UpdateService.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/SettingsWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSemaphore>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace app = HuhuCapture::Application;
namespace core = HuhuCapture::Core;
namespace desktop = HuhuCapture::Desktop;
namespace media = HuhuCapture::Media;
namespace ui = HuhuCapture::UI::Qt;
namespace localization = HuhuCapture::Localization;
namespace capture = HuhuCapture::Capture;

static void initializeResources() { Q_INIT_RESOURCE(huhucapture_ui); }

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void events()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void waitFor(const std::function<bool()>& predicate, const char* message, int timeout = 12000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) { events(); QThread::msleep(1); }
    require(predicate(), message);
}
void write(const QString& path, const QByteArray& content)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Create an owned test directory");
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Open an explicit owned test file");
    require(file.write(content) == content.size() && file.flush(), "Write complete actual fixture bytes");
}
QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read an actual owned fixture");
    const auto content = file.readAll(); require(file.error() == QFileDevice::NoError, "Read the complete fixture"); return content;
}
QString argument(const QStringList& arguments, const QString& prefix)
{
    QString value;
    for (const auto& item : arguments) if (item.startsWith(prefix)) value = item.mid(prefix.size());
    return value;
}
QWidget* window(const QString& name)
{
    for (auto* candidate : QApplication::topLevelWidgets()) if (candidate->objectName() == name) return candidate;
    return nullptr;
}
void acknowledgeErrors()
{
    for (auto* candidate : QApplication::topLevelWidgets()) {
        auto* message = qobject_cast<QMessageBox*>(candidate);
        if (message && message->objectName() == QStringLiteral("applicationError") && message->isVisible())
            message->button(QMessageBox::Ok)->click();
    }
}
void render(QWidget& widget, const QString& directory, const QString& name)
{
    require(QDir().mkpath(directory), "Create application UI artifact directory"); events();
    const auto pixels = widget.grab(); require(!pixels.isNull(), "Render the actual production Qt widget");
    require(pixels.save(QDir(directory).filePath(name + QStringLiteral(".png"))), "Save real application UI PNG evidence");
}
void profile(const QString& directory, bool enabledLan = false)
{
    write(QDir(directory).filePath(QStringLiteral("config.json")),
        enabledLan ? QByteArrayLiteral("{\"_huhucapture\":{\"schema\":1},\"common\":{\"language\":\"en-US\",\"autoStart\":true},\"shortcutKey\":{\"cap\":\"Ctrl+Alt+F9\"},\"lan\":{\"enabled\":true},\"future\":\"retain\"}") :
        QByteArrayLiteral("{\"_huhucapture\":{\"schema\":1},\"common\":{\"language\":\"en-US\"},\"lan\":{\"enabled\":false},\"future\":\"retain\"}"));
}
void verifyDrained(app::DesktopApplication& host)
{
    if (const auto* value = host.findChild<capture::DesktopCapture*>()) require(!value->busy(), "Exit waits for real capture callbacks");
    if (const auto* value = host.findChild<app::RecordingController*>()) require(!value->busy(), "Exit waits for real recording work");
    if (const auto* value = host.findChild<app::ScrollController*>()) require(!value->busy(), "Exit waits for real scroll work");
    if (const auto* value = host.findChild<app::MediaWorkspace*>()) require(!value->busy(), "Exit waits for real media work");
    if (const auto* value = host.findChild<desktop::SystemClipboard*>()) require(!value->busy(), "Exit waits for real clipboard work");
    if (const auto* value = host.findChild<desktop::FileRevealer*>()) require(!value->busy(), "Exit waits for actual file-manager work");
    if (const auto* value = host.findChild<app::SettingsController*>()) require(!value->busy(), "Exit waits for native shortcut callbacks");
    if (const auto* value = host.findChild<app::LanController*>()) require(!value->busy(), "Exit waits for the actual LAN controller");
    if (const auto* value = host.findChild<HuhuCapture::Updates::UpdateService*>())
        require(value->snapshot().stopped && !value->busy(), "Exit waits for actual update network/worker shutdown");
}

struct Observation : QObject {
    int exits{}, exitCode{-1}, errors{}, clipboardStops{};
    app::InstanceRole role{app::InstanceRole::Pending};
    explicit Observation(app::DesktopApplication& host)
    {
        QObject::connect(&host, &app::DesktopApplication::started, this, [this, &host]() {
            role = host.findChild<app::SingleInstance*>()->role();
            if (auto* clipboard = host.findChild<desktop::SystemClipboard*>())
                QObject::connect(clipboard, &desktop::SystemClipboard::shutdownFinished, this, [this] { ++clipboardStops; });
        });
        QObject::connect(&host, &app::DesktopApplication::operationFailed, this, [this] { ++errors; });
        QObject::connect(&host, &app::DesktopApplication::exitReady, this, [this, &host](int code) {
            if (const auto* instance = host.findChild<app::SingleInstance*>()) role = instance->role();
            verifyDrained(host); exitCode = code; ++exits;
        });
    }
};

app::DesktopApplicationPaths paths(const QString& directory)
{
    app::DesktopApplicationPaths result;
    result.settingsFile = QDir(directory).filePath(QStringLiteral("config.json"));
    result.dataDirectory = QDir(directory).filePath(QStringLiteral("data"));
    result.runtimeDirectory = QDir(directory).filePath(QStringLiteral("runtime"));
    result.archiveDirectory = QDir(directory).filePath(QStringLiteral("Documents/huhucapture"));
    result.persistentExecutable = QApplication::applicationFilePath();
    result.portableDirectory = directory;
    require(QDir().mkpath(result.runtimeDirectory), "Create actual private runtime directory");
    require(QFile::setPermissions(result.runtimeDirectory,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "Set actual private runtime permissions");
    return result;
}
core::StartupRequest startup(const QStringList& arguments)
{
    // This is the shared production request parser, not a second application
    // entry point. The harness owns only its explicit isolated directory.
    std::vector<std::u16string> storage;
    for (const auto& value : arguments) storage.push_back(value.toStdU16String());
    std::vector<std::u16string_view> views;
    for (const auto& value : storage) views.emplace_back(value);
    return core::parseStartupArguments<char16_t>(views);
}
void verifyIndependentServices(app::DesktopApplication& host)
{
    auto* settings = host.findChild<app::SettingsController*>();
    auto* lan = host.findChild<app::LanController*>();
    require(settings && !settings->captureShortcutAvailable() && lan && !lan->available() &&
        lan->service().state() == HuhuCapture::Transfer::ServiceState::Disabled &&
        !host.findChild<QSystemTrayIcon*>() && !host.findChild<HuhuCapture::Updates::UpdateService*>(),
        "One-shot retains shared read-only-capable UI services without a shortcut, LAN runtime, updater or tray");
    require(host.findChildren<desktop::SystemClipboard*>().size() == 1 && host.findChildren<app::MediaWorkspace*>().size() == 1 &&
        host.findChildren<localization::LanguageCatalog*>().size() == 1 && host.findChildren<app::SettingsController*>().size() == 1 &&
        host.findChildren<app::LanController*>().size() == 1,
        "Independent journeys reuse the application's one service of each kind");
}
void damagedConfiguration()
{
    QTemporaryDir directory; require(directory.isValid(), "Create a real malformed-profile fixture");
    const auto locations = paths(directory.path());
    const QByteArray original("{\"common\":broken,\"media\":\"retain all data\"}"); write(locations.settingsFile, original);
    app::DesktopApplication host(locations); Observation observed(host);
    QObject::connect(&host, &app::DesktopApplication::started, &host, [&] { host.showSettings(); });
    require(host.start(startup({QStringLiteral("--auto-quit")})), "Start the actual damaged-config path");
    waitFor([&] { return host.ready() && window(QStringLiteral("applicationError")); }, "Malformed persisted data is visibly reported");
    auto* settings = window(QStringLiteral("settingsWindow"));
    auto* language = settings ? settings->findChild<QComboBox*>(QStringLiteral("settingsLanguage")) : nullptr;
    require(host.findChild<app::MediaWorkspace*>() && settings && settings->isVisible() && language && !language->isEnabled(),
        "The actual damaged-config host remains usable with read-only settings and its shared media workspace");
    require(read(locations.settingsFile) == original, "Startup never replaces damaged user bytes");
    verifyIndependentServices(host);
    acknowledgeErrors(); settings->close(); host.requestExit();
    waitFor([&] { return observed.exits == 1; }, "Read-only configuration closes through the ordinary exit coordinator");
    require(observed.exitCode == 0 && read(locations.settingsFile) == original,
        "The recoverable configuration error retains exact original bytes through normal exit");
}

class WorkerBarrier final {
public:
    WorkerBarrier() : maximum_(QThreadPool::globalInstance()->maxThreadCount()), gate_(std::make_shared<Gate>())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        task_ = QtConcurrent::run([gate = gate_] { gate->entered.release(); gate->expired = !gate->released.tryAcquire(1, 10000); });
        waitFor([&] { return gate_->entered.available(); }, "A real worker occupies the validation pool");
    }
    ~WorkerBarrier() { release(); task_.waitForFinished(); QThreadPool::globalInstance()->setMaxThreadCount(maximum_); }
    void release() { if (!released_) { released_ = true; gate_->released.release(); } }
    bool expired() const { return gate_->expired; }
private:
    struct Gate { QSemaphore entered, released; std::atomic_bool expired{}; };
    int maximum_; std::shared_ptr<Gate> gate_; QFuture<void> task_; bool released_{};
};

void independentAndDrain(const QString& artifacts)
{
    QTemporaryDir directory; require(directory.isValid(), "Create an actual one-shot profile"); profile(directory.path(), true);
    const auto path = QDir(directory.path()).filePath(QStringLiteral("config.json")); const auto original = read(path);
    app::DesktopApplication host(paths(directory.path())); Observation observed(host);
    require(host.start(startup({QStringLiteral("--auto-quit"), QStringLiteral("--auto-start")})), "Start actual independent capture despite background flag");
    waitFor([&] { return observed.role == app::InstanceRole::Independent && window(QStringLiteral("applicationError")); },
        "The actual offscreen capture is explicitly unavailable inside the independent journey");
    auto* capture = host.findChild<capture::DesktopCapture*>(); auto* clipboard = host.findChild<desktop::SystemClipboard*>();
    require(capture && clipboard && !capture->capability().canRequest(), "Production capability reports a genuinely unavailable desktop source");
    verifyIndependentServices(host);
    require(!QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("lan"))) && read(path) == original,
        "One-shot never generates a saved LAN identity or changes persisted startup preferences");
    render(*window(QStringLiteral("applicationError")), artifacts, QStringLiteral("application-source-unavailable"));

    // Supply an actual isolated archived PNG to the host's sole clipboard.
    // The fixture archive is test data, not a second production controller.
    media::MediaArchiveService fixture(QDir(directory.path()).filePath(QStringLiteral("fixture-media")));
    QImage pixels(29, 17, QImage::Format_ARGB32); pixels.fill(QColor(44, 88, 132, 180));
    require(fixture.archiveImage(pixels).durable(), "Create a complete durable file for the real host drain test");
    const auto entries = fixture.list(); require(!entries.failure && entries.entries.size() == 1, "Read actual fixture archive entry");
    auto lease = fixture.lease(entries.entries.front());
    auto prepared = desktop::prepareFile(std::move(lease.lease)); require(!prepared.failure && prepared.content, "Prepare actual PNG bytes and read reservation");
    WorkerBarrier barrier;
    int receipts{};
    quint64 token{};
    QObject::connect(clipboard, &desktop::SystemClipboard::finished, &host, [&](quint64 received, const desktop::ClipboardResult& result) {
        if (received != token) return;
        require(result.failure.code == desktop::Error::Cancelled, "Application exit cancels the pending publication with its real receipt"); ++receipts;
    });
    token = clipboard->publish(std::move(prepared.content), nullptr); require(token && clipboard->busy(), "The host now owns an unfinished actual validation future");
    acknowledgeErrors(); host.requestExit();
    waitFor([&] { return receipts == 1; }, "Normal application shutdown settles the request receipt");
    require(host.shuttingDown() && !observed.exits && !observed.clipboardStops && clipboard->busy(),
        "The host keeps its dispatcher alive after cancellation while the real worker remains unfinished");
    require(fixture.reserveRemoval(entries.entries.front()).failure.code == media::Error::Busy, "The unfinished worker still protects the actual media file");
    barrier.release(); waitFor([&] { return observed.exits == 1; }, "Host exit follows actual clipboard shutdownFinished and service drain");
    require(!barrier.expired() && observed.exitCode == 0 && observed.clipboardStops == 1 && receipts == 1,
        "Normal one-shot exit waits for exactly one real clipboard shutdown receipt");
    require(!fixture.reserveRemoval(entries.entries.front()).failure && read(path) == original, "Exit releases actual read leases and retains the user's configuration");
}

class Child final {
public:
    QProcess process;
    QByteArray output;
    int expectedExit{};
    ~Child() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); } }
    void start(const QProcessEnvironment& environment, const QStringList& arguments, int code = 0)
    {
        expectedExit = code;
        process.setProcessEnvironment(environment); process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QCoreApplication::applicationFilePath(), arguments);
        require(process.waitForStarted(3000), "Start a real independent application-host process");
    }
    void collect()
    {
        output += process.readAll(); require(output.size() <= 1024 * 1024, "Child diagnostics stay bounded");
        if (process.state() == QProcess::NotRunning && process.exitCode() != expectedExit)
            throw std::runtime_error(std::string("Actual host child failed: ") + output.toStdString());
    }
    void until(const QByteArray& marker)
    {
        waitFor([&] { collect(); return output.contains(marker); }, "Actual child did not reach its expected lifecycle marker", 15000);
    }
    void complete()
    {
        waitFor([&] { collect(); return process.state() == QProcess::NotRunning; }, "Actual child did not drain and exit", 15000);
        collect(); require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == expectedExit, "Real application-host child returns its documented result");
    }
};

void processContracts(const QString& artifacts)
{
    QTemporaryDir directory; require(directory.isValid(), "Create actual cross-process profile/runtime fixture");
    const auto profilePath = QDir(directory.path()).filePath(QStringLiteral("profile")); profile(profilePath);
    const auto runtime = QDir(directory.path()).filePath(QStringLiteral("runtime")); require(QDir().mkpath(runtime), "Create actual private XDG runtime");
    require(QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "Set actual private runtime mode");
    const auto marker = QDir(directory.path()).filePath(QStringLiteral("stop-primary"));
    auto environment = QProcessEnvironment::systemEnvironment(); environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
#ifndef Q_OS_WIN
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), QDir(directory.path()).filePath(QStringLiteral("xdg-config")));
    environment.insert(QStringLiteral("XDG_DATA_HOME"), QDir(directory.path()).filePath(QStringLiteral("xdg-data")));
#endif
    const auto profileArg = QStringLiteral("--portable-dir=") + profilePath;
    Child primary;
    primary.start(environment, {QStringLiteral("--lifecycle-child=primary"), profileArg, QStringLiteral("--auto-start"),
        QStringLiteral("--lifecycle-stop=") + marker, QStringLiteral("--lifecycle-artifacts=") + artifacts});
    primary.until("PRIMARY_READY");
    Child forwarded; forwarded.start(environment, {QStringLiteral("--lifecycle-child=forwarded"), profileArg});
    forwarded.complete(); require(forwarded.output.contains("FORWARDED"), "Default CLI forwards to the existing real primary without constructing business services");
    primary.until("CAPTURE_UNAVAILABLE");
    const auto capturesBeforeControl = primary.output.count("CAPTURE_UNAVAILABLE");
    Child control;
    control.start(environment, {QStringLiteral("--lifecycle-child=control"), profileArg,
        QStringLiteral("--action=finish-capture"), QStringLiteral("--auto-quit")}, 2);
    control.complete(); require(control.output.contains("CONTROL_REJECTED"),
        "A real finish child contacts the primary and rejects its absent recording/scroll owner without starting a capture");
    primary.collect(); require(primary.output.count("CAPTURE_UNAVAILABLE") == capturesBeforeControl &&
        primary.process.state() == QProcess::Running, "Rejected control neither launches another screenshot nor stops the primary");
    Child independent;
    independent.start(environment, {QStringLiteral("--lifecycle-child=independent"), profileArg, QStringLiteral("--auto-quit"), QStringLiteral("--auto-start")});
    independent.complete(); require(independent.output.contains("INDEPENDENT"), "Actual one-shot process bypasses the live primary and owns its own failed-source journey");
    require(primary.process.state() == QProcess::Running, "One-shot completion does not stop the resident primary");
    write(marker, "stop"); primary.complete();
    require(primary.output.contains("CLIPBOARD_DRAINED") && primary.output.indexOf("CLIPBOARD_DRAINED") < primary.output.indexOf("EXIT_READY 0"),
        "The resident host observes the real clipboard barrier before its final exit");
    Child absentControl;
    absentControl.start(environment, {QStringLiteral("--lifecycle-child=control"), profileArg,
        QStringLiteral("--action=finish-capture")}, 2);
    absentControl.complete(); require(absentControl.output.contains("CONTROL_REJECTED"),
        "A control-only process does not elect a new primary after the actual owner exits");
    std::cout << "Actual host child processes passed Primary/Forwarded/Independent and control rejection, visible no-tray entry and asynchronous exit.\n";
}

int childMain(QApplication& application, const QStringList& arguments)
{
    const auto expected = argument(arguments, QStringLiteral("--lifecycle-child="));
    const auto stopPath = argument(arguments, QStringLiteral("--lifecycle-stop="));
    const auto artifacts = argument(arguments, QStringLiteral("--lifecycle-artifacts="));
    const auto profileDirectory = argument(arguments, QStringLiteral("--portable-dir="));
    require(QFileInfo(profileDirectory).isAbsolute() && QFileInfo(profileDirectory).isDir(), "Child receives its actual isolated profile directory");
    require(expected == QStringLiteral("primary") || expected == QStringLiteral("forwarded") ||
        expected == QStringLiteral("independent") || expected == QStringLiteral("control"), "Unknown lifecycle child role is rejected");
    const auto request = startup(arguments.mid(1));
    app::DesktopApplication host(paths(profileDirectory));
    int result = 3; bool sawOwner{}, clipboardDrained{};
    QObject::connect(&host, &app::DesktopApplication::operationFailed, &host, [&](const QString& message) {
        require(expected != QStringLiteral("control"), "A control child never opens an ordinary error dialog");
        auto* source = host.findChild<capture::DesktopCapture*>();
        if (expected == QStringLiteral("primary") && source && !source->capability().canRequest() &&
            message == source->capability().reason) std::cout << "CAPTURE_UNAVAILABLE\n" << std::flush;
        std::cerr << "Actual host operation failed: " << message.toStdString() << '\n' << std::flush;
        QTimer::singleShot(0, &host, acknowledgeErrors);
    });
    QObject::connect(&host, &app::DesktopApplication::started, &host, [&] {
        sawOwner = true;
        auto* instance = host.findChild<app::SingleInstance*>(); require(instance, "Owned role has one actual instance coordinator");
        auto* clipboard = host.findChild<desktop::SystemClipboard*>(); require(clipboard, "Owned role constructs its one real clipboard service");
        QObject::connect(clipboard, &desktop::SystemClipboard::shutdownFinished, &host, [&] {
            clipboardDrained = true; std::cout << "CLIPBOARD_DRAINED\n" << std::flush;
        });
        if (instance->role() == app::InstanceRole::Independent) {
            require(expected == QStringLiteral("independent"), "Only the explicit one-shot child acquires Independent");
            verifyIndependentServices(host);
            std::cout << "INDEPENDENT\n" << std::flush; return;
        }
        require(instance->role() == app::InstanceRole::Primary && expected == QStringLiteral("primary"), "The actual background child acquires Primary");
        require(host.findChild<capture::DesktopCapture*>() && !host.findChild<capture::DesktopCapture*>()->busy(),
            "Actual --auto-start creates no capture request");
        require(!QSystemTrayIcon::isSystemTrayAvailable(), "This process validates a genuinely unavailable system tray");
        auto* entry = window(QStringLiteral("applicationLauncher")); require(entry && entry->isVisible(), "A no-tray primary provides a visible shared entry and Exit");
        auto* captureButton = entry->findChild<QPushButton*>(QStringLiteral("launcherCapture"));
        auto* exitButton = entry->findChild<QPushButton*>(QStringLiteral("launcherExit"));
        auto* settingsButton = entry->findChild<QPushButton*>(QStringLiteral("launcherSettings"));
        require(captureButton && captureButton->isEnabled() && exitButton && exitButton->isEnabled() && settingsButton,
            "The real fallback exposes working capture, settings and exit controls");
        render(*entry, artifacts, QStringLiteral("application-entry-en-us"));
        settingsButton->click();
        auto* settings = window(QStringLiteral("settingsWindow")); require(settings && settings->isVisible(), "Fallback Settings opens the existing production settings window");
        settings->close();
        require(host.findChildren<app::SettingsController*>().size() == 1 && host.findChildren<app::LanController*>().size() == 1 &&
            host.findChildren<app::MediaWorkspace*>().size() == 1, "Resident entry actions reuse one application controller of each kind");
        require(!host.findChild<app::LanController*>()->settingsSnapshot().enabled, "Default-disabled LAN does not start a listener when its entry exists");
        std::cout << "PRIMARY_READY\n" << std::flush;
    });
    QObject::connect(&host, &app::DesktopApplication::exitReady, &host, [&](int code) {
        verifyDrained(host);
        const auto* instance = host.findChild<app::SingleInstance*>(); require(instance, "Every attempted child has its actual instance result");
        if (expected == QStringLiteral("forwarded") || expected == QStringLiteral("control")) {
            require(!sawOwner && !host.ready() && !host.findChild<app::MediaWorkspace*>() &&
                !host.findChild<desktop::SystemClipboard*>() && !host.findChild<app::SettingsController*>() &&
                !host.findChild<app::LanController*>() && !host.findChild<capture::DesktopCapture*>(),
                "Forwarded and control-only children construct no second set of business services");
            for (const auto* widget : QApplication::topLevelWidgets()) require(!widget->isVisible(), "A non-owner child opens no ordinary application window");
            if (expected == QStringLiteral("forwarded")) {
                require(code == 0 && instance->role() == app::InstanceRole::Forwarded, "Actual launch forwarding receives the existing primary acknowledgement");
                std::cout << "FORWARDED\n" << std::flush;
            } else {
                require(code == 2 && instance->role() == app::InstanceRole::Failed &&
                    request.action == core::StartupAction::FinishCapture,
                    "Idle or absent primary rejects a genuine finish request without acquiring ownership");
                require(!host.findChild<localization::LanguageCatalog*>(), "A control child starts no localization/settings application journey");
                std::cout << "CONTROL_REJECTED\n" << std::flush;
            }
        } else require(sawOwner && clipboardDrained && code == 0, "Owned child exit follows the actual clipboard service-drain signal");
        result = code; std::cout << "EXIT_READY " << code << '\n' << std::flush; application.exit(code);
    });
    QTimer control;
    if (!stopPath.isEmpty()) {
        control.setInterval(25);
        QObject::connect(&control, &QTimer::timeout, &host, [&] { if (QFileInfo::exists(stopPath)) { control.stop(); host.requestExit(); } });
        control.start();
    }
    QTimer deadline; deadline.setSingleShot(true); deadline.setInterval(30000);
    QObject::connect(&deadline, &QTimer::timeout, &host, [&] { std::cerr << "Actual host child exceeded its lifecycle deadline\n"; application.exit(3); });
    deadline.start(); require(host.start(request), "Actual child host accepts its production-parsed startup request"); application.exec(); return result;
}

#ifndef Q_OS_WIN
int clipboardReceiver(const QString& encoded)
{
    const auto expected = QString::fromUtf8(QByteArray::fromBase64(encoded.toLatin1()));
    require(!expected.isEmpty(), "Receiver has a complete expected Unicode value");
    waitFor([] { return QApplication::clipboard()->mimeData()->hasText(); }, "A real independent receiver sees the retained clipboard offer");
    require(QApplication::clipboard()->text() == expected, "A separate process receives complete Unicode from the visible retained owner");
    std::cout << "VISIBLE_OWNER_RECEIVED\n"; return 0;
}
void clipboardOwner(const QString& artifacts)
{
    require(QGuiApplication::platformName() == QStringLiteral("xcb"), "Clipboard-owner retention needs an actual X11 desktop, never offscreen");
    QTemporaryDir directory; require(directory.isValid(), "Create actual owner-retention profile"); profile(directory.path());
    QWidget origin; origin.setWindowTitle(QStringLiteral("huhucapture actual clipboard origin")); origin.resize(360, 180); origin.show(); events();
    app::DesktopApplication host(paths(directory.path())); Observation observed(host);
    QObject::connect(&host, &app::DesktopApplication::operationFailed, &host, [&] { QTimer::singleShot(0, &host, acknowledgeErrors); });
    require(host.start(startup({QStringLiteral("--auto-quit")})), "Start actual one-shot host for clipboard retention");
    waitFor([&] { return observed.role == app::InstanceRole::Independent; }, "The actual independent host initializes its services");
    const auto value = QStringLiteral("huhucapture 保留剪贴板\nUnicode \U0001f680\t完整内容");
    auto prepared = desktop::prepareText(value); require(!prepared.failure && prepared.content, "Prepare real complete Unicode payload");
    auto* clipboard = host.findChild<desktop::SystemClipboard*>(); int receipts{}; quint64 token{};
    QObject::connect(clipboard, &desktop::SystemClipboard::finished, &host, [&](quint64 request, const desktop::ClipboardResult& result) {
        if (request != token) return;
        require(result.completed(), "The actual X server confirms ownership of the published text"); ++receipts;
    });
    token = clipboard->publish(std::move(prepared.content), origin.windowHandle());
    waitFor([&] { return receipts == 1; }, "Actual native clipboard publication completes");
    host.requestExit();
    waitFor([&] { acknowledgeErrors(); return host.shuttingDown() && window(QStringLiteral("applicationClipboardOwner")) &&
        window(QStringLiteral("applicationClipboardOwner"))->isVisible(); }, "Normal exit preserves a visible owner after all actual services drain");
    origin.close(); events();
    auto* entry = window(QStringLiteral("applicationClipboardOwner")); require(entry && entry->isVisible() && !observed.exits && observed.clipboardStops == 1,
        "One-shot never silently exits or becomes a hidden clipboard owner");
    auto* keep = entry->findChild<QPushButton*>(QStringLiteral("applicationKeepClipboard"));
    auto* exit = entry->findChild<QPushButton*>(QStringLiteral("applicationExit"));
    require(keep && keep->isVisible() && keep->isEnabled() && exit && exit->isVisible(), "The same shared Qt window gives explicit Keep and Exit choices");
    keep->click(); require(!keep->isEnabled() && !observed.exits, "Choosing Keep retains the visible owner without claiming process-exit persistence");
    render(*entry, artifacts, QStringLiteral("application-clipboard-owner-en-us"));
    Child receiver; receiver.start(QProcessEnvironment::systemEnvironment(), {
        QStringLiteral("--lifecycle-read-clipboard=") + QString::fromLatin1(value.toUtf8().toBase64())});
    receiver.complete(); require(receiver.output.contains("VISIBLE_OWNER_RECEIVED"), "Actual independent receiver pastes while the retained window is open");
    exit->click(); waitFor([&] { return observed.exits == 1; }, "The user's explicit Exit releases the final process-exit hold");
    require(observed.exitCode == 0, "Explicit owner exit uses the normal drained exit result");
}
#endif
}

int main(int argc, char** argv)
{
    try {
        QApplication application(argc, argv); application.setApplicationName(QStringLiteral("huhucapture"));
        application.setOrganizationName({}); application.setOrganizationDomain(QStringLiteral("github.com"));
        application.setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture")); application.setQuitOnLastWindowClosed(false);
        initializeResources();
        const auto arguments = application.arguments();
        if (!argument(arguments, QStringLiteral("--lifecycle-child=")).isEmpty()) return childMain(application, arguments);
#ifndef Q_OS_WIN
        const auto clipboard = argument(arguments, QStringLiteral("--lifecycle-read-clipboard="));
        if (!clipboard.isEmpty()) return clipboardReceiver(clipboard);
#endif
        const auto artifacts = arguments.size() > 1 && !arguments[1].startsWith(QStringLiteral("--")) ?
            QDir(arguments[1]).absolutePath() : QDir::current().filePath(QStringLiteral("artifacts/tests/application"));
#ifndef Q_OS_WIN
        if (arguments.contains(QStringLiteral("--clipboard-owner"))) { clipboardOwner(artifacts); return 0; }
#endif
        require(QGuiApplication::platformName() == QStringLiteral("offscreen"), "Lifecycle failure/entry checks use an actually unavailable desktop backend");
        damagedConfiguration(); events();
        independentAndDrain(artifacts); events();
        if (!arguments.contains(QStringLiteral("--no-ipc"))) processContracts(artifacts);
        std::cout << "Actual DesktopApplication profile protection, independent no-backend and async drain contracts passed.\n";
        if (arguments.contains(QStringLiteral("--no-ipc"))) std::cout << "Real cross-process IPC explicitly not run in this invocation.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "application_lifecycle_tests: " << failure.what() << '\n'; return 1;
    }
}
