#include "app/Version.h"
#include "services/localization/LanguageCatalog.h"
#include "services/update/UpdateService.h"
#include "ui/qt/AboutPanel.h"
#include "ui/qt/UpdatePresentation.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace updates = HuhuCapture::Updates;
namespace ui = HuhuCapture::UI::Qt;
namespace localization = HuhuCapture::Localization;

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

template<class Condition> void waitFor(Condition condition, int milliseconds, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!condition()) {
        require(timer.elapsed() < milliseconds, message);
        settle(); QThread::msleep(1);
    }
}

template<class Type> Type* child(QObject& object, const char* name)
{
    auto* result = object.findChild<Type*>(QString::fromLatin1(name));
    require(result != nullptr, "The actual About control exists"); return result;
}

updates::PackageKind kind(const QString& name)
{
#ifdef Q_OS_WIN
    if (name == QStringLiteral("installer")) return updates::PackageKind::WindowsInstaller;
    if (name == QStringLiteral("portable-zip")) return updates::PackageKind::WindowsPortableZip;
#else
    if (name == QStringLiteral("deb")) return updates::PackageKind::LinuxDeb;
    if (name == QStringLiteral("appimage")) return updates::PackageKind::LinuxAppImage;
#endif
    throw std::runtime_error("The complete package kind must match this native platform");
}

QString fileHash(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Open the completed actual package");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file) && file.error() == QFileDevice::NoError,
        "Read every byte of the completed package independently");
    return QString::fromLatin1(hash.result().toHex());
}

void saveReport(const QString& path, const QJsonObject& report)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Create one new complete-package evidence report");
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    require(file.write(bytes) == bytes.size() && file.flush(), "Write the complete-package evidence report");
}
} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("huhucapture-complete-update-probe"));
    const auto version = QString::fromLatin1(HuhuCapture::Version::text.data(),
        static_cast<qsizetype>(HuhuCapture::Version::text.size()));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Real complete-package update and About lifecycle evidence; no installation or execution"));
    parser.addHelpOption();
    for (const auto* name : {"origin", "kind", "downloads", "output", "asset", "sha256", "size"})
        parser.addOption({QString::fromLatin1(name), QString::fromLatin1(name), QStringLiteral("value")});
    parser.process(application);
    QJsonObject report{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("passed"), false},
        {QStringLiteral("compiledVersion"), version}, {QStringLiteral("autoExecuted"), false}};
    const auto output = parser.value(QStringLiteral("output"));
    std::unique_ptr<updates::UpdateService> service;
    bool shutdownFinished = false;
    QElapsedTimer total; total.start();
    try {
        require(parser.positionalArguments().isEmpty(), "Unexpected positional complete-package probe argument");
        for (const auto* name : {"origin", "kind", "downloads", "output", "asset", "sha256", "size"})
            require(parser.isSet(QString::fromLatin1(name)) && !parser.value(QString::fromLatin1(name)).isEmpty(),
                "Every complete-package probe input is explicit");
        const auto origin = QUrl(parser.value(QStringLiteral("origin")));
        require(origin.isValid() && origin.scheme() == QStringLiteral("http") &&
            origin.host() == QStringLiteral("127.0.0.1") && origin.port() > 0 && origin.userInfo().isEmpty() &&
            origin.path().isEmpty() && !origin.hasQuery() && !origin.hasFragment(),
            "The explicit integration server is a literal loopback HTTP origin");
        const auto packageKind = kind(parser.value(QStringLiteral("kind")));
        const auto downloads = QFileInfo(parser.value(QStringLiteral("downloads"))).absoluteFilePath();
        require(QDir().mkpath(downloads) && QDir(downloads).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
            "The native probe has an empty owned download directory");
        bool sizeValid = false;
        const auto expectedSize = parser.value(QStringLiteral("size")).toULongLong(&sizeValid);
        require(sizeValid && expectedSize > 0, "The actual complete package has a positive exact size");
        const auto expectedHash = parser.value(QStringLiteral("sha256"));
        require(expectedHash.size() == 64 && QByteArray::fromHex(expectedHash.toLatin1()).size() == 32,
            "The expected complete package digest is SHA-256");
        localization::LanguageCatalog languages({localization::ResourcePolicy::LinuxUserResources, downloads, downloads});
        require(languages.load(QStringLiteral("en-US")).usable, "Load the actual embedded English About instructions");
        service = std::make_unique<updates::UpdateService>(updates::Options{downloads, version, packageKind, origin});
        QObject::connect(service.get(), &updates::UpdateService::shutdownFinished, &application,
            [&shutdownFinished] { shutdownFinished = true; });
        std::unique_ptr<ui::AboutPanel> page;
        // The connection must disappear before the local page slot on both
        // success and exception unwinding; shutdown still pumps GUI events.
        QObject pageObserver;
        bool admitted = false;
        QString revealed;
        const auto makePage = [&] {
            auto result = std::make_unique<ui::AboutPanel>();
            result->setTranslator([&languages](QStringView key) { return languages.text(key); });
            result->setApplicationVersion(version); result->setLinksEnabled(true);
            result->setUpdateSnapshot(ui::aboutUpdateSnapshot(service->snapshot()));
            QObject::connect(result.get(), &ui::AboutPanel::downloadRequested, service.get(), [&] {
                admitted = service->download();
            });
            QObject::connect(result.get(), &ui::AboutPanel::openUpdateFolderRequested, service.get(),
                [&revealed](const QString& file) { revealed = file; });
            result->resize(780, 640); result->show(); return result;
        };
        page = makePage();
        QObject::connect(service.get(), &updates::UpdateService::changed, &pageObserver,
            [&page](const updates::Snapshot& value) { if (page) page->setUpdateSnapshot(ui::aboutUpdateSnapshot(value)); });
        settle();
        require(child<QPushButton>(*page, "settingsDownloadUpdate")->isEnabled(), "About admits download of the current stable version");
        child<QPushButton>(*page, "settingsDownloadUpdate")->click();
        require(admitted, "The actual About action starts the application-owned production updater");
        waitFor([&] {
            const auto value = service->snapshot();
            require(value.stage != updates::Stage::Failed && value.stage != updates::Stage::Cancelled,
                "The actual complete-package update must reach streaming without failure");
            return value.stage == updates::Stage::Downloading && value.received > 0 && value.received < value.total;
        }, 180000, "Observe actual incomplete package bytes while the production updater streams");
        const auto beforeClose = service->snapshot();
        require(service->busy() && child<QProgressBar>(*page, "settingsUpdateProgress")->isVisible(),
            "About displays real streamed package progress");
        page.reset();
        waitFor([&] {
            const auto value = service->snapshot();
            require(value.stage != updates::Stage::Failed && value.stage != updates::Stage::Cancelled,
                "Closing the About page must not cancel the application-owned download");
            return value.received > beforeClose.received;
        }, 180000, "The download continues after the actual About widget is destroyed");
        page = makePage(); settle();
        const auto restored = service->snapshot();
        require(restored.received > beforeClose.received &&
            (restored.stage == updates::Stage::Downloaded ||
             child<QProgressBar>(*page, "settingsUpdateProgress")->value() ==
                static_cast<int>(static_cast<long double>(restored.received) * 100 / restored.total)),
            "Reopening About restores the real task and current progress");
        waitFor([&] {
            const auto value = service->snapshot();
            require(value.stage != updates::Stage::Failed && value.stage != updates::Stage::Cancelled,
                "The complete package finishes without a transport, identity or integrity failure");
            return value.stage == updates::Stage::Downloaded;
        }, 180000, "The complete package finishes within the bounded native probe deadline");
        const auto completed = service->snapshot();
        require(!service->busy() && completed.version == version && completed.tag == QStringLiteral("v") + version &&
            completed.assetName == parser.value(QStringLiteral("asset")) && completed.packageKind == packageKind &&
            completed.received == expectedSize && completed.total == expectedSize,
            "The actual same-version complete package has the requested identity and every advertised byte");
        require(QFileInfo(completed.file).absolutePath() == downloads &&
            QFileInfo(completed.file).fileName() == completed.assetName &&
            static_cast<quint64>(QFileInfo(completed.file).size()) == expectedSize && fileHash(completed.file) == expectedHash,
            "The published owned download is byte-for-byte the real finalized package");
        const auto shown = ui::aboutUpdateSnapshot(completed);
        const auto instructions = languages.text(shown.installationInstructionsKey);
        require(!instructions.startsWith(QStringLiteral("update.")) &&
            child<QLabel>(*page, "settingsUpdateStatus")->text().contains(instructions) &&
            child<QLabel>(*page, "settingsUpdatePath")->isVisible() &&
            child<QPushButton>(*page, "settingsUpdateFolder")->isEnabled(),
            "About shows the retained file and the actual package's manual installation instructions");
        child<QPushButton>(*page, "settingsUpdateFolder")->click();
        require(revealed == completed.file, "About's folder action identifies precisely the completed package");
        const auto screenshot = QFileInfo(output).absoluteDir().filePath(QStringLiteral("about-complete.png"));
        require(page->grab().save(screenshot), "Save the actual completed About widget for review");
        report.insert(QStringLiteral("packageKind"), updates::packageKindName(packageKind));
        report.insert(QStringLiteral("version"), completed.version); report.insert(QStringLiteral("tag"), completed.tag);
        report.insert(QStringLiteral("asset"), completed.assetName); report.insert(QStringLiteral("file"), completed.file);
        report.insert(QStringLiteral("sha256"), expectedHash);
        report.insert(QStringLiteral("received"), static_cast<qint64>(completed.received));
        report.insert(QStringLiteral("total"), static_cast<qint64>(completed.total));
        report.insert(QStringLiteral("closedAfterBytes"), static_cast<qint64>(beforeClose.received));
        report.insert(QStringLiteral("reopenedAfterBytes"), static_cast<qint64>(restored.received));
        report.insert(QStringLiteral("installationInstructionsKey"), shown.installationInstructionsKey);
        report.insert(QStringLiteral("pageDestroyedWhileDownloading"), true);
        report.insert(QStringLiteral("pageReopenedFromService"), true);
        page.reset(); service->shutdown();
        waitFor([&] { return shutdownFinished; }, 20000, "Shutdown joins the actual updater worker before its completion receipt");
        require(!service->busy(), "No updater work remains after shutdownFinished");
        report.insert(QStringLiteral("shutdownFinished"), true); report.insert(QStringLiteral("passed"), true);
        report.insert(QStringLiteral("elapsedMs"), total.elapsed()); saveReport(output, report);
        std::cout << "Actual complete package and production About lifecycle passed; no installer was executed.\n";
        return 0;
    } catch (const std::exception& error) {
        report.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
        if (service) {
            const auto value = service->snapshot();
            report.insert(QStringLiteral("failureStage"), static_cast<int>(value.stage));
            report.insert(QStringLiteral("failureError"), static_cast<int>(value.error));
            report.insert(QStringLiteral("received"), static_cast<qint64>(value.received));
            report.insert(QStringLiteral("total"), static_cast<qint64>(value.total));
            service->shutdown();
            try { waitFor([&] { return shutdownFinished; }, 20000, "Join failed complete-package probe work"); }
            catch (const std::exception&) { }
        }
        report.insert(QStringLiteral("shutdownFinished"), shutdownFinished);
        report.insert(QStringLiteral("elapsedMs"), total.elapsed());
        try { if (!output.isEmpty()) saveReport(output, report); } catch (const std::exception&) { }
        std::cerr << "Complete-package update validation failed: " << error.what() << '\n'; return 1;
    }
}
