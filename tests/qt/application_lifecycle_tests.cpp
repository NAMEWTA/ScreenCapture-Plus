#include "app/QtApplication.h"
#include "app/CaptureWorkspace.h"
#include "app/LanController.h"
#include "app/MediaWorkspace.h"
#include "app/RecordingWorkspace.h"
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

namespace app = HuhuCapture::Application;
namespace core = HuhuCapture::Core;
namespace desktop = HuhuCapture::Desktop;
namespace media = HuhuCapture::Media;
namespace ui = HuhuCapture::UI::Qt;
namespace localization = HuhuCapture::Localization;

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
void verifyDrained(app::QtApplication& host)
{
    if (const auto* value = host.findChild<app::CaptureWorkspace*>()) require(!value->busy(), "Exit waits for real capture callbacks");
    if (const auto* value = host.findChild<app::RecordingWorkspace*>()) require(!value->busy(), "Exit waits for real recording work");
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
    explicit Observation(app::QtApplication& host)
    {
        QObject::connect(&host, &app::QtApplication::started, this, [this, &host](app::InstanceRole value) {
            role = value;
            if (auto* clipboard = host.findChild<desktop::SystemClipboard*>())
                QObject::connect(clipboard, &desktop::SystemClipboard::shutdownFinished, this, [this] { ++clipboardStops; });
        });
        QObject::connect(&host, &app::QtApplication::operationFailed, this, [this] { ++errors; });
        QObject::connect(&host, &app::QtApplication::exitReady, this, [this, &host](int code) {
            verifyDrained(host); exitCode = code; ++exits;
        });
    }
};

void argumentContracts()
{
    const auto ordinary = app::parseApplicationArguments({});
    require(ordinary.failure.isEmpty() && !ordinary.startup.oneShot && !ordinary.startup.background &&
        ordinary.startup.entry == core::CaptureEntry::Capture, "The real Qt CLI defaults to the inherited screenshot action");
    for (const auto& mode : {QStringLiteral("--auto-start"), QStringLiteral("--enter=tray")})
        require(app::parseApplicationArguments({mode}).startup.background, "Actual background CLI never requests an initial screenshot");
    const auto independent = app::parseApplicationArguments({QStringLiteral("--auto-start"), QStringLiteral("--auto-quit"),
        QStringLiteral("--enter=qr"), QStringLiteral("--portable-dir=/tmp/配置 profile")});
    require(independent.failure.isEmpty() && independent.startup.oneShot && !independent.startup.background &&
        independent.startup.entry == core::CaptureEntry::Qr && independent.portableDirectory == QStringLiteral("/tmp/配置 profile"),
        "One-shot priority, inherited QR entry and complete Unicode profile use one production parser");
    require(!app::parseApplicationArguments({QStringLiteral("--auto-quit=TRUE")}).startup.oneShot &&
        !app::parseApplicationArguments({QStringLiteral("--auto-start=false"), QStringLiteral("--auto-start")}).startup.background,
        "Qt entry retains exact legacy boolean and repeated bare-flag semantics");
    require(!app::parseApplicationArguments({QStringLiteral("--portable-dir")}).failure.isEmpty() &&
        !app::parseApplicationArguments({QStringLiteral("--portable-dir=")}).failure.isEmpty(),
        "An incomplete explicit profile never silently selects another configuration");
    for (const auto& profile : {QStringLiteral("--portable-dir"), QStringLiteral("--portable-dir=")}) {
        for (const auto& arguments : {QStringList{profile, QStringLiteral("--action=finish-capture")},
            QStringList{QStringLiteral("--action=finish-capture"), profile}}) {
            const auto rejected = app::parseApplicationArguments(arguments);
            require(!rejected.failure.isEmpty() && rejected.startup.action == core::StartupAction::FinishCapture,
                "A malformed control profile must retain its control action and cannot open an ordinary startup dialog");
        }
    }
}

void invalidProfile(QApplication& application)
{
    app::ApplicationOptions options; options.portableDirectory = QStringLiteral("relative-profile");
    app::QtApplication host(application, options); Observation observed(host);
    require(host.start(app::parseApplicationArguments({QStringLiteral("--auto-quit")}).startup), "The host owns the rejected-path error journey");
    waitFor([&] { return window(QStringLiteral("applicationError")); }, "An invalid explicit profile has an actual visible error");
    require(!host.findChild<app::MediaWorkspace*>() && !host.findChild<app::LanController*>(), "Invalid profile construction starts no business or LAN service");
    acknowledgeErrors(); waitFor([&] { return observed.exits == 1; }, "The acknowledged startup failure drains and exits");
    require(observed.exitCode == 1 && observed.errors == 1, "The actual rejected profile returns failure rather than success");
}

void damagedConfiguration(QApplication& application)
{
    QTemporaryDir directory; require(directory.isValid(), "Create a real malformed-profile fixture");
    const auto path = QDir(directory.path()).filePath(QStringLiteral("config.json"));
    const QByteArray original("{\"common\":broken,\"media\":\"retain all data\"}"); write(path, original);
    app::ApplicationOptions options; options.portableDirectory = directory.path();
    app::QtApplication host(application, options); Observation observed(host);
    require(host.start(app::parseApplicationArguments({QStringLiteral("--auto-quit")}).startup), "Start the actual damaged-config path");
    waitFor([&] { return window(QStringLiteral("applicationError")); }, "Malformed persisted data is visibly reported");
    require(read(path) == original && !host.findChild<app::MediaWorkspace*>(), "Startup does not replace damaged user bytes or open an unconfigured workspace");
    acknowledgeErrors(); waitFor([&] { return observed.exits == 1; }, "Damaged configuration closes through the ordinary exit coordinator");
    require(observed.exitCode == 1 && read(path) == original, "The error exit retains exact original configuration bytes");
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

void independentAndDrain(QApplication& application, const QString& artifacts)
{
    QTemporaryDir directory; require(directory.isValid(), "Create an actual one-shot profile"); profile(directory.path(), true);
    const auto path = QDir(directory.path()).filePath(QStringLiteral("config.json")); const auto original = read(path);
    app::ApplicationOptions options; options.portableDirectory = directory.path();
    app::QtApplication host(application, options); Observation observed(host);
    require(host.start(app::parseApplicationArguments({QStringLiteral("--auto-quit"), QStringLiteral("--auto-start")}).startup), "Start actual independent capture despite background flag");
    waitFor([&] { return observed.role == app::InstanceRole::Independent && window(QStringLiteral("applicationError")); },
        "The actual offscreen capture is explicitly unavailable inside the independent journey");
    auto* capture = host.findChild<app::CaptureWorkspace*>(); auto* clipboard = host.findChild<desktop::SystemClipboard*>();
    require(capture && clipboard && !capture->capabilities().screenshot.canRequest(), "Production capability reports a genuinely unavailable desktop source");
    require(!host.findChild<app::SettingsController*>() && !host.findChild<app::LanController*>() && !host.findChild<QSystemTrayIcon*>() &&
        !host.findChild<HuhuCapture::Updates::UpdateService*>(),
        "One-shot creates no shortcut/settings controller, LAN runtime, updater or tray even when saved LAN is enabled");
    require(host.findChildren<desktop::SystemClipboard*>().size() == 1 && host.findChildren<app::MediaWorkspace*>().size() == 1 &&
        host.findChildren<localization::LanguageCatalog*>().size() == 1, "Independent journeys still reuse one shared application service of each kind");
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
    token = clipboard->publish(prepared.content, nullptr); require(token && clipboard->busy(), "The host now owns an unfinished actual validation future");
    acknowledgeErrors(); host.requestQuit();
    waitFor([&] { return receipts == 1; }, "Normal application shutdown settles the request receipt");
    require(host.phase() == app::ApplicationPhase::Draining && !observed.exits && !observed.clipboardStops && clipboard->busy(),
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
    ~Child() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); } }
    void start(const QProcessEnvironment& environment, const QStringList& arguments)
    {
        process.setProcessEnvironment(environment); process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QCoreApplication::applicationFilePath(), arguments);
        require(process.waitForStarted(3000), "Start a real independent application-host process");
    }
    void collect()
    {
        output += process.readAll(); require(output.size() <= 1024 * 1024, "Child diagnostics stay bounded");
        if (process.state() == QProcess::NotRunning && process.exitCode() != 0)
            throw std::runtime_error(std::string("Actual host child failed: ") + output.toStdString());
    }
    void until(const QByteArray& marker)
    {
        waitFor([&] { collect(); return output.contains(marker); }, "Actual child did not reach its expected lifecycle marker", 15000);
    }
    void complete()
    {
        waitFor([&] { collect(); return process.state() == QProcess::NotRunning; }, "Actual child did not drain and exit", 15000);
        collect(); require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, "Real application-host child exits normally");
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
    primary.until("CAPTURE_FINISHED");
    Child independent;
    independent.start(environment, {QStringLiteral("--lifecycle-child=independent"), profileArg, QStringLiteral("--auto-quit"), QStringLiteral("--auto-start")});
    independent.complete(); require(independent.output.contains("INDEPENDENT"), "Actual one-shot process bypasses the live primary and owns its own failed-source journey");
    require(primary.process.state() == QProcess::Running, "One-shot completion does not stop the resident primary");
    write(marker, "stop"); primary.complete();
    require(primary.output.contains("CLIPBOARD_DRAINED") && primary.output.indexOf("CLIPBOARD_DRAINED") < primary.output.indexOf("EXIT_READY 0"),
        "The resident host observes the real clipboard barrier before its final exit");
    std::cout << "Actual CLI child processes passed Primary/Forwarded/Independent, visible no-tray entry and asynchronous exit.\n";
}

int childMain(QApplication& application, const QStringList& arguments)
{
    const auto expected = argument(arguments, QStringLiteral("--lifecycle-child="));
    const auto stopPath = argument(arguments, QStringLiteral("--lifecycle-stop="));
    const auto artifacts = argument(arguments, QStringLiteral("--lifecycle-artifacts="));
    const auto parsed = app::parseApplicationArguments(arguments.mid(1)); require(parsed.failure.isEmpty(), "Child uses complete production CLI arguments");
    app::ApplicationOptions options; options.portableDirectory = parsed.portableDirectory;
    app::QtApplication host(application, options);
    int result = 2; bool sawRole{}, clipboardDrained{};
    QObject::connect(&host, &app::QtApplication::operationFailed, &host, [&](const QString& message) {
        std::cerr << "Actual host operation failed: " << message.toStdString() << '\n' << std::flush;
        QTimer::singleShot(0, &host, acknowledgeErrors);
    });
    QObject::connect(&host, &app::QtApplication::started, &host, [&](app::InstanceRole role) {
        sawRole = true;
        if (role == app::InstanceRole::Forwarded) {
            require(expected == QStringLiteral("forwarded") && !host.findChild<app::MediaWorkspace*>() && !host.findChild<app::SettingsController*>(),
                "A forwarded actual process has no duplicate application services");
            std::cout << "FORWARDED\n" << std::flush; return;
        }
        auto* clipboard = host.findChild<desktop::SystemClipboard*>(); require(clipboard, "Owned role constructs its one real clipboard service");
        QObject::connect(clipboard, &desktop::SystemClipboard::shutdownFinished, &host, [&] {
            clipboardDrained = true; std::cout << "CLIPBOARD_DRAINED\n" << std::flush;
        });
        if (role == app::InstanceRole::Independent) {
            require(expected == QStringLiteral("independent") && !host.findChild<app::SettingsController*>() && !host.findChild<app::LanController*>() &&
                !host.findChild<QSystemTrayIcon*>(), "An independent CLI child owns no resident-only service");
            std::cout << "INDEPENDENT\n" << std::flush; return;
        }
        require(role == app::InstanceRole::Primary && expected == QStringLiteral("primary"), "The actual background child acquires Primary");
        require(!host.findChild<app::CaptureWorkspace*>()->busy(), "Actual --auto-start creates no capture journey");
        QObject::connect(host.findChild<app::CaptureWorkspace*>(), &app::CaptureWorkspace::captureFinished, &host,
            [] { std::cout << "CAPTURE_FINISHED\n" << std::flush; });
        require(!QSystemTrayIcon::isSystemTrayAvailable(), "This process validates a genuinely unavailable system tray");
        auto* entry = window(QStringLiteral("applicationEntry")); require(entry && entry->isVisible(), "A no-tray primary provides a visible shared entry and Exit");
        require(entry->findChild<QPushButton*>(QStringLiteral("applicationCapture"))->isEnabled() &&
            entry->findChild<QPushButton*>(QStringLiteral("applicationExit"))->isEnabled(), "The real fallback exposes working capture and exit controls");
        render(*entry, artifacts, QStringLiteral("application-entry-en-us"));
        entry->findChild<QPushButton*>(QStringLiteral("applicationSettings"))->click();
        auto* settings = window(QStringLiteral("settingsWindow")); require(settings && settings->isVisible(), "Fallback Settings opens the existing production settings window");
        settings->close();
        require(host.findChildren<app::SettingsController*>().size() == 1 && host.findChildren<app::LanController*>().size() == 1 &&
            host.findChildren<app::MediaWorkspace*>().size() == 1, "Resident entry actions reuse one application controller of each kind");
        require(!host.findChild<app::LanController*>()->settingsSnapshot().enabled, "Default-disabled LAN does not start a listener when its entry exists");
        std::cout << "PRIMARY_READY\n" << std::flush;
    });
    QObject::connect(&host, &app::QtApplication::exitReady, &host, [&](int code) {
        verifyDrained(host);
        require(sawRole && (expected == QStringLiteral("forwarded") || clipboardDrained), "Child exit follows its actual role and service-drain signal");
        result = code; std::cout << "EXIT_READY " << code << '\n' << std::flush; application.exit(code);
    });
    QTimer control;
    if (!stopPath.isEmpty()) {
        control.setInterval(25);
        QObject::connect(&control, &QTimer::timeout, &host, [&] { if (QFileInfo::exists(stopPath)) { control.stop(); host.requestQuit(); } });
        control.start();
    }
    QTimer deadline; deadline.setSingleShot(true); deadline.setInterval(30000);
    QObject::connect(&deadline, &QTimer::timeout, &host, [&] { std::cerr << "Actual host child exceeded its lifecycle deadline\n"; application.exit(3); });
    deadline.start(); require(host.start(parsed.startup), "Actual child host accepts its parsed CLI request"); application.exec(); return result;
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
void clipboardOwner(QApplication& application, const QString& artifacts)
{
    require(QGuiApplication::platformName() == QStringLiteral("xcb"), "Clipboard-owner retention needs an actual X11 desktop, never offscreen");
    QTemporaryDir directory; require(directory.isValid(), "Create actual owner-retention profile"); profile(directory.path());
    QWidget origin; origin.setWindowTitle(QStringLiteral("huhucapture actual clipboard origin")); origin.resize(360, 180); origin.show(); events();
    app::ApplicationOptions options; options.portableDirectory = directory.path();
    app::QtApplication host(application, options); Observation observed(host);
    QObject::connect(&host, &app::QtApplication::operationFailed, &host, [&] { QTimer::singleShot(0, &host, acknowledgeErrors); });
    require(host.start(app::parseApplicationArguments({QStringLiteral("--auto-quit")}).startup), "Start actual one-shot host for clipboard retention");
    waitFor([&] { return observed.role == app::InstanceRole::Independent; }, "The actual independent host initializes its services");
    const auto value = QStringLiteral("huhucapture 保留剪贴板\nUnicode \U0001f680\t完整内容");
    auto prepared = desktop::prepareText(value); require(!prepared.failure && prepared.content, "Prepare real complete Unicode payload");
    auto* clipboard = host.findChild<desktop::SystemClipboard*>(); int receipts{}; quint64 token{};
    QObject::connect(clipboard, &desktop::SystemClipboard::finished, &host, [&](quint64 request, const desktop::ClipboardResult& result) {
        if (request != token) return;
        require(result.completed(), "The actual X server confirms ownership of the published text"); ++receipts;
    });
    token = clipboard->publish(prepared.content, origin.windowHandle());
    waitFor([&] { return receipts == 1; }, "Actual native clipboard publication completes");
    host.requestQuit();
    waitFor([&] { acknowledgeErrors(); return host.phase() == app::ApplicationPhase::ClipboardOwner; }, "Normal exit preserves a visible owner after all actual services drain");
    origin.close(); events();
    auto* entry = window(QStringLiteral("applicationEntry")); require(entry && entry->isVisible() && !observed.exits && observed.clipboardStops == 1,
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
        const auto arguments = application.arguments();
        if (!argument(arguments, QStringLiteral("--lifecycle-child=")).isEmpty()) return childMain(application, arguments);
#ifndef Q_OS_WIN
        const auto clipboard = argument(arguments, QStringLiteral("--lifecycle-read-clipboard="));
        if (!clipboard.isEmpty()) return clipboardReceiver(clipboard);
#endif
        const auto artifacts = arguments.size() > 1 && !arguments[1].startsWith(QStringLiteral("--")) ?
            QDir(arguments[1]).absolutePath() : QDir::current().filePath(QStringLiteral("artifacts/tests/application"));
#ifndef Q_OS_WIN
        if (arguments.contains(QStringLiteral("--clipboard-owner"))) { clipboardOwner(application, artifacts); return 0; }
#endif
        require(QGuiApplication::platformName() == QStringLiteral("offscreen"), "Lifecycle failure/entry checks use an actually unavailable desktop backend");
        argumentContracts(); invalidProfile(application); events(); damagedConfiguration(application); events();
        independentAndDrain(application, artifacts); events();
        if (!arguments.contains(QStringLiteral("--no-ipc"))) processContracts(artifacts);
        std::cout << "Actual application argument, profile protection, independent no-backend and async drain contracts passed.\n";
        if (arguments.contains(QStringLiteral("--no-ipc"))) std::cout << "Real cross-process IPC explicitly not run in this invocation.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "application_lifecycle_tests: " << failure.what() << '\n'; return 1;
    }
}
