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
} // namespace

bool isPrivateStorage(const std::filesystem::path& path, bool directory, std::error_code& error)
{
    error.clear();
    const Handle file(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
    if (!file) return fail(error);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) return fail(error);
    if (GetFileType(file.get()) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return fail(error, ERROR_ACCESS_DENIED);

    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return fail(error);
    const Handle token(rawToken);
    DWORD length{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    if (!length || length > 65536) return fail(error);
    std::vector<unsigned char> userBytes(length);
    if (!GetTokenInformation(token.get(), TokenUser, userBytes.data(), length, &length)) return fail(error);
    const auto user = reinterpret_cast<TOKEN_USER*>(userBytes.data())->User.Sid;

    std::array<unsigned char, SECURITY_MAX_SID_SIZE> systemBytes{}, adminBytes{};
    DWORD systemSize = static_cast<DWORD>(systemBytes.size()), adminSize = static_cast<DWORD>(adminBytes.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBytes.data(), &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminBytes.data(), &adminSize)) return fail(error);

    PSID owner{};
    PACL dacl{};
    PSECURITY_DESCRIPTOR descriptor{};
    const auto status = GetSecurityInfo(file.get(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) return fail(error, status);
    struct DescriptorOwner {
        PSECURITY_DESCRIPTOR value;
        ~DescriptorOwner() { LocalFree(value); }
    } allocation{descriptor};
    if (!owner || !EqualSid(owner, user) || !dacl || !IsValidAcl(dacl))
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

} // namespace HuhuCapture::Platform
