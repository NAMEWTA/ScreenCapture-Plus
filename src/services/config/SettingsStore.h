#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace HuhuCapture::Config {

enum class SettingsError {
    None, Read, Decode, FutureSchema, NotLoaded, InvalidKey, Locked,
    ChangedOnDisk, Backup, Write,
};

struct SettingsResult {
    SettingsError error{SettingsError::None};
    QString detail;
    QString backupPath;
    QString importedSourceBackupPath;
    bool ok() const noexcept { return error == SettingsError::None; }
};

enum class PathPolicy { WindowsLegacy, Linux };

// Resolve existing ancestors even when the selected directory or config.json
// does not exist yet. Empty means invalid or physically inside the supplied
// AppImage mount; this does not create directories or claim write permission.
QString resolvePortableDirectory(const QString& directory, const QString& appImageMount = {});

// Callers pass the actual Known Folder / QStandardPaths locations. An explicit
// portable directory must be outside an AppImage's read-only mount.
QString settingsPath(PathPolicy policy, const QString& applicationDirectory,
    const QString& userConfigDirectory, const QString& explicitPortableDirectory = {});

// UI-thread-owned, with no widgets, WinRT or global state. A failed load blocks
// writes; callers must surface the error and keep the existing file intact.
class SettingsStore final {
public:
    explicit SettingsStore(QString path);
    SettingsResult load();
    QJsonValue value(const QStringList& keys, const QJsonValue& fallback = {}) const;
    const QJsonObject& document() const noexcept { return document_; }
    const QString& path() const noexcept { return path_; }

    // Updates exactly one key, retaining unrelated and unknown JSON fields.
    // The in-memory value changes only after QSaveFile commits successfully.
    SettingsResult setValue(const QStringList& keys, const QJsonValue& value);
    SettingsResult save();

    // The UI supplies one explicitly selected old config.json; no disk scan.
    // Source bytes and any existing destination are backed up before import.
    // Reimporting the same bytes is idempotent and preserves subsequent edits.
    // Selecting the active file (including an alias) performs no rewrite.
    SettingsResult importFrom(const QString& sourcePath);

private:
    SettingsResult checkSnapshot() const;
    SettingsResult persist(QJsonObject candidate, bool forceBackup,
        const QByteArray* importedSource = nullptr);
    QString path_;
    QJsonObject document_;
    QByteArray originalBytes_;
    bool existed_{false};
    bool loaded_{false};
    bool needsMigration_{false};
};

} // namespace HuhuCapture::Config
