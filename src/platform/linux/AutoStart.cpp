#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include "services/desktop/AutoStart.h"
#include "services/desktop/LaunchCommand.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>
#include <utility>

namespace HuhuCapture::Desktop {
namespace {
constexpr qsizetype maximumEntryBytes = 64 * 1024;
constexpr char group[] = "Desktop Entry";
constexpr char ownership[] = "X-huhucapture-AutostartVersion";
struct KeyFileDelete { void operator()(GKeyFile* value) const { g_key_file_unref(value); } };
using KeyFile = std::unique_ptr<GKeyFile, KeyFileDelete>;
struct Entry {
    bool exists{};
    QByteArray bytes;
    AutoStartError error{AutoStartError::None};
    friend bool operator==(const Entry&, const Entry&) = default;
};
Entry readEntry(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) return {true, {}, AutoStartError::ForeignEntry};
    if (!info.exists()) return {};
    if (!info.isFile() || info.size() > maximumEntryBytes) return {true, {}, AutoStartError::ForeignEntry};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {true, {}, AutoStartError::Read};
    const auto bytes = file.read(maximumEntryBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maximumEntryBytes || !file.atEnd()) return {true, {}, AutoStartError::Read};
    QStringDecoder decoder(QStringDecoder::Utf8); const QString decoded = decoder(bytes);
    if (decoder.hasError() || decoded.contains(QChar(u'\0'))) return {true, {}, AutoStartError::Read};
    return {true, bytes, AutoStartError::None};
}
KeyFile parse(const Entry& entry)
{
    KeyFile document(g_key_file_new());
    if (entry.exists) {
        if (entry.error != AutoStartError::None || !g_key_file_load_from_data(document.get(), entry.bytes.constData(),
                static_cast<gsize>(entry.bytes.size()), static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), nullptr)) return {};
        g_autofree gchar* marker = g_key_file_get_string(document.get(), group, ownership, nullptr);
        g_autofree gchar* type = g_key_file_get_string(document.get(), group, "Type", nullptr);
        if (!marker || QByteArray(marker) != "1" || !type || QByteArray(type) != "Application") return {};
    }
    return document;
}
bool enabled(GKeyFile* document)
{
    if (g_key_file_get_boolean(document, group, "Hidden", nullptr)) return false;
    if (g_key_file_has_key(document, group, "X-GNOME-Autostart-enabled", nullptr) &&
        !g_key_file_get_boolean(document, group, "X-GNOME-Autostart-enabled", nullptr)) return false;
    g_autoptr(GDesktopAppInfo) app = g_desktop_app_info_new_from_keyfile(document);
    return app && g_desktop_app_info_get_show_in(app, nullptr);
}
AutoStartError replace(const QString& path, const Entry& expected, const Entry& desired)
{
    const auto actual = readEntry(path);
    if (actual.error != AutoStartError::None) return actual.error;
    if (!(actual == expected)) return AutoStartError::ChangedOnDisk;
    if (actual == desired) return AutoStartError::None;
    if (!desired.exists) return QFile::remove(path) ? AutoStartError::None : AutoStartError::Write;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return AutoStartError::Write;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(desired.bytes) != desired.bytes.size() || !file.flush()) return AutoStartError::Write;
    if (!(readEntry(path) == expected)) { file.cancelWriting(); return AutoStartError::ChangedOnDisk; }
    if (!file.commit()) return AutoStartError::Write;
    return readEntry(path) == desired ? AutoStartError::None : AutoStartError::ChangedOnDisk;
}
}

struct AutoStart::Private {
    QString executable;
    QString path;
    QStringList systemPaths;
    explicit Private(QString value) : executable(std::move(value))
    {
        const auto user = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        if (!user.isEmpty() && QDir::isAbsolutePath(user)) path = QDir(user).filePath(QStringLiteral("autostart/huhucapture.desktop"));
        const auto locations = QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation);
        if (locations.size() > 32) { path.clear(); return; }
        for (const auto& directory : locations) {
            if (directory == user || !QDir::isAbsolutePath(directory)) continue;
            systemPaths.push_back(QDir(directory).filePath(QStringLiteral("autostart/huhucapture.desktop")));
        }
    }
    Entry effectiveEntry() const
    {
        const auto user = readEntry(path);
        if (user.exists || user.error != AutoStartError::None) return user;
        for (const auto& system : systemPaths) {
            const auto entry = readEntry(system);
            if (entry.exists || entry.error != AutoStartError::None) return entry;
        }
        return {};
    }
    AutoStartSnapshot inspect() const
    {
        if (path.isEmpty()) return {false, false, AutoStartError::Unavailable};
        const auto entry = effectiveEntry();
        if (entry.error != AutoStartError::None) return {false, false, entry.error};
        auto document = parse(entry);
        if (!document) return {false, false, AutoStartError::ForeignEntry};
        const bool registered = entry.exists && enabled(document.get());
        if (!persistentLaunchExecutable(executable)) return {registered, false, AutoStartError::InvalidExecutable};
        return {registered, true, AutoStartError::None};
    }
};

AutoStart::AutoStart(QString executable) : d(std::make_unique<Private>(std::move(executable))) {}
AutoStart::~AutoStart() = default;
AutoStartSnapshot AutoStart::snapshot() const { return d->inspect(); }
AutoStartResult AutoStart::setEnabled(bool requested, const std::function<bool()>& persist)
{
    const auto state = snapshot();
    if (!state.canChange) return {state.error, state};
    // Even the lock directory is only created by an explicit user change.
    if (!QDir().mkpath(QFileInfo(d->path).absolutePath())) return {AutoStartError::Write, snapshot()};
    QLockFile lock(d->path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return {AutoStartError::ChangedOnDisk, snapshot()};
    const auto before = readEntry(d->path);
    const auto inherited = d->effectiveEntry();
    if (before.error != AutoStartError::None || inherited.error != AutoStartError::None)
        return {before.error != AutoStartError::None ? before.error : inherited.error, snapshot()};
    auto document = parse(inherited);
    if (!document) return {AutoStartError::ForeignEntry, snapshot()};
    Entry desired;
    if (requested || inherited.exists) {
        g_key_file_set_string(document.get(), group, "Type", "Application");
        g_key_file_set_string(document.get(), group, "Name", "huhucapture");
        g_key_file_set_string(document.get(), group, ownership, "1");
        const auto executable = d->executable.toUtf8();
        const auto command = desktopExecCommand(d->executable, {QStringLiteral("--auto-start")}).toUtf8();
        if (command.isEmpty()) return {AutoStartError::InvalidExecutable, snapshot()};
        g_key_file_set_string(document.get(), group, "Exec", command.constData());
        g_key_file_set_string(document.get(), group, "TryExec", executable.constData());
        g_key_file_set_boolean(document.get(), group, "Terminal", false);
        g_key_file_set_boolean(document.get(), group, "Hidden", !requested);
        g_key_file_set_boolean(document.get(), group, "X-GNOME-Autostart-enabled", requested);
        if (requested) {
            g_key_file_remove_key(document.get(), group, "OnlyShowIn", nullptr);
            g_key_file_remove_key(document.get(), group, "NotShowIn", nullptr);
        }
        gsize size{}; g_autofree gchar* bytes = g_key_file_to_data(document.get(), &size, nullptr);
        if (!bytes || size > static_cast<gsize>(maximumEntryBytes)) return {AutoStartError::Write, snapshot()};
        desired.exists = true; desired.bytes = QByteArray(bytes, static_cast<qsizetype>(size));
        if (requested && !enabled(document.get())) return {AutoStartError::InvalidExecutable, snapshot()};
    }
    const auto changed = replace(d->path, before, desired);
    if (changed != AutoStartError::None) return {changed, snapshot()};
    bool saved = true;
    try { if (persist) saved = persist(); } catch (...) { saved = false; }
    if (saved) return {AutoStartError::None, snapshot()};
    const auto restored = replace(d->path, desired, before);
    return {restored == AutoStartError::None ? AutoStartError::SaveFailed : AutoStartError::RollbackFailed, snapshot()};
}

} // namespace HuhuCapture::Desktop
