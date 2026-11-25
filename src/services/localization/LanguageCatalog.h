#pragma once

#include "services/config/JsonText.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringView>
#include <QVector>

namespace HuhuCapture::Localization {

enum class ResourcePolicy { WindowsDirectoryFallback, LinuxUserResources };

struct LanguagePaths {
    ResourcePolicy policy;
    QString applicationDirectory;
    QString userDataDirectory;
};

struct LanguageEntry {
    QString name;
    QString code;
    QString source;
};

struct LanguageResult {
    // A broken external translation falls back to embedded English without
    // changing the user's selected language in persistent settings.
    bool usable{};
    bool fellBack{};
    QString requestedCode;
    QString effectiveCode;
    QString source;
    Config::JsonTextResult error;
};

// One catalog per application. UI windows consume the same cached text and
// changed signal; this service does not own widgets or write configuration.
// Use it on its owning application thread. Workers receive text snapshots.
class LanguageCatalog final : public QObject {
    Q_OBJECT
public:
    explicit LanguageCatalog(LanguagePaths paths, QObject* parent = nullptr);
    QVector<LanguageEntry> available() const;
    LanguageResult load(const QString& code);
    QString text(QStringView key) const;
    QString requestedCode() const { return requested_; }
    QString effectiveCode() const { return effective_; }

Q_SIGNALS:
    void changed();

private:
    QVector<LanguageEntry> externalEntries() const;
    LanguagePaths paths_;
    QJsonObject current_;
    QJsonObject english_;
    QString requested_;
    QString effective_;
    mutable QHash<QString, QString> cache_;
};

} // namespace HuhuCapture::Localization
