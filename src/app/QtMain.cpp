#include "app/DesktopApplication.h"
#include "app/Version.h"
#include "services/config/SettingsStore.h"
#include "services/desktop/LaunchCommand.h"
#include "services/media/MediaArchiveService.h"

#include <QApplication>
#include <QDir>
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
    const auto args = app.arguments().mid(1);
    storage.reserve(args.size());
    for (const auto& argument : args) {
        storage.push_back(argument.toStdU16String());
        if (argument.startsWith(QStringLiteral("--portable-dir="))) portable = argument.mid(15);
    }
    std::vector<std::u16string_view> views;
    for (const auto& argument : storage) views.emplace_back(argument);
    const auto request = HuhuCapture::Core::parseStartupArguments<char16_t>(views);
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
    const auto mount = QDir::cleanPath(qEnvironmentVariable("APPDIR"));
    const auto portablePath = QDir::cleanPath(portable);
    const auto portableCanonical = QFileInfo(portable).canonicalFilePath();
    const auto mountCanonical = QFileInfo(mount).canonicalFilePath();
    const auto inside = [](const QString& path, const QString& parent) {
        return !parent.isEmpty() && (path == parent || path.startsWith(parent + QChar(u'/')));
    };
    const bool insideMount = !qEnvironmentVariable("APPDIR").isEmpty() &&
        (inside(portablePath, mount) || (!portableCanonical.isEmpty() && inside(portableCanonical, mountCanonical)));
    if (!portable.isEmpty() && (!QDir::isAbsolutePath(portable) || insideMount)) {
        QMessageBox::critical(nullptr, QStringLiteral("huhucapture"),
            QStringLiteral("--portable-dir requires an absolute writable directory outside the AppImage mount."));
        return 2;
    }
    paths.settingsFile = HuhuCapture::Config::settingsPath(policy, applicationDirectory, userConfig, portable);
    const std::array candidates{
        QDir(applicationDirectory).filePath(QStringLiteral("installation.json")),
        QDir(applicationDirectory).filePath(QStringLiteral("../share/huhucapture/installation.json")),
        QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("huhucapture/installation.json"))};
    for (const auto& candidate : candidates)
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile()) { paths.installationFile = candidate; break; }

    HuhuCapture::Application::DesktopApplication desktop(std::move(paths));
    QObject::connect(&desktop, &HuhuCapture::Application::DesktopApplication::exitReady,
        &app, [&app](int code) { app.exit(code); });
    QTimer::singleShot(0, &desktop, [&desktop, request] {
        if (!desktop.start(request)) QCoreApplication::exit(2);
    });
    return app.exec();
}
