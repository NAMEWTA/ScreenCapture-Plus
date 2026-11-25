#pragma once

#include "platform/FileIdentity.h"

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

// Bounded diagnostic facts only: never includes a SID, pathname, key or ACL.
// TokenOwner fields are observed only on Windows; access means the existing
// DACL there and the existing owner-only mode on Linux.
struct PrivateStorageObservation {
    bool ownerObserved{};
    bool ownerMatchesUser{};
    bool tokenOwnerObserved{};
    bool ownerMatchesTokenOwner{};
    bool tokenOwnerMatchesUser{};
    bool accessObserved{};
    bool accessPrivate{};
};

enum class PrivateFileCreationStage {
    NotAttempted, Inspect, Reopen, SetOwner, Verify, Ready,
};

struct PrivateFileCreation {
    PrivateFileCreationStage stage{PrivateFileCreationStage::NotAttempted};
    PrivateStorageObservation before;
    PrivateStorageObservation after;
    bool ownerChanged{};
    bool unnamedTemporary{};
};

// For a file exclusively created by this application, BEFORE its first write.
// The caller must retain its creation claim and pass that exact object's fd.
// Requires an ordinary empty file with the expected identity and already-private
// access: singly linked, or a Linux O_TMPFILE object not yet named by QSaveFile.
// Never reopens a pathname or changes a DACL/mode.
// Windows may set TokenUser as owner only when the observed owner is exactly
// this process's TokenOwner. Existing identities and external files must use
// the strict read-only isPrivateStorage predicate instead.
bool initializeCreatedPrivateFile(int descriptor, FileIdentity expected,
    std::error_code& error, PrivateFileCreation& observation);

} // namespace HuhuCapture::Platform
