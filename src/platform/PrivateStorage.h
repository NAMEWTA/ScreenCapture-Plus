#pragma once

#include <filesystem>
#include <system_error>

namespace HuhuCapture::Platform {

// The resolved application-private directory and identity file must belong to
// the current user, be ordinary objects, and exclude other users from content
// and modification. Windows permits Qt's metadata-only rights and also
// SYSTEM/Administrators; this is not protection from machine admins or
// another process running as the same user. Never changes an existing ACL.
bool isPrivateStorage(const std::filesystem::path& path, bool directory,
    std::error_code& error);

} // namespace HuhuCapture::Platform
