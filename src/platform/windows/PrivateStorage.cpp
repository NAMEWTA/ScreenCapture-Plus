#include "platform/PrivateStorage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#include <array>
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

bool ordinaryObject(HANDLE file, bool directory, BY_HANDLE_FILE_INFORMATION& info,
    std::error_code& error)
{
    if (!GetFileInformationByHandle(file, &info)) return fail(error);
    if (GetFileType(file) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return fail(error, ERROR_ACCESS_DENIED);
    return true;
}

bool currentUser(std::vector<unsigned char>& userBytes, std::error_code& error)
{
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return fail(error);
    const Handle token(rawToken);
    DWORD length{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    if (!length || length > 65536) return fail(error);
    userBytes.resize(length);
    if (!GetTokenInformation(token.get(), TokenUser, userBytes.data(), length, &length)) return fail(error);
    return true;
}

bool privateDacl(HANDLE file, bool directory, PSID user, bool& ownerMatches,
    std::error_code& error)
{
    ownerMatches = false;
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
    if (!owner || !IsValidSid(owner) || !dacl || !IsValidAcl(dacl))
        return fail(error, ERROR_ACCESS_DENIED);
    ownerMatches = EqualSid(owner, user) != FALSE;
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
        if (!IsValidSid(sid)) return fail(error, ERROR_ACCESS_DENIED);
        const bool privileged = EqualSid(sid, user) || EqualSid(sid, systemBytes.data()) || EqualSid(sid, adminBytes.data());
        // Qt 6.12's owner-only QNativeFilePermissions deliberately retains
        // these metadata rights for the group/Everyone. They grant no reading
        // of file data, listing children, executing, writing or ACL ownership.
        // Any wider right (including generic rights) on another SID is refused.
        constexpr ACCESS_MASK metadataOnly = READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        if (!privileged && (allowed->Mask & ~metadataOnly)) return fail(error, ERROR_ACCESS_DENIED);
    }
    return true;
}

bool emptyStagingObject(HANDLE file, std::error_code& error)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!ordinaryObject(file, false, info, error)) return false;
    if (info.nFileSizeHigh || info.nFileSizeLow || info.nNumberOfLinks != 1)
        return fail(error, ERROR_ACCESS_DENIED);
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
    BY_HANDLE_FILE_INFORMATION info{};
    if (!ordinaryObject(file.get(), directory, info, error)) return false;
    std::vector<unsigned char> userBytes;
    if (!currentUser(userBytes, error)) return false;
    const auto user = reinterpret_cast<TOKEN_USER*>(userBytes.data())->User.Sid;
    bool ownerMatches{};
    if (!privateDacl(file.get(), directory, user, ownerMatches, error)) return false;
    return ownerMatches || fail(error, ERROR_ACCESS_DENIED);
}

bool preparePrivateStagingFile(int borrowedDescriptor, std::error_code& error)
{
    error.clear();
    if (borrowedDescriptor < 0) return fail(error, ERROR_INVALID_HANDLE);
    const auto value = _get_osfhandle(borrowedDescriptor);
    if (value == -1) return fail(error, ERROR_INVALID_HANDLE);
    // This handle belongs to QFile/the caller. Neither this function nor the
    // temporary metadata handle takes ownership of the borrowed CRT descriptor.
    const auto file = reinterpret_cast<HANDLE>(value);
    if (!emptyStagingObject(file, error)) return false;
    std::vector<unsigned char> userBytes;
    if (!currentUser(userBytes, error)) return false;
    const auto user = reinterpret_cast<TOKEN_USER*>(userBytes.data())->User.Sid;
    bool ownerMatches{};
    if (!privateDacl(file, false, user, ownerMatches, error)) return false;
    if (!ownerMatches) {
        // ReOpenFile addresses the same open object, not its mutable pathname.
        // Metadata-only access requests no data/delete sharing, while all three
        // share flags preserve QFile/QSaveFile's existing data/delete access.
        // No backup/restore privilege, WRITE_DAC or token changes are used.
        const Handle metadata(ReOpenFile(file, READ_CONTROL | WRITE_OWNER,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0));
        if (!metadata) return fail(error);
        if (!emptyStagingObject(file, error) ||
            !privateDacl(metadata.get(), false, user, ownerMatches, error)) return false;
        if (!ownerMatches) {
            const auto status = SetSecurityInfo(metadata.get(), SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION, user, nullptr, nullptr, nullptr);
            if (status != ERROR_SUCCESS) return fail(error, status);
        }
    }
    // The complete strict rule still has to hold after preparation. In
    // particular, setting an owner never repairs an unsafe existing DACL.
    if (!emptyStagingObject(file, error) ||
        !privateDacl(file, false, user, ownerMatches, error)) return false;
    return ownerMatches || fail(error, ERROR_ACCESS_DENIED);
}

} // namespace HuhuCapture::Platform
