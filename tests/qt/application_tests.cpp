#include "app/DesktopApplication.h"
#include "services/config/SettingsStore.h"
#include "ui/qt/SettingsWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace HuhuCapture;
static void initializeResources() { Q_INIT_RESOURCE(huhucapture_ui); }
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void settle()
{ QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
void waitFor(const std::function<bool()>& condition, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 15000) { settle(); QThread::msleep(1); }
    require(condition(), message);
}
Application::DesktopApplicationPaths paths(const QString& root)
{
    Application::DesktopApplicationPaths result;
    result.settingsFile = root + QStringLiteral("/config/config.json");
    result.dataDirectory = root + QStringLiteral("/data");
    result.runtimeDirectory = root + QStringLiteral("/runtime");
    result.archiveDirectory = root + QStringLiteral("/Documents/huhucapture");
    result.persistentExecutable = QApplication::applicationFilePath();
    require(QDir().mkpath(result.runtimeDirectory), "Create actual owned runtime directory");
    QFile::setPermissions(result.runtimeDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return result;
}
void oneShot(const QString& root, bool invalidSettings)
{
    auto locations = paths(root);
    const QByteArray original("{broken configuration\n");
    if (invalidSettings) {
        require(QDir().mkpath(QFileInfo(locations.settingsFile).absolutePath()), "Create corrupt configuration fixture directory");
        QFile file(locations.settingsFile); require(file.open(QIODevice::WriteOnly), "Create corrupt configuration fixture");
        require(file.write(original) == original.size(), "Write exact malformed bytes");
    }
    Application::DesktopApplication application(locations);
    unsigned starts{}, exits{}; int code{-1};
    QObject::connect(&application, &Application::DesktopApplication::started, &application, [&] {
        ++starts; application.showSettings(); application.showHistory();
    });
    QObject::connect(&application, &Application::DesktopApplication::exitReady, &application, [&](int result) { ++exits; code = result; });
    Core::StartupRequest request; request.oneShot = true;
    require(application.start(request), "Start actual independent application with production services");
    waitFor([&] { return starts == 1; }, "Actual service assembly finishes");
    require(application.ready(), "Actual application is ready after assembly");
    auto* settings = static_cast<UI::Qt::SettingsWindow*>(nullptr);
    for (auto* view : QApplication::topLevelWidgets())
        if (auto* candidate = qobject_cast<UI::Qt::SettingsWindow*>(view)) settings = candidate;
    require(settings, "Production settings window is connected to application host");
    if (invalidSettings) {
        auto* language = settings->findChild<QComboBox*>(QStringLiteral("settingsLanguage"));
        require(language && !language->isEnabled(), "Corrupt configuration disables editing without replacing it");
    }
    // Close this test process's genuine views as they become visible. Linux's
    // offscreen backend reports unavailable; Windows may finish an actual
    // native snapshot later. Both paths must drain their real workers.
    settle();
    QTimer closeViews;
    QObject::connect(&closeViews, &QTimer::timeout, &application, [] {
        for (auto* view : QApplication::topLevelWidgets()) if (view->isVisible()) view->close();
    });
    closeViews.start(10);
    waitFor([&] { return exits == 1; }, "One-shot exits only after views and actual workers drain");
    closeViews.stop();
    require(code == 0 && application.shuttingDown(), "One-shot reaches coordinated normal shutdown");
    require(!application.activate({}), "Stopped application rejects later capture activation");
    require(!QFile::exists(QFileInfo(locations.settingsFile).absolutePath() + QStringLiteral("/lan/identity.json")),
        "Default-disabled LAN never provisions a transfer identity");
    if (invalidSettings) {
        QFile file(locations.settingsFile); require(file.open(QIODevice::ReadOnly) && file.readAll() == original,
            "Production app preserves exact malformed configuration bytes");
    }
    std::cout << "PASS actual one-shot assembly, settings/history, unavailable capture and coordinated shutdown\n";
}
void primary(const QString& root)
{
    auto locations = paths(root);
    Application::DesktopApplication application(locations);
    unsigned starts{}, exits{};
    QObject::connect(&application, &Application::DesktopApplication::started, &application, [&] { ++starts; });
    QObject::connect(&application, &Application::DesktopApplication::exitReady, &application,
        [&](int code) { require(code == 0, "Primary host exits normally"); ++exits; });
    Core::StartupRequest request; request.background = true;
    require(application.start(request), "Admit actual primary instance");
    waitFor([&] { return starts == 1; }, "Actual local IPC primary becomes ready");
    application.showHistory(); application.showSettings(); settle();
    application.requestExit(); application.requestExit();
    waitFor([&] { return exits == 1; }, "Primary services drain once before exit");
    std::cout << "PASS primary process services, native IPC lock and application-owned shutdown\n";
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setApplicationName(QStringLiteral("huhucapture-application-tests"));
    app.setQuitOnLastWindowClosed(false); initializeResources();
    try {
        QTemporaryDir directory(QDir::temp().filePath(QStringLiteral("huhu-app-XXXXXX")));
        require(directory.isValid(), "Create isolated application files");
        if (app.arguments().contains(QStringLiteral("--primary"))) primary(directory.path());
        else { oneShot(directory.path() + QStringLiteral("/normal"), false); settle(); oneShot(directory.path() + QStringLiteral("/invalid"), true); }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
