#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace HuhuCapture::Platform {

// Stable volume/file identity, not a pathname or a thumbnail cache key.
struct FileIdentity {
    std::uint64_t volume{};
    std::uint64_t file{};
    auto operator<=>(const FileIdentity&) const = default;
};

struct FileSnapshot {
    FileIdentity identity;
    std::uint64_t bytes{};
    // Opaque native modification/change timestamps, used for equality only.
    std::array<std::uint64_t, 4> revision{};
    bool operator==(const FileSnapshot&) const = default;
};

// Opens the final component without following links, rejects directories,
// reparse points and special files, and reads identity from that open object.
// This snapshot does not prevent another process from changing the file later.
// Archive operations additionally reserve an application lease and revalidate.
std::optional<FileSnapshot> readFileSnapshot(const std::filesystem::path& path,
    std::error_code& error);

// Borrows a POSIX / Windows CRT file descriptor (for example QFile::handle()).
// Reads that exact open object without closing it or reopening its pathname.
std::optional<FileSnapshot> readFileDescriptorSnapshot(int descriptor,
    std::error_code& error);

class FileHandle final {
public:
    FileHandle() noexcept = default;
    ~FileHandle();
    FileHandle(FileHandle&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    FileHandle& operator=(FileHandle&&) = delete;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    int get() const noexcept { return value_; }
    bool close(std::error_code& error) noexcept;
    explicit operator bool() const noexcept { return value_ >= 0; }
private:
    explicit FileHandle(int value) noexcept : value_(value) {}
    int value_{-1};
    friend FileHandle openRegularFileReadOnly(const std::filesystem::path&, std::error_code&);
    friend FileHandle openOwnedEmptyFileForWrite(const std::filesystem::path&, FileIdentity, std::error_code&);
};

// Opens once without following the final symlink/reparse point or blocking on
// a FIFO. Windows denies content writers for the lifetime of this handle.
// QFile may borrow get() with DontCloseHandle; it must not reopen the path.
FileHandle openRegularFileReadOnly(const std::filesystem::path& path, std::error_code& error);

// Opens an already-claimed empty file read/write WITHOUT truncating or following
// links. The same opened object's identity and empty length must match before
// any caller may write. A substituted path is never truncated as a side effect.
FileHandle openOwnedEmptyFileForWrite(const std::filesystem::path& path,
    FileIdentity expected, std::error_code& error);

} // namespace HuhuCapture::Platform
