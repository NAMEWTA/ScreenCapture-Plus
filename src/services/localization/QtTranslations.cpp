#include "services/localization/QtTranslations.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QThread>
#include <QTranslator>
#include <utility>

namespace HuhuCapture::Localization {

QtTranslations::QtTranslations(QString directory)
    : directory_(directory.isEmpty() ? QLibraryInfo::path(QLibraryInfo::TranslationsPath) : std::move(directory))
{}

QtTranslations::~QtTranslations()
{
    if (translator_) QCoreApplication::removeTranslator(translator_.get());
    QLocale::setDefault(originalLocale_);
}

bool QtTranslations::load(const QString& effectiveLanguageCode)
{
    auto* application = QCoreApplication::instance();
    if (!application || QThread::currentThread() != application->thread()) return false;
    if (language_ == effectiveLanguageCode) return loaded_;
    if (translator_) QCoreApplication::removeTranslator(translator_.get());
    translator_.reset(); loaded_ = false; language_ = effectiveLanguageCode;
    const QLocale locale(effectiveLanguageCode);
    QLocale::setDefault(locale);
    // English is Qt's original text. Removing the previous catalog is the real
    // language change; it does not depend on a redundant qtbase_en.qm.
    if (locale.language() == QLocale::English) { loaded_ = true; return true; }
    if (locale.language() == QLocale::C || !QDir::isAbsolutePath(directory_)) return false;
    auto candidate = std::make_unique<QTranslator>();
    if (!candidate->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), directory_) ||
        !QCoreApplication::installTranslator(candidate.get())) return false;
    translator_ = std::move(candidate); loaded_ = true; return true;
}

QString QtTranslations::loadedFile() const
{ return translator_ ? translator_->filePath() : QString(); }

} // namespace HuhuCapture::Localization
