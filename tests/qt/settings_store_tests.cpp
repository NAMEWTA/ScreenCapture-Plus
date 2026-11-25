#include "services/config/JsonText.h"
#include "services/config/SettingsStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <iostream>
#include <filesystem>
#include <stdexcept>

namespace config = HuhuCapture::Config;

namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open real fixture");
    require(file.write(bytes) == bytes.size(), "write complete fixture");
    file.close();
    require(file.error() == QFileDevice::NoError, "close fixture");
}

QByteArray read(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read real fixture");
    const auto bytes = file.readAll();
    require(file.error() == QFileDevice::NoError, "read complete fixture");
    return bytes;
}

QByteArray utf16(const QString& text, bool littleEndian = true)
{
    QByteArray bytes = QByteArray::fromHex(littleEndian ? "fffe" : "feff");
    for (const auto unit : text) {
        const auto value = unit.unicode();
        bytes.append(static_cast<char>(littleEndian ? value & 255 : value >> 8));
        bytes.append(static_cast<char>(littleEndian ? value >> 8 : value & 255));
    }
    return bytes;
}

QString location(const QTemporaryDir& parent, const QString& name)
{
    const auto path = QDir(parent.path()).filePath(name);
    require(QDir().mkpath(path), "create isolated fixture directory");
    return QDir(path).filePath(QStringLiteral("config.json"));
}

void decodeContracts()
{
    const QString text = QString::fromUtf8(R"({"text":"中文 😀 é","unknown":{"keep":42}})");
    for (const auto& bytes : {text.toUtf8(), QByteArray::fromHex("efbbbf") + text.toUtf8(), utf16(text), utf16(text, false)}) {
        QJsonObject object;
        require(config::parseJsonObject(bytes, object).ok(), "legacy and UTF-8 encodings decode");
        require(object.value(QStringLiteral("text")).toString() == QString::fromUtf8("中文 😀 é"),
            "all Unicode text survives decoding");
    }
    QJsonObject retained{{QStringLiteral("sentinel"), 1}};
    const auto original = retained;
    for (const auto& bytes : {QByteArray::fromHex("fffe7b00ff"), QByteArray::fromHex("fffe00d8"),
        QByteArray("{\"text\":\"") + QByteArray::fromHex("c0af") + "\"}",
        QByteArray("{\"text\":\"") + QByteArray::fromHex("f09f") + "\"}", QByteArray("[]"), QByteArray("{")}) {
        require(!config::parseJsonObject(bytes, retained).ok(), "malformed encodings and JSON fail explicitly");
        require(retained == original, "failed decoding does not replace earlier output");
    }
    require(!config::parseJsonObject(QByteArray(config::maximumJsonBytes + 1, ' '), retained).ok(),
        "configuration read budget is enforced");
}

void migrationAndUnknownFields(const QTemporaryDir& root)
{
    const auto path = location(root, QString::fromUtf8("旧版配置"));
    const auto original = utf16(QString::fromUtf8(R"({"common":{"language":"ru-RU","future":37},"shortcutKey":{"cap":"Ctrl+Shift+F8"},"toolPin":{"rect":{"width":7,"future":[1,2]}},"custom":"用户😀"})"));
    write(path, original);
    config::SettingsStore store(path);
    require(store.load().ok(), "load old UTF-16 configuration");
    require(read(path) == original, "loading does not rewrite existing config");
    require(store.value({"shortcutKey", "cap"}).toString() == "Ctrl+Shift+F8", "custom shortcut retained");
    require(store.value({"common", "autoStart"}).toBool(true) == false, "only missing defaults added");
    const auto result = store.setValue({"toolPin", "rect", "width"}, 9);
    require(result.ok() && !result.backupPath.isEmpty(), "migration succeeds only with a real backup");
    require(read(result.backupPath) == original, "backup retains exact original encoding and bytes");
    require(!read(path).startsWith(QByteArray::fromHex("fffe")), "new config is normalized UTF-8");
    require(store.value({"common", "future"}).toInt() == 37 &&
        store.value({"custom"}).toString() == QString::fromUtf8("用户😀"), "unknown fields retained");
    require(store.value({"toolPin", "rect", "future"}).isArray(), "nested unknown fields retained");
    const auto before = QDir(QFileInfo(path).absolutePath()).entryList({"*.bak"}, QDir::Files);
    config::SettingsStore restarted(path);
    require(restarted.load().ok() && restarted.save().ok(), "restarting migration is idempotent");
    require(QDir(QFileInfo(path).absolutePath()).entryList({"*.bak"}, QDir::Files) == before,
        "successful migration is not repeated");
    require(restarted.setValue({"common", "language"}, "en-US").ok(), "normal subsequent save succeeds");
    require(read(result.backupPath) == original, "later saves never replace migration backup");

    const auto oldDefaultPath = location(root, "old-default-shortcut");
    write(oldDefaultPath, R"({"shortcutKey":{"cap":"Ctrl+Alt+A"}})");
    config::SettingsStore oldDefault(oldDefaultPath);
    require(oldDefault.load().ok() && oldDefault.value({"shortcutKey", "cap"}).toString() == "F1",
        "previous default shortcut follows the established v0.0.5 migration");
    require(oldDefault.save().ok(), "persist migrated default");
    require(oldDefault.setValue({"shortcutKey", "cap"}, "Ctrl+Alt+A").ok(), "new-schema deliberate shortcut is accepted");
    require(oldDefault.load().ok() && oldDefault.value({"shortcutKey", "cap"}).toString() == "Ctrl+Alt+A",
        "schema marker prevents repeating an old-default migration");
}

void preserveErrors(const QTemporaryDir& root)
{
    const auto path = location(root, "bad");
    for (const auto& bytes : {QByteArray::fromHex("fffe7b"), QByteArray("not JSON"),
        QByteArray(R"({"_huhucapture":{"schema":2},"keep":"future"})")}) {
        write(path, bytes);
        config::SettingsStore store(path);
        require(!store.load().ok(), "existing invalid or future config is not a fresh installation");
        require(store.save().error == config::SettingsError::NotLoaded, "failed load blocks writes");
        require(read(path) == bytes, "failed config preserved exactly");
    }
    config::SettingsStore empty{QString()};
    require(!empty.load().ok(), "empty path cannot choose the process working directory");
    for (const auto& marker : {QByteArray(), QByteArray(" \t\r\n"), utf16(QStringLiteral(" \r\n"))}) {
        write(path, marker);
        config::SettingsStore portable(path);
        require(portable.load().ok() && portable.value({"shortcutKey", "cap"}).toString() == "F1",
            "documented empty portable marker loads defaults");
        const auto saved = portable.save();
        require(saved.ok() && !saved.backupPath.isEmpty() && read(saved.backupPath) == marker,
            "empty portable marker is backed up before normalization");
    }
    const auto blocked = QDir(root.path()).filePath("parent-becomes-file");
    config::SettingsStore missing(QDir(blocked).filePath("config.json"));
    require(missing.load().ok(), "missing configuration uses defaults in memory");
    const auto previous = missing.document();
    write(blocked, "unrelated user file");
    require(!missing.setValue({"common", "language"}, "en-US").ok(), "real write failure is returned");
    require(missing.document() == previous && read(blocked) == "unrelated user file", "write failure retains memory and files");
}

void writersAndBackupCollision(const QTemporaryDir& root)
{
    const auto path = location(root, "writers");
    const QByteArray bytes(R"({"common":{"language":"zh-CN"},"unknown":true})");
    write(path, bytes);
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().first(16);
    const auto collision = path + ".pre-qt-" + QString::fromLatin1(digest) + ".bak";
    write(collision, "unrelated backup");
    config::SettingsStore first(path), second(path);
    require(first.load().ok() && second.load().ok(), "two readers load the same initial config");
    {
        QLockFile lock(path + ".lock");
        require(lock.tryLock(0), "own real concurrent writer lock");
        require(first.save().error == config::SettingsError::Locked, "active writer is respected");
    }
    const auto saved = first.setValue({"common", "language"}, "en-US");
    require(saved.ok() && saved.backupPath != collision, "backup collision uses an exclusive different name");
    require(read(collision) == "unrelated backup" && read(saved.backupPath) == bytes, "both backups preserved");
    const auto successful = read(path);
    require(second.setValue({"common", "language"}, "ru-RU").error == config::SettingsError::ChangedOnDisk,
        "stale writer cannot erase newer settings");
    require(read(path) == successful, "stale write leaves complete successful file intact");
    write(path, R"({"outside":"edit"})");
    require(first.save().error == config::SettingsError::ChangedOnDisk, "outside editor changes are detected");
    require(read(path) == R"({"outside":"edit"})", "outside editor data remains intact");
}

void pathAndImportContracts(const QTemporaryDir& root)
{
    const auto executableConfig = location(root, "portable-exe");
    const auto userConfig = location(root, "roaming-or-xdg");
    write(executableConfig, R"({"shortcutKey":{"cap":"Ctrl+Alt+F9"},"legacy":42})");
    write(userConfig, R"({"common":{"language":"en-US"},"destination":24})");
    const auto executableDirectory = QFileInfo(executableConfig).absolutePath();
    const auto userDirectory = QFileInfo(userConfig).absolutePath();
    require(config::settingsPath(config::PathPolicy::WindowsLegacy, executableDirectory, userDirectory) == executableConfig,
        "existing Windows portable config has priority");
    require(config::settingsPath(config::PathPolicy::Linux, executableDirectory, userDirectory) == userConfig,
        "Linux uses XDG config even if an AppImage resource contains config.json");
    require(config::settingsPath(config::PathPolicy::Linux, executableDirectory, userDirectory, executableDirectory) == executableConfig,
        "only explicit Linux portable mode chooses its external directory");

    config::SettingsStore installed(userConfig);
    require(installed.load().ok(), "installed config loaded");
    const auto previous = read(userConfig);
    const auto oldSource = read(executableConfig);
    const auto imported = installed.importFrom(executableConfig);
    require(imported.ok() && !imported.backupPath.isEmpty() && !imported.importedSourceBackupPath.isEmpty(),
        "import backs up both original destination and selected source");
    require(read(imported.backupPath) == previous && read(imported.importedSourceBackupPath) == oldSource,
        "import backups are exact bytes");
    require(read(executableConfig) == oldSource && installed.value({"legacy"}).toInt() == 42,
        "explicit source remains unchanged and its unknown fields migrate");
    require(installed.setValue({"common", "language"}, "id-ID").ok(), "edit after import");
    const auto afterEdit = read(userConfig);
    require(installed.importFrom(executableConfig).ok() && read(userConfig) == afterEdit,
        "reimporting the same source does not erase subsequent user edits");
    require(installed.importFrom(userConfig).ok() && read(userConfig) == afterEdit,
        "importing the active file is a no-op and does not rewrite the selected source");
    write(userConfig, R"({"external":"edit after import"})");
    require(installed.importFrom(executableConfig).error == config::SettingsError::ChangedOnDisk,
        "idempotent import still rejects stale destination state");
    require(QFile::remove(userConfig), "remove destination for outside-change fixture");
    require(installed.importFrom(executableConfig).error == config::SettingsError::ChangedOnDisk,
        "idempotent import does not report success for an externally deleted destination");
    write(userConfig, afterEdit);
    write(executableConfig, "broken source");
    require(!installed.importFrom(executableConfig).ok() && read(userConfig) == afterEdit,
        "invalid import does not replace valid destination");
}

void aliasProtection(const QTemporaryDir& root)
{
    const auto path = location(root, "aliases");
    write(path, R"({"shortcutKey":{"cap":"Ctrl+Shift+F9"}})");
    const auto alias = QFileInfo(path).absolutePath() + "/linked-config.json";
    std::error_code error;
#ifdef _WIN32
    std::filesystem::create_symlink(std::filesystem::path(path.toStdWString()),
        std::filesystem::path(alias.toStdWString()), error);
#else
    std::filesystem::create_symlink(std::filesystem::path(path.toUtf8().toStdString()),
        std::filesystem::path(alias.toUtf8().toStdString()), error);
#endif
    if (error) {
        std::cout << "NOT RUN: symlink creation unavailable: " << error.message() << '\n';
        return;
    }
    config::SettingsStore direct(path), linked(alias);
    require(direct.load().ok() && linked.load().ok(), "read active config through both names");
    require(direct.path() == linked.path(), "writer lock and QSaveFile use one canonical destination");
    const auto before = read(path);
    require(direct.importFrom(alias).ok() && read(path) == before,
        "import through an alias leaves the source unchanged");
    {
        QLockFile lock(direct.path() + ".lock");
        require(lock.tryLock(0), "hold real destination lock");
        require(linked.save().error == config::SettingsError::Locked,
            "a symbolic alias cannot bypass the active writer lock");
    }
    require(direct.setValue({"common", "language"}, "ru-RU").ok(), "save through canonical destination");
    const auto successful = read(path);
    require(linked.setValue({"common", "language"}, "en-US").error == config::SettingsError::ChangedOnDisk,
        "alias writer cannot erase a newer save");
    require(read(path) == successful && read(alias) == successful, "alias still names the complete saved config");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "create real temporary fixture root");
        decodeContracts();
        migrationAndUnknownFields(directory);
        preserveErrors(directory);
        writersAndBackupCollision(directory);
        pathAndImportContracts(directory);
        aliasProtection(directory);
        std::cout << "Qt configuration migration, backups, encoding and real filesystem checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
