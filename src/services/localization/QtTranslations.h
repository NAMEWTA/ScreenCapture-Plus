#pragma once

#include <QLocale>
#include <QString>
#include <memory>

class QTranslator;

namespace HuhuCapture::Localization {

// The application owns one of these beside its LanguageCatalog. Use only on
// the application thread. Official Qt catalogs supply standard dialog/button
// translations; project text continues to use the existing six-language catalog.
class QtTranslations final {
public:
    // Empty selects QLibraryInfo::TranslationsPath, resolved by deployed qt.conf.
    explicit QtTranslations(QString directory = {});
    ~QtTranslations();
    QtTranslations(const QtTranslations&) = delete;
    QtTranslations& operator=(const QtTranslations&) = delete;
    bool load(const QString& effectiveLanguageCode);
    QString languageCode() const { return language_; }
    QString loadedFile() const;

private:
    QString directory_, language_;
    QLocale originalLocale_;
    std::unique_ptr<QTranslator> translator_;
    bool loaded_{};
};

} // namespace HuhuCapture::Localization
