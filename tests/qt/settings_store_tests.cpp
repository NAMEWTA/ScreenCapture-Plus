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
}

void preserveErrors(const QTemporaryDir& root)
{
    const auto path = location(root, "bad");
    for (const auto& bytes : {QByteArray::fromHex("fffe7b"), QByteArray("not JSON"),
        QByteArray(R"({"_huhucapture":{"schema":2},"keep":"future"})"), QByteArray()}) {
        write(path, bytes);
        config::SettingsStore store(path);
        require(!store.load().ok(), "existing invalid or future config is not a fresh installation");
        require(store.save().error == config::SettingsError::NotLoaded, "failed load blocks writes");
        require(read(path) == bytes, "failed config preserved exactly");
    }
    config::SettingsStore empty{QString()};
    require(!empty.load().ok(), "empty path cannot choose the process working directory");
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
    write(executableConfig, "broken source");
    require(!installed.importFrom(executableConfig).ok() && read(userConfig) == afterEdit,
        "invalid import does not replace valid destination");
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
        std::cout << "Qt configuration migration, backups, encoding and real filesystem checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
