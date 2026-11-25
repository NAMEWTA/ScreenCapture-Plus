#pragma once

#include <filesystem>
#include <system_error>

namespace HuhuCapture::Platform {

enum class CommitState {
    NotCommitted,
    Committed,
    CommittedDurabilityUnknown,
};

struct CommitResult {
    CommitState state{CommitState::NotCommitted};
    // Nonempty as soon as this operation creates the final name, including
    // when a later persistence step fails. Never clean up this path on error.
    std::filesystem::path destination;
    std::error_code error;
    // Linux may publish a stable inode even if the staging name was changed.
    // Such a name is left alone; it must not be blindly removed by the caller.
    bool stagingRetained{true};
    std::error_code cleanupError;

    bool committed() const noexcept { return state != CommitState::NotCommitted; }
    bool durable() const noexcept { return state == CommitState::Committed; }
};

// Claims an empty, private-permission sibling using exclusive creation. The
// directory must exist; neither this function nor commit creates directories.
// The returned file belongs to the caller. Keep its namespace private while
// encoding/writing, close all writers, validate the complete media, then commit.
// Paths from peers are display metadata and must never be passed here.
std::filesystem::path createExclusiveTemporaryFile(
    const std::filesystem::path& directory, std::error_code& error);

// Publishes a finished regular staging file on the same filesystem without
// replacing any existing destination (including a dangling symlink).
// The platform's data flush precedes publication. Linux also syncs the affected
// directories; Windows retains MoveFileEx's write-through commit behavior.
// File format/completeness validation belongs to the media service.
//
// Parent directories are trusted, resolved application paths. The final parent
// and source must not themselves be symlinks/reparse points. The caller must own
// the staging namespace and stop its writers/renamers until this call returns;
// these functions are not a sandbox against another process with the same user
// privileges. In particular, identity-check + unlink is not an atomic operation.
//
// On NotCommitted the source is retained. On CommittedDurabilityUnknown the
// destination remains valid and is returned: report the uncertainty, do not
// acknowledge a durable receive and do not delete/retry the finished archive.
CommitResult commitFileNoReplace(const std::filesystem::path& staging,
    const std::filesystem::path& destination);

} // namespace HuhuCapture::Platform
