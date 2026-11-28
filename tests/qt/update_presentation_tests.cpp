#include "services/localization/LanguageCatalog.h"
#include "ui/qt/AboutPanel.h"
#include "ui/qt/SettingsWindow.h"
#include "ui/qt/UpdatePresentation.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <array>
#include <iostream>
#include <stdexcept>

namespace ui = HuhuCapture::UI::Qt;
namespace updates = HuhuCapture::Updates;
namespace localization = HuhuCapture::Localization;

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Type> Type* child(QObject& object, const char* name)
{
    auto* result = object.findChild<Type*>(QString::fromLatin1(name));
    require(result, "The production settings widget exists"); return result;
}
void settle()
{
    QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        QTemporaryDir temporary; require(temporary.isValid(), "Create actual presentation output fixture directory");
        const auto pictures = application.arguments().size() > 1 ? application.arguments()[1] : QString();
        if (!pictures.isEmpty()) require(QDir().mkpath(pictures), "Create real screenshot output directory");
        const auto retained = QDir(temporary.path()).filePath(QStringLiteral("download 中文.zip"));
        { QFile file(retained); require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
            file.write("retained view fixture") == 21, "Create an explicitly labelled retained-file UI fixture"); }
        localization::LanguageCatalog languages({localization::ResourcePolicy::LinuxUserResources, temporary.path(), temporary.path()});
        require(languages.load(QStringLiteral("en-US")).usable, "Read actual embedded language resources");
        ui::SettingsWindow window;
        window.setTranslator([&languages](QStringView key) { return languages.text(key); });
        ui::SettingsSnapshot settings; settings.canOpenLinks = true; settings.canChangeLanguage = true;
        settings.languages = languages.available(); settings.languageCode = QStringLiteral("en-US");
        settings.effectiveLanguageCode = settings.languageCode;
        window.setSnapshot(settings); window.setApplicationVersion(QStringLiteral("0.0.5"));
        window.setPage(ui::SettingsWindow::Page::About); window.show(); settle();
        QString reveal;
        QObject::connect(&window, &ui::SettingsWindow::openUpdateFolderRequested, &window,
            [&reveal](const QString& file) { reveal = file; });
        const std::array kinds{updates::PackageKind::WindowsInstaller, updates::PackageKind::WindowsPortableZip,
            updates::PackageKind::LinuxDeb, updates::PackageKind::LinuxAppImage};
        for (const auto* code : {"en-US", "zh-CN", "zh-TW", "ja-JP", "ru-RU", "id-ID"}) {
            require(languages.load(QString::fromLatin1(code)).usable, "Every product language loads from its existing UTF-16 catalog");
            window.refreshLanguage();
            updates::Snapshot value; value.stage = updates::Stage::Downloaded;
            value.version = QStringLiteral("0.0.6"); value.file = retained;
            for (const auto kind : kinds) {
                value.packageKind = kind;
                const auto shown = ui::aboutUpdateSnapshot(value); window.setUpdateSnapshot(shown); settle();
                const auto expected = languages.text(shown.installationInstructionsKey);
                require(!expected.startsWith(QStringLiteral("update.")) &&
                    child<QLabel>(window, "settingsUpdateStatus")->text().contains(expected),
                    "Each verified package kind displays its complete localized manual installation instructions");
                require(child<QPushButton>(window, "settingsUpdateFolder")->isEnabled(), "A retained completed file has a real location action");
                child<QPushButton>(window, "settingsUpdateFolder")->click();
                require(reveal == retained, "The view dispatches the exact retained file instead of inventing a download location");
            }
            if (!pictures.isEmpty()) require(window.grab().save(QDir(pictures).filePath(QStringLiteral("update-instructions-%1.png").arg(QString::fromLatin1(code)))),
                "Write an actual production-window screenshot for every product language");
        }
        updates::Snapshot uncertain; uncertain.stage = updates::Stage::Failed;
        uncertain.error = updates::Error::DurabilityUnknown; uncertain.file = retained;
        window.setUpdateSnapshot(ui::aboutUpdateSnapshot(uncertain)); settle();
        require(child<QLabel>(window, "settingsUpdateStatus")->text() == languages.text(u"update.durabilityUnknown") &&
            child<QPushButton>(window, "settingsUpdateFolder")->isVisible() && child<QPushButton>(window, "settingsUpdateFolder")->isEnabled() &&
            child<QLabel>(window, "settingsUpdatePath")->isVisible(),
            "Unknown commit durability is an explicit failure while the real published path remains visible and reachable");
        child<QPushButton>(window, "settingsUpdateFolder")->click(); require(reveal == retained, "Durability failure retains the location action");
        updates::Snapshot cancelled; cancelled.stage = updates::Stage::Cancelled; cancelled.stopped = true;
        window.setUpdateSnapshot(ui::aboutUpdateSnapshot(cancelled)); settle();
        require(child<QLabel>(window, "settingsUpdateStatus")->text() == languages.text(u"update.cancelled") &&
            !child<QPushButton>(window, "settingsDownloadUpdate")->isEnabled(),
            "Application shutdown is presented as cancellation, without an idle or success claim");
        ui::AboutUpdateSnapshot unknown; unknown.stage = ui::UpdateStage::Unavailable;
        window.setUpdateSnapshot(unknown); settle();
        require(child<QLabel>(window, "settingsUpdateStatus")->text() == languages.text(u"update.unavailable") &&
            !child<QPushButton>(window, "settingsDownloadUpdate")->isEnabled() && child<QPushButton>(window, "settingsReleases")->isEnabled(),
            "Unidentified installations offer the actual Release page without an invented package selection");
        settings.languageCode = QStringLiteral("id-ID"); settings.effectiveLanguageCode = settings.languageCode;
        settings.standardDialogLanguageNote = languages.text(u"setting.qtLanguageFallback");
        window.setSnapshot(settings); window.setPage(ui::SettingsWindow::Page::General); settle();
        require(child<QLabel>(window, "settingsStandardLanguageNote")->isVisible() &&
            child<QLabel>(window, "settingsStandardLanguageNote")->text() == settings.standardDialogLanguageNote,
            "Missing standard dialog catalogs are described in the existing language setting without a blocking startup prompt");
        if (!pictures.isEmpty()) require(window.grab().save(QDir(pictures).filePath(QStringLiteral("standard-language-fallback-id.png"))),
            "Write the actual language fallback layout");
        std::cout << "Production update presentation: six languages, four package instructions, retained failure path, explicit cancellation, unavailable install and standard-dialog fallback passed. UI fixtures do not certify a downloaded package or OS installer.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Update presentation validation failed: " << error.what() << '\n'; return 1;
    }
}
