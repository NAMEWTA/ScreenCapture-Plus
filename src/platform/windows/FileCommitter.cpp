#include "platform/FileCommitter.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <string>

namespace HuhuCapture::Platform {
namespace {
namespace fs = std::filesystem;

class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

std::error_code nativeError() { return {static_cast<int>(GetLastError()), std::system_category()}; }

bool usablePath(const fs::path& path)
{
    const auto name = path.filename().native();
    return !name.empty() && name != L"." && name != L".." &&
        name.find(L':') == std::wstring::npos &&
        path.native().find(L'\0') == std::wstring::npos;
}

fs::path parentOf(const fs::path& path)
{
    return path.has_parent_path() ? path.parent_path() : fs::path(L".");
}

bool checkDirectory(const fs::path& directory, std::error_code& error)
{
    if (directory.empty() || directory.native().find(L'\0') != std::wstring::npos) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    const Handle parent(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!parent) { error = nativeError(); return false; }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(parent.get(), &info)) { error = nativeError(); return false; }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    return true;
}
} // namespace

fs::path createExclusiveTemporaryFile(const fs::path& directory, std::error_code& error)
{
    error.clear();
    if (!checkDirectory(directory, error)) return {};
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        GUID id{};
        const auto hr = CoCreateGuid(&id);
        if (FAILED(hr)) { error = {static_cast<int>(hr), std::system_category()}; return {}; }
        wchar_t name[40]{};
        if (!StringFromGUID2(id, name, 40)) {
            error = std::make_error_code(std::errc::io_error);
            return {};
        }
        const auto path = directory / (L".huhucapture-" + std::wstring(name) + L".part");
        const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto native = GetLastError();
            if (native == ERROR_FILE_EXISTS || native == ERROR_ALREADY_EXISTS) continue;
            error = {static_cast<int>(native), std::system_category()};
            return {};
        }
        if (!CloseHandle(file)) {
            error = nativeError();
            DeleteFileW(path.c_str());
            return {};
        }
        return path;
    }
    error = std::make_error_code(std::errc::file_exists);
    return {};
}

CommitResult commitFileNoReplace(const fs::path& staging, const fs::path& destination)
{
    CommitResult result;
    if (!usablePath(staging) || !usablePath(destination)) {
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    if (!checkDirectory(parentOf(staging), result.error) ||
        !checkDirectory(parentOf(destination), result.error)) return result;

    // In particular reject same-file aliases (case/trailing-dot normalization)
    // before MoveFileEx can treat one as a no-op. Atomic no-replace below still
    // arbitrates destinations created after this check.
    if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        result.error = std::make_error_code(std::errc::file_exists);
        return result;
    }
    const Handle source(CreateFileW(staging.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!source) { result.error = nativeError(); return result; }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(source.get(), &info)) { result.error = nativeError(); return result; }
    if (GetFileType(source.get()) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    if (!FlushFileBuffers(source.get())) { result.error = nativeError(); return result; }

    // The caller owns a closed-writer staging namespace. The read handle stays
    // live across publication and denies new writers. Neither REPLACE_EXISTING
    // nor COPY_ALLOWED is permitted: a different filesystem is a real failure.
    if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        result.error = nativeError();
        return result;
    }
    result.state = CommitState::Committed;
    result.destination = destination;
    result.stagingRetained = false;
    return result;
}

} // namespace HuhuCapture::Platform
