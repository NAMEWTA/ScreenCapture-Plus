#include "app/DesktopApplication.h"
#include "app/Version.h"
#include "services/config/SettingsStore.h"
#include "services/desktop/LaunchCommand.h"
#include "services/desktop/DesktopCaptureAction.h"
#include "services/media/MediaArchiveService.h"

#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

static void initializeResources()
{
    Q_INIT_RESOURCE(huhucapture_ui);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("huhucapture"));
    QApplication::setApplicationVersion(QString::fromLatin1(HuhuCapture::Version::text.data(), HuhuCapture::Version::text.size()));
    QApplication::setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture"));
    QApplication::setQuitOnLastWindowClosed(false);
    initializeResources();
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/huhucapture/brand/icon.svg")));

    std::vector<std::u16string> storage;
    QString portable;
    bool portableRequested{};
    const auto args = app.arguments().mid(1);
    storage.reserve(args.size());
    for (const auto& argument : args) {
        storage.push_back(argument.toStdU16String());
        if (argument.startsWith(QStringLiteral("--portable-dir="))) {
            portableRequested = true; portable = argument.mid(15);
        }
    }
    std::vector<std::u16string_view> views;
    for (const auto& argument : storage) views.emplace_back(argument);
    auto request = HuhuCapture::Core::parseStartupArguments<char16_t>(views);
    if (const auto proof = HuhuCapture::Desktop::DesktopCaptureAction::applyProbeEnvironment(request)) {
        qWarning().noquote() << proof.detail; return 2;
    }
    const bool control = request.action != HuhuCapture::Core::StartupAction::Launch;
    if (request.action == HuhuCapture::Core::StartupAction::Invalid) {
        qWarning("The application action is invalid"); return 2;
    }
    const auto applicationDirectory = QApplication::applicationDirPath();
    const auto userConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    HuhuCapture::Application::DesktopApplicationPaths paths;
    paths.dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    paths.runtimeDirectory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    paths.persistentExecutable = qEnvironmentVariable("APPIMAGE");
    if (paths.persistentExecutable.isEmpty()) paths.persistentExecutable = QApplication::applicationFilePath();
    paths.archiveDirectory = HuhuCapture::Media::MediaArchiveService::defaultDirectory();
#ifdef Q_OS_WIN
    constexpr auto policy = HuhuCapture::Config::PathPolicy::WindowsLegacy;
#else
    constexpr auto policy = HuhuCapture::Config::PathPolicy::Linux;
#endif
    const auto resolvedPortable = !portableRequested ? QString() :
        HuhuCapture::Config::resolvePortableDirectory(portable, qEnvironmentVariable("APPDIR"));
    if (portableRequested && resolvedPortable.isEmpty()) {
        if (control) { qWarning("The portable action scope is invalid"); return 2; }
        QMessageBox::critical(nullptr, QStringLiteral("huhucapture"),
            QStringLiteral("--portable-dir requires an absolute writable directory outside the AppImage mount."));
        return 2;
    }
    paths.portableDirectory = resolvedPortable;
    paths.settingsFile = HuhuCapture::Config::settingsPath(policy, applicationDirectory, userConfig, resolvedPortable);
    HuhuCapture::Application::DesktopApplication desktop(std::move(paths));
    QObject::connect(&desktop, &HuhuCapture::Application::DesktopApplication::exitReady,
        &app, [&app](int code) { app.exit(code); });
    QTimer::singleShot(0, &desktop, [&desktop, request] {
        if (!desktop.start(request)) QCoreApplication::exit(2);
    });
    return app.exec();
}
