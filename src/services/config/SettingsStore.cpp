#include "services/config/SettingsStore.h"

#include "platform/FileCommitter.h"
#include "services/config/JsonText.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <filesystem>
#include <utility>

namespace HuhuCapture::Config {
namespace {
constexpr auto metadataKey = "_huhucapture";
constexpr int schemaVersion = 1;

std::filesystem::path nativePath(const QString& path)
{
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}

QString qtPath(const std::filesystem::path& path)
{
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QString::fromUtf8(path.native().data(), static_cast<qsizetype>(path.native().size()));
#endif
}

bool existsIncludingLink(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

SettingsResult readFile(const QString& path, QByteArray& bytes, bool& exists)
{
    exists = existsIncludingLink(path);
    if (!exists) { bytes.clear(); return {}; }
    if (!QFileInfo(path).isFile())
        return {SettingsError::Read, QStringLiteral("Configuration is not a regular file"), {}, {}};
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return {SettingsError::Read, input.errorString(), {}, {}};
    if (input.size() > maximumJsonBytes)
        return {SettingsError::Read, QStringLiteral("Configuration exceeds 8 MiB"), {}, {}};
    auto content = input.read(maximumJsonBytes + 1);
    if (input.error() != QFileDevice::NoError || content.size() > maximumJsonBytes || !input.atEnd())
        return {SettingsError::Read, QStringLiteral("Could not read the complete configuration"), {}, {}};
    bytes = std::move(content);
    return {};
}

SettingsResult parse(const QByteArray& bytes, QJsonObject& output)
{
    auto result = parseJsonObject(bytes, output);
    // An empty config.json is the documented legacy portable-mode marker.
    // Accept only correctly decoded empty/whitespace text, and still back up
    // those exact bytes before its first write. Broken encoding is not empty.
    if (result.error == TextError::EmptyDocument) { output = {}; return {}; }
    if (!result.ok()) return {SettingsError::Decode, result.detail, {}, {}};
    const auto metadata = output.value(QLatin1String(metadataKey));
    if (!metadata.isUndefined() && !metadata.isObject())
        return {SettingsError::FutureSchema, QStringLiteral("Unrecognized migration metadata; file was retained"), {}, {}};
    const auto version = metadata.toObject().value(QStringLiteral("schema"));
    if (!version.isUndefined() && (!version.isDouble() || version.toDouble() != schemaVersion))
        return {SettingsError::FutureSchema, QStringLiteral("Unsupported configuration schema; file was retained"), {}, {}};
    return {};
}

void insertMissingDefaults(QJsonObject& root)
{
    auto insert = [&root](const QString& section, const QJsonObject& defaults) {
        const auto existing = root.value(section);
        // A malformed known section is retained too. Typed readers can use
        // defaults, but changing another setting must not erase its raw value.
        if (!existing.isUndefined() && !existing.isObject()) return;
        auto object = existing.toObject();
        for (auto it = defaults.begin(); it != defaults.end(); ++it)
            if (!object.contains(it.key())) object.insert(it.key(), it.value());
        root.insert(section, object);
    };
    insert(QStringLiteral("common"), {{QStringLiteral("autoStart"), false},
        {QStringLiteral("language"), QStringLiteral("zh-CN")}});
    insert(QStringLiteral("shortcutKey"), {{QStringLiteral("cap"), QStringLiteral("F1")}});
}

void migrateLegacyShortcut(QJsonObject& root)
{
    const auto schema = root.value(QLatin1String(metadataKey)).toObject().value(QStringLiteral("schema"));
    if (!schema.isUndefined()) return;
    auto shortcuts = root.value(QStringLiteral("shortcutKey")).toObject();
    if (shortcuts.value(QStringLiteral("cap")).toString() == QStringLiteral("Ctrl+Alt+A")) {
        // Preserve v0.0.5's explicit migration of its previous default. Other
        // custom shortcuts, and deliberate values in the new schema, survive.
        shortcuts.insert(QStringLiteral("cap"), QStringLiteral("F1"));
        root.insert(QStringLiteral("shortcutKey"), shortcuts);
    }
}

bool replaceValue(QJsonObject& object, const QStringList& keys, qsizetype position, const QJsonValue& value)
{
    if (position + 1 == keys.size()) { object.insert(keys[position], value); return true; }
    const auto child = object.value(keys[position]);
    if (!child.isUndefined() && !child.isObject()) return false;
    auto nested = child.toObject();
    if (!replaceValue(nested, keys, position + 1, value)) return false;
    object.insert(keys[position], nested);
    return true;
}

SettingsResult backup(const QString& destination, const QByteArray& bytes, const QString& label)
{
    const auto directory = QFileInfo(destination).absoluteDir().canonicalPath();
    if (directory.isEmpty())
        return {SettingsError::Backup, QStringLiteral("Cannot resolve the configuration directory"), {}, {}};
    std::error_code error;
    const auto staging = Platform::createExclusiveTemporaryFile(nativePath(directory), error);
    if (staging.empty())
        return {SettingsError::Backup, QString::fromStdString(error.message()), {}, {}};
    const auto stagingPath = qtPath(staging);
    QFile output(stagingPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.flush()) {
        const auto detail = output.errorString();
        output.close();
        QFile::remove(stagingPath);
        return {SettingsError::Backup, detail, {}, {}};
    }
    output.close();
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().first(16);
    const auto stem = QFileInfo(destination).fileName() + QLatin1Char('.') + label +
        QLatin1Char('-') + QString::fromLatin1(digest);
    for (int sequence = 0; sequence < 128; ++sequence) {
        const auto suffix = sequence ? QStringLiteral("-%1").arg(sequence) : QString();
        const auto target = QDir(directory).filePath(stem + suffix + QStringLiteral(".bak"));
        const auto committed = Platform::commitFileNoReplace(staging, nativePath(target));
        if (committed.durable()) return {SettingsError::None, {}, qtPath(committed.destination), {}};
        if (committed.committed()) {
            // Preserve both names after a persistence uncertainty. The config
            // remains unchanged and the caller receives the actual backup path.
            return {SettingsError::Backup, QString::fromStdString(committed.error.message()),
                qtPath(committed.destination), {}};
        }
        if (committed.error == std::errc::file_exists) continue;
        QFile::remove(stagingPath);
        return {SettingsError::Backup, QString::fromStdString(committed.error.message()), {}, {}};
    }
    QFile::remove(stagingPath);
    return {SettingsError::Backup, QStringLiteral("Configuration backup name limit reached"), {}, {}};
}
} // namespace

QString settingsPath(PathPolicy policy, const QString& applicationDirectory,
    const QString& userConfigDirectory, const QString& explicitPortableDirectory)
{
    if (!explicitPortableDirectory.isEmpty())
        return QDir(explicitPortableDirectory).absoluteFilePath(QStringLiteral("config.json"));
    if (policy == PathPolicy::WindowsLegacy && !applicationDirectory.isEmpty()) {
        const auto besideExecutable = QDir(applicationDirectory).absoluteFilePath(QStringLiteral("config.json"));
        if (existsIncludingLink(besideExecutable)) return besideExecutable;
    }
    if (userConfigDirectory.isEmpty()) return {};
    return QDir(userConfigDirectory).absoluteFilePath(QStringLiteral("config.json"));
}

SettingsStore::SettingsStore(QString path)
{
    if (path.isEmpty() || path.contains(QChar::Null)) return;
    // QSaveFile resolves file symlinks internally. Use that same physical path
    // for reading and locking, so aliases cannot obtain independent writer
    // locks for one destination. weakly_canonical also resolves an existing
    // parent when a fresh configuration does not exist yet.
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(
        nativePath(QFileInfo(path).absoluteFilePath()), error);
    if (!error) path_ = qtPath(resolved);
}

SettingsResult SettingsStore::load()
{
    loaded_ = false;
    if (path_.isEmpty())
        return {SettingsError::Read, QStringLiteral("No valid configuration path is available"), {}, {}};
    QByteArray bytes;
    bool existed = false;
    auto result = readFile(path_, bytes, existed);
    if (!result.ok()) return result;
    QJsonObject document;
    if (existed) {
        result = parse(bytes, document);
        if (!result.ok()) return result;
    }
    const auto migration = document.value(QLatin1String(metadataKey)).toObject();
    needsMigration_ = existed && migration.value(QStringLiteral("schema")).toInt() != schemaVersion;
    insertMissingDefaults(document);
    migrateLegacyShortcut(document);
    document_ = std::move(document);
    originalBytes_ = std::move(bytes);
    existed_ = existed;
    loaded_ = true;
    return {};
}

QJsonValue SettingsStore::value(const QStringList& keys, const QJsonValue& fallback) const
{
    QJsonValue value(document_);
    for (const auto& key : keys) {
        if (!value.isObject()) return fallback;
        value = value.toObject().value(key);
    }
    return value.isUndefined() ? fallback : value;
}

SettingsResult SettingsStore::setValue(const QStringList& keys, const QJsonValue& value)
{
    if (keys.isEmpty() || keys.size() > 16 || keys.first() == QLatin1String(metadataKey) || value.isUndefined())
        return {SettingsError::InvalidKey, QStringLiteral("Invalid configuration key"), {}, {}};
    for (const auto& key : keys)
        if (key.isEmpty()) return {SettingsError::InvalidKey, QStringLiteral("Empty configuration key"), {}, {}};
    auto candidate = document_;
    if (!replaceValue(candidate, keys, 0, value))
        return {SettingsError::InvalidKey, QStringLiteral("An existing parent value is not a JSON object"), {}, {}};
    return persist(std::move(candidate), false);
}

SettingsResult SettingsStore::save() { return persist(document_, false); }

SettingsResult SettingsStore::checkSnapshot() const
{
    QByteArray current;
    bool exists = false;
    const auto result = readFile(path_, current, exists);
    if (!result.ok()) return result;
    if (exists != existed_ || current != originalBytes_)
        return {SettingsError::ChangedOnDisk, QStringLiteral("Configuration changed on disk; reload before saving"), {}, {}};
    return {};
}

SettingsResult SettingsStore::persist(QJsonObject candidate, bool forceBackup, const QByteArray* importedSource)
{
    if (!loaded_)
        return {SettingsError::NotLoaded, QStringLiteral("Load and resolve configuration errors before saving"), {}, {}};
    const auto directory = QFileInfo(path_).absolutePath();
    if (!QDir().mkpath(directory))
        return {SettingsError::Write, QStringLiteral("Cannot create the configuration directory"), {}, {}};
    QLockFile lock(path_ + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return {SettingsError::Locked, QStringLiteral("Configuration is in use by another writer"), {}, {}};

    const auto result = checkSnapshot();
    if (!result.ok()) return result;

    auto metadata = candidate.value(QLatin1String(metadataKey)).toObject();
    metadata.insert(QStringLiteral("schema"), schemaVersion);
    candidate.insert(QLatin1String(metadataKey), metadata);
    const auto bytes = QJsonDocument(candidate).toJson(QJsonDocument::Indented);
    if (bytes.size() > maximumJsonBytes)
        return {SettingsError::Write, QStringLiteral("Updated configuration exceeds 8 MiB"), {}, {}};
    if (bytes == originalBytes_ && !importedSource) return {};

    SettingsResult outcome;
    if (existed_ && (needsMigration_ || forceBackup)) {
        outcome = backup(path_, originalBytes_, QStringLiteral("pre-qt"));
        if (!outcome.ok()) return outcome;
    }
    if (importedSource) {
        const auto importedBackup = backup(path_, *importedSource, QStringLiteral("import-source"));
        outcome.importedSourceBackupPath = importedBackup.backupPath;
        if (!importedBackup.ok()) {
            outcome.error = importedBackup.error;
            outcome.detail = importedBackup.detail;
            return outcome;
        }
    }
    QSaveFile output(path_);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        outcome.error = SettingsError::Write;
        outcome.detail = output.errorString();
        return outcome;
    }
    document_ = std::move(candidate);
    originalBytes_ = bytes;
    existed_ = true;
    needsMigration_ = false;
    return outcome;
}

SettingsResult SettingsStore::importFrom(const QString& sourcePath)
{
    if (!loaded_)
        return {SettingsError::NotLoaded, QStringLiteral("Load the destination before importing"), {}, {}};
    QByteArray bytes;
    bool exists = false;
    auto result = readFile(sourcePath, bytes, exists);
    if (!result.ok()) return result;
    if (!exists) return {SettingsError::Read, QStringLiteral("Selected configuration does not exist"), {}, {}};
    if (existsIncludingLink(path_)) {
        std::error_code error;
        const bool sameFile = std::filesystem::equivalent(nativePath(sourcePath), nativePath(path_), error);
        if (error) return {SettingsError::Read, QString::fromStdString(error.message()), {}, {}};
        if (sameFile) return checkSnapshot();
    }
    QJsonObject candidate;
    result = parse(bytes, candidate);
    if (!result.ok()) return result;
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const auto previous = document_.value(QLatin1String(metadataKey)).toObject();
    if (previous.value(QStringLiteral("importedSha256")).toString() == digest) return checkSnapshot();
    insertMissingDefaults(candidate);
    migrateLegacyShortcut(candidate);
    auto metadata = candidate.value(QLatin1String(metadataKey)).toObject();
    metadata.insert(QStringLiteral("importedSha256"), digest);
    metadata.insert(QStringLiteral("importedSource"), QFileInfo(sourcePath).absoluteFilePath());
    candidate.insert(QLatin1String(metadataKey), metadata);
    return persist(std::move(candidate), true, &bytes);
}

} // namespace HuhuCapture::Config
