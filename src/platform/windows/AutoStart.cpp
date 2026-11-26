#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "services/desktop/AutoStart.h"
#include "services/desktop/LaunchCommand.h"
#include "app/Brand.h"
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace HuhuCapture::Desktop {
namespace {
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr DWORD maximumValueBytes = 64 * 1024;
struct RegistryKey {
    HKEY value{};
    ~RegistryKey() { if (value) RegCloseKey(value); }
};
struct Entry {
    bool exists{};
    DWORD type{};
    QByteArray bytes;
    AutoStartError error{AutoStartError::None};
    friend bool operator==(const Entry&, const Entry&) = default;
};
Entry readEntry()
{
    RegistryKey key;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE, &key.value);
    if (opened == ERROR_FILE_NOT_FOUND) return {};
    if (opened != ERROR_SUCCESS) return {false, 0, {}, AutoStartError::Read};
    Entry entry; DWORD size{};
    auto status = RegQueryValueExW(key.value, Brand::name.data(), nullptr, &entry.type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    if (status != ERROR_SUCCESS || size > maximumValueBytes) return {false, 0, {}, AutoStartError::Read};
    entry.exists = true; entry.bytes.resize(size);
    status = RegQueryValueExW(key.value, Brand::name.data(), nullptr, &entry.type,
        reinterpret_cast<BYTE*>(entry.bytes.data()), &size);
    if (status != ERROR_SUCCESS || size != static_cast<DWORD>(entry.bytes.size())) entry.error = AutoStartError::Read;
    return entry;
}
QString registeredExecutable(const Entry& entry)
{
    if (!entry.exists || entry.error != AutoStartError::None || entry.type != REG_SZ ||
        entry.bytes.size() < static_cast<qsizetype>(sizeof(wchar_t)) || entry.bytes.size() % sizeof(wchar_t)) return {};
    // QByteArray alignment is not a WCHAR alignment guarantee. Copy first.
    std::wstring command(static_cast<std::size_t>(entry.bytes.size()) / sizeof(wchar_t), L'\0');
    std::memcpy(command.data(), entry.bytes.constData(), static_cast<std::size_t>(entry.bytes.size()));
    if (command.back() != L'\0') return {};
    command.pop_back();
    if (command.find(L'\0') != std::wstring::npos) return {};
    int count{}; auto* argv = CommandLineToArgvW(command.c_str(), &count);
    if (!argv) return {};
    const auto executable = count == 2 && std::wstring_view(argv[1]) == L"--auto-start" ? QString::fromWCharArray(argv[0]) : QString();
    LocalFree(argv); return executable;
}
bool ownEntry(const Entry& entry, const QString& executable)
{
    if (!entry.exists) return true;
    const auto registered = registeredExecutable(entry);
    if (registered.isEmpty() || !QDir::isAbsolutePath(registered)) return false;
    return QFileInfo(registered).fileName().compare(QString::fromWCharArray(Brand::executableName.data()), Qt::CaseInsensitive) == 0 ||
        QDir::cleanPath(QDir::fromNativeSeparators(registered)).compare(
            QDir::cleanPath(QDir::fromNativeSeparators(executable)), Qt::CaseInsensitive) == 0;
}
AutoStartError replace(const Entry& expected, const Entry& desired)
{
    const auto actual = readEntry();
    if (actual.error != AutoStartError::None) return actual.error;
    if (!(actual == expected)) return AutoStartError::ChangedOnDisk;
    if (actual == desired) return AutoStartError::None;
    RegistryKey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
        nullptr, &key.value, nullptr) != ERROR_SUCCESS) return AutoStartError::Write;
    // Re-read after acquiring the write handle. This is not a registry-wide
    // compare-and-swap; unrelated Run values are never enumerated or written.
    if (!(readEntry() == expected)) return AutoStartError::ChangedOnDisk;
    const auto status = desired.exists ? RegSetValueExW(key.value, Brand::name.data(), 0, desired.type,
        reinterpret_cast<const BYTE*>(desired.bytes.constData()), static_cast<DWORD>(desired.bytes.size())) :
        RegDeleteValueW(key.value, Brand::name.data());
    if (status != ERROR_SUCCESS && !(status == ERROR_FILE_NOT_FOUND && !desired.exists)) return AutoStartError::Write;
    return readEntry() == desired ? AutoStartError::None : AutoStartError::ChangedOnDisk;
}
}

struct AutoStart::Private {
    QString executable;
    explicit Private(QString value) : executable(QDir::toNativeSeparators(std::move(value))) {}
    AutoStartSnapshot inspect() const
    {
        const auto entry = readEntry();
        if (entry.error != AutoStartError::None) return {false, false, entry.error};
        if (!ownEntry(entry, executable)) return {false, false, AutoStartError::ForeignEntry};
        const auto path = registeredExecutable(entry);
        const bool enabled = !path.isEmpty() && QFileInfo(path).isFile();
        if (!persistentLaunchExecutable(executable)) return {enabled, false, AutoStartError::InvalidExecutable};
        return {enabled, true, AutoStartError::None};
    }
};

AutoStart::AutoStart(QString executable) : d(std::make_unique<Private>(std::move(executable))) {}
AutoStart::~AutoStart() = default;
AutoStartSnapshot AutoStart::snapshot() const { return d->inspect(); }
AutoStartResult AutoStart::setEnabled(bool enabled, const std::function<bool()>& persist)
{
    const auto state = snapshot();
    if (!state.canChange) return {state.error, state};
    const auto before = readEntry();
    if (before.error != AutoStartError::None || !ownEntry(before, d->executable))
        return {before.error != AutoStartError::None ? before.error : AutoStartError::ForeignEntry, snapshot()};
    Entry desired;
    if (enabled) {
        const auto command = windowsLaunchCommand(d->executable, {QStringLiteral("--auto-start")}).toStdWString();
        if (command.empty() || command.size() >= maximumValueBytes / sizeof(wchar_t)) return {AutoStartError::InvalidExecutable, snapshot()};
        desired.exists = true; desired.type = REG_SZ;
        desired.bytes = QByteArray(reinterpret_cast<const char*>(command.c_str()), static_cast<qsizetype>((command.size() + 1) * sizeof(wchar_t)));
    }
    const auto changed = replace(before, desired);
    if (changed != AutoStartError::None) return {changed, snapshot()};
    bool saved = true;
    try { if (persist) saved = persist(); } catch (...) { saved = false; }
    if (saved) return {AutoStartError::None, snapshot()};
    const auto restored = replace(desired, before);
    return {restored == AutoStartError::None ? AutoStartError::SaveFailed : AutoStartError::RollbackFailed, snapshot()};
}

} // namespace HuhuCapture::Desktop
