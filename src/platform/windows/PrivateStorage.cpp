#include "platform/PrivateStorage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <array>
#include <cstddef>
#include <io.h>
#include <vector>

namespace HuhuCapture::Platform {
namespace {
class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_); }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
private:
    HANDLE value_;
};

bool fail(std::error_code& error, DWORD value = GetLastError())
{
    error = {static_cast<int>(value), std::system_category()};
    return false;
}

struct TokenIdentity {
    std::vector<unsigned char> userBytes;
    std::vector<unsigned char> ownerBytes;
    PSID user() const noexcept
    { return reinterpret_cast<const TOKEN_USER*>(userBytes.data())->User.Sid; }
    PSID owner() const noexcept
    { return ownerBytes.empty() ? nullptr : reinterpret_cast<const TOKEN_OWNER*>(ownerBytes.data())->Owner; }
};

bool readTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind,
    std::size_t minimum, std::vector<unsigned char>& bytes, std::error_code& error)
{
    DWORD length{};
    if (GetTokenInformation(token, kind, nullptr, 0, &length))
        return fail(error, ERROR_INVALID_DATA);
    const auto queryError = GetLastError();
    if (queryError != ERROR_INSUFFICIENT_BUFFER) return fail(error, queryError);
    if (length < minimum || length > 65536) return fail(error, ERROR_INVALID_DATA);
    bytes.resize(length);
    if (!GetTokenInformation(token, kind, bytes.data(), length, &length)) return fail(error);
    return true;
}

bool readTokenIdentity(bool includeOwner, TokenIdentity& identity, std::error_code& error)
{
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return fail(error);
    const Handle token(rawToken);
    if (!readTokenInformation(token.get(), TokenUser, sizeof(TOKEN_USER), identity.userBytes, error) ||
        (includeOwner && !readTokenInformation(token.get(), TokenOwner, sizeof(TOKEN_OWNER), identity.ownerBytes, error)))
        return false;
    if (!IsValidSid(identity.user()) || (identity.owner() && !IsValidSid(identity.owner())))
        return fail(error, ERROR_INVALID_SID);
    return true;
}

bool inspectPrivateHandle(HANDLE file, bool directory, const TokenIdentity& token,
    PrivateStorageObservation& observation, std::error_code& error)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return fail(error);
    if (GetFileType(file) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return fail(error, ERROR_ACCESS_DENIED);

    std::array<unsigned char, SECURITY_MAX_SID_SIZE> systemBytes{}, adminBytes{};
    DWORD systemSize = static_cast<DWORD>(systemBytes.size()), adminSize = static_cast<DWORD>(adminBytes.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBytes.data(), &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminBytes.data(), &adminSize)) return fail(error);

    PSID owner{};
    PACL dacl{};
    PSECURITY_DESCRIPTOR descriptor{};
    const auto status = GetSecurityInfo(file, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) return fail(error, status);
    struct DescriptorOwner {
        PSECURITY_DESCRIPTOR value;
        ~DescriptorOwner() { LocalFree(value); }
    } allocation{descriptor};
    observation.ownerObserved = owner && IsValidSid(owner);
    observation.ownerMatchesUser = observation.ownerObserved && EqualSid(owner, token.user());
    observation.tokenOwnerObserved = token.owner() != nullptr;
    observation.ownerMatchesTokenOwner = observation.ownerObserved && observation.tokenOwnerObserved && EqualSid(owner, token.owner());
    observation.tokenOwnerMatchesUser = observation.tokenOwnerObserved && EqualSid(token.owner(), token.user());
    observation.accessObserved = true;
    if (!observation.ownerObserved || !dacl || !IsValidAcl(dacl))
        return fail(error, ERROR_ACCESS_DENIED);
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* rawAce{};
        if (!GetAce(dacl, index, &rawAce)) return fail(error);
        const auto header = static_cast<ACE_HEADER*>(rawAce);
        if ((header->AceFlags & INHERIT_ONLY_ACE) && !directory) continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        // Object/callback/unknown ACEs cannot be proven private by this narrow
        // file store. Refuse to read/write, retaining the existing identity.
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceSize < sizeof(ACCESS_ALLOWED_ACE))
            return fail(error, ERROR_ACCESS_DENIED);
        const auto allowed = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
        if (!allowed->Mask) continue;
        const auto sid = const_cast<DWORD*>(&allowed->SidStart);
        if (!IsValidSid(sid) || GetLengthSid(sid) > header->AceSize - offsetof(ACCESS_ALLOWED_ACE, SidStart))
            return fail(error, ERROR_ACCESS_DENIED);
        const bool privileged = EqualSid(sid, token.user()) || EqualSid(sid, systemBytes.data()) || EqualSid(sid, adminBytes.data());
        // Qt 6.12's owner-only QNativeFilePermissions deliberately retains
        // these metadata rights for the group/Everyone. They grant no reading
        // of file data, listing children, executing, writing or ACL ownership.
        // Any wider right (including generic rights) on another SID is refused.
        constexpr ACCESS_MASK metadataOnly = READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        if (!privileged && (allowed->Mask & ~metadataOnly)) return fail(error, ERROR_ACCESS_DENIED);
    }
    observation.accessPrivate = true;
    return true;
}

bool matchesCreatedEmptyFile(HANDLE file, FileIdentity expected, std::error_code& error)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return fail(error);
    const FileIdentity actual{info.dwVolumeSerialNumber,
        (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow};
    if (GetFileType(file) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1 || info.nFileSizeHigh || info.nFileSizeLow || actual != expected) {
        error = std::make_error_code(std::errc::state_not_recoverable);
        return false;
    }
    return true;
}
} // namespace

bool isPrivateStorage(const std::filesystem::path& path, bool directory, std::error_code& error)
{
    error.clear();
    const Handle file(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
    if (!file) return fail(error);
    TokenIdentity token;
    PrivateStorageObservation observation;
    if (!readTokenIdentity(false, token, error) ||
        !inspectPrivateHandle(file.get(), directory, token, observation, error)) return false;
    return observation.ownerMatchesUser || fail(error, ERROR_ACCESS_DENIED);
}

bool initializeCreatedPrivateFile(int descriptor, FileIdentity expected,
    std::error_code& error, PrivateFileCreation& observation)
{
    error.clear();
    observation = {};
    observation.stage = PrivateFileCreationStage::Inspect;
    if (descriptor < 0) return fail(error, ERROR_INVALID_HANDLE);
    const auto raw = _get_osfhandle(descriptor);
    if (raw == -1) return fail(error, ERROR_INVALID_HANDLE);
    const auto file = reinterpret_cast<HANDLE>(raw); // Borrowed, never closed here.
    if (!matchesCreatedEmptyFile(file, expected, error)) return false;
    TokenIdentity token;
    if (!readTokenIdentity(true, token, error) ||
        !inspectPrivateHandle(file, false, token, observation.before, error)) return false;
    if (!observation.before.ownerMatchesUser) {
        if (!observation.before.ownerMatchesTokenOwner) return fail(error, ERROR_ACCESS_DENIED);
        // A CREATE_NEW file can inherit TokenOwner instead of TokenUser.
        // Request metadata rights on this same kernel object, not its name.
        // Do not request data access, enable privileges or alter the private DACL.
        observation.stage = PrivateFileCreationStage::Reopen;
        const Handle ownerHandle(ReOpenFile(file, WRITE_OWNER | READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0));
        if (!ownerHandle) return fail(error);
        PrivateStorageObservation rechecked;
        if (!matchesCreatedEmptyFile(ownerHandle.get(), expected, error) ||
            !inspectPrivateHandle(ownerHandle.get(), false, token, rechecked, error)) return false;
        if (!rechecked.ownerMatchesTokenOwner) return fail(error, ERROR_ACCESS_DENIED);
        observation.stage = PrivateFileCreationStage::SetOwner;
        const auto status = SetSecurityInfo(ownerHandle.get(), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION, token.user(), nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS) return fail(error, status);
        observation.ownerChanged = true;
    }
    observation.stage = PrivateFileCreationStage::Verify;
    if (!matchesCreatedEmptyFile(file, expected, error) ||
        !inspectPrivateHandle(file, false, token, observation.after, error)) return false;
    if (!observation.after.ownerMatchesUser) return fail(error, ERROR_ACCESS_DENIED);
    observation.stage = PrivateFileCreationStage::Ready;
    return true;
}

} // namespace HuhuCapture::Platform
