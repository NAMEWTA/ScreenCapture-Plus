#include "services/localization/QtTranslations.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QPushButton>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void deliverLanguageChange()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        // Optional absolute path is the actual SDK/deployed translation folder,
        // never generated mock text. The default is qt.conf's real Qt location.
        HuhuCapture::Localization::QtTranslations translations(
            application.arguments().size() > 1 ? application.arguments()[1] : QString());
        require(translations.load(QStringLiteral("en-US")), "Qt source English requires no synthetic catalog");
        QDialogButtonBox buttons(QDialogButtonBox::Open | QDialogButtonBox::Cancel);
        buttons.show();
        const auto englishOpen = buttons.button(QDialogButtonBox::Open)->text();
        const auto englishCancel = buttons.button(QDialogButtonBox::Cancel)->text();
        for (const auto* code : {"zh-CN", "zh-TW", "ja-JP", "ru-RU"}) {
            require(translations.load(QString::fromLatin1(code)), "Load actual installed qtbase QM for every bundled non-English language");
            require(QFileInfo(translations.loadedFile()).isFile(), "Qt reports a real deployed compiled catalog");
            deliverLanguageChange();
            require(buttons.button(QDialogButtonBox::Open)->text() != englishOpen &&
                buttons.button(QDialogButtonBox::Cancel)->text() != englishCancel,
                "An existing real Qt standard button box retranslates after LanguageChange");
        }
        require(!translations.load(QStringLiteral("id-ID")) && translations.loadedFile().isEmpty(),
            "The fixed official Qt SDK has no Indonesian catalog; the service exposes that actual capability limit");
        require(translations.load(QStringLiteral("en-US")), "Switch back to source English");
        deliverLanguageChange();
        require(buttons.button(QDialogButtonBox::Open)->text() == englishOpen &&
            buttons.button(QDialogButtonBox::Cancel)->text() == englishCancel && translations.loadedFile().isEmpty(),
            "Removing the catalog restores the real original standard button text");
        QTemporaryDir empty; require(empty.isValid(), "Create an actually empty catalog directory");
        HuhuCapture::Localization::QtTranslations missing(empty.path());
        require(!missing.load(QStringLiteral("zh-CN")) && missing.loadedFile().isEmpty(),
            "Missing deployed catalogs report failure instead of claiming a translated dialog");
        std::cout << "Real deployed Qt catalog loading, four bundled non-English standard button transitions, English restoration and missing-catalog failure passed. The fixed Qt SDK has no Indonesian catalog; native OS-owned dialogs remain governed by the desktop.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Qt standard translation validation failed: " << error.what() << '\n'; return 1;
    }
}
