#include "platform/FileIdentity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cerrno>
#include <fcntl.h>
#include <io.h>
#include <string>

namespace HuhuCapture::Platform {
namespace {

std::optional<FileSnapshot> snapshot(HANDLE file,
    std::error_code& error)
{
    error.clear();
    BY_HANDLE_FILE_INFORMATION identity{};
    FILE_BASIC_INFO basic{};
    const auto kind = GetFileType(file);
    const bool available = GetFileInformationByHandle(file, &identity) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
    const auto savedError = GetLastError();
    if (!available) { error = {static_cast<int>(savedError), std::system_category()}; return {}; }
    if (kind != FILE_TYPE_DISK ||
        (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    return FileSnapshot{
        {identity.dwVolumeSerialNumber,
         (std::uint64_t{identity.nFileIndexHigh} << 32) | identity.nFileIndexLow},
        (std::uint64_t{identity.nFileSizeHigh} << 32) | identity.nFileSizeLow,
        {static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart), 0,
         static_cast<std::uint64_t>(basic.ChangeTime.QuadPart), 0}};
}
} // namespace

std::optional<FileSnapshot> readFileSnapshot(const std::filesystem::path& path,
    std::error_code& error)
{
    error.clear();
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos ||
        path.filename().native().find(L':') != std::wstring::npos) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    const auto file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = {static_cast<int>(GetLastError()), std::system_category()}; return {};
    }
    auto result = snapshot(file, error);
    CloseHandle(file);
    return result;
}

std::optional<FileSnapshot> readFileDescriptorSnapshot(int descriptor, std::error_code& error)
{
    // The caller supplies an existing QFile/CRT descriptor, never an arbitrary
    // integer from a peer. _get_osfhandle does not transfer ownership.
    if (descriptor < 0) { error = std::make_error_code(std::errc::bad_file_descriptor); return {}; }
    const auto handle = _get_osfhandle(descriptor);
    if (handle == -1) { error = std::make_error_code(std::errc::bad_file_descriptor); return {}; }
    return snapshot(reinterpret_cast<HANDLE>(handle), error);
}

FileHandle::~FileHandle() { std::error_code ignored; close(ignored); }

bool FileHandle::close(std::error_code& error) noexcept
{
    error.clear();
    const auto fd = std::exchange(value_, -1);
    if (fd >= 0 && _close(fd) != 0) { error = {errno, std::generic_category()}; return false; }
    return true;
}

FileHandle openRegularFileReadOnly(const std::filesystem::path& path, std::error_code& error)
{
    error.clear();
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos ||
        path.filename().native().find(L':') != std::wstring::npos) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    const auto file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = {static_cast<int>(GetLastError()), std::system_category()}; return {};
    }
    if (!snapshot(file, error)) { CloseHandle(file); return {}; }
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        error = {errno, std::generic_category()}; CloseHandle(file); return {};
    }
    return FileHandle(descriptor);
}

FileHandle openOwnedEmptyFileForWrite(const std::filesystem::path& path,
    FileIdentity expected, std::error_code& error)
{
    error.clear();
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos ||
        path.filename().native().find(L':') != std::wstring::npos) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = {static_cast<int>(GetLastError()), std::system_category()}; return {};
    }
    const auto opened = snapshot(file, error);
    if (!opened || opened->identity != expected || opened->bytes != 0) {
        if (opened) error = std::make_error_code(std::errc::state_not_recoverable);
        CloseHandle(file); return {};
    }
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDWR | _O_BINARY);
    if (descriptor == -1) {
        error = {errno, std::generic_category()}; CloseHandle(file); return {};
    }
    return FileHandle(descriptor);
}

} // namespace HuhuCapture::Platform
