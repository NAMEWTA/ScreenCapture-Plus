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

// Only for an empty regular staging file that the caller has just created and
// still owns. Borrows a POSIX / Windows CRT descriptor without closing it. The
// caller must invoke this before writing any private bytes, never for an
// existing identity or trust file. Windows verifies the existing private DACL
// and, if needed, sets only this new object's owner to the current TokenUser;
// Linux verifies the current euid and exact 0600 permissions without mutation.
// Named files must have one link; Linux also permits an unlinked O_TMPFILE.
bool preparePrivateStagingFile(int borrowedDescriptor, std::error_code& error);

} // namespace HuhuCapture::Platform
