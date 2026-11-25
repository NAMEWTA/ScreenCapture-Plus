#include "platform/FileCommitter.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace HuhuCapture::Platform {
namespace {
namespace fs = std::filesystem;

class Descriptor final {
public:
    explicit Descriptor(int value = -1) noexcept : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ >= 0; }
private:
    int value_;
};

std::error_code nativeError() { return {errno, std::generic_category()}; }

bool usablePath(const fs::path& path)
{
    const auto name = path.filename().native();
    return !name.empty() && name != "." && name != ".." &&
        path.native().find('\0') == std::string::npos;
}

fs::path parentOf(const fs::path& path)
{
    return path.has_parent_path() ? path.parent_path() : fs::path(".");
}

int openDirectory(const fs::path& path)
{
    if (path.empty() || path.native().find('\0') != std::string::npos) {
        errno = EINVAL;
        return -1;
    }
    return ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

bool syncFile(int descriptor)
{
    int result;
    do { result = ::fsync(descriptor); } while (result != 0 && errno == EINTR);
    return result == 0;
}

bool sameFile(const struct stat& left, const struct stat& right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool randomName(std::string& name, std::error_code& error)
{
    std::array<unsigned char, 16> random{};
    std::size_t offset = 0;
    while (offset < random.size()) {
        const auto amount = ::getrandom(random.data() + offset, random.size() - offset, 0);
        if (amount < 0) {
            if (errno == EINTR) continue;
            error = nativeError();
            return false;
        }
        if (!amount) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        offset += static_cast<std::size_t>(amount);
    }
    constexpr char hex[] = "0123456789abcdef";
    name = ".huhucapture-";
    for (const auto value : random) {
        name += hex[value >> 4];
        name += hex[value & 15];
    }
    name += ".part";
    return true;
}

bool publishDescriptor(int source, int directory, const char* name)
{
    // Unlike renameat2(sourceName), this publishes the inode we actually
    // validated and flushed, even if the source pathname was replaced while
    // fsync was running. linkat is atomic and never replaces a destination.
    // AT_EMPTY_PATH may require a capability; /proc/self/fd is the documented
    // unprivileged alternative. Both keep the source fd open until completion.
    if (::linkat(source, "", directory, name, AT_EMPTY_PATH) == 0) return true;
    if (errno != EPERM && errno != EACCES && errno != ENOENT && errno != EINVAL) return false;
    const auto sourceLink = "/proc/self/fd/" + std::to_string(source);
    return ::linkat(AT_FDCWD, sourceLink.c_str(), directory, name, AT_SYMLINK_FOLLOW) == 0;
}
} // namespace

fs::path createExclusiveTemporaryFile(const fs::path& directory, std::error_code& error)
{
    error.clear();
    const Descriptor parent(openDirectory(directory));
    if (!parent) { error = nativeError(); return {}; }
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        std::string name;
        if (!randomName(name, error)) return {};
        const auto file = ::openat(parent.get(), name.c_str(),
            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (file < 0) {
            if (errno == EEXIST) continue;
            error = nativeError();
            return {};
        }
        if (::close(file) != 0) {
            // On Linux close must not be retried after EINTR: the fd has
            // already been released and may have been reused by another thread.
            error = nativeError();
            ::unlinkat(parent.get(), name.c_str(), 0);
            return {};
        }
        return directory / name;
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
    const Descriptor sourceDirectory(openDirectory(parentOf(staging)));
    if (!sourceDirectory) { result.error = nativeError(); return result; }
    const Descriptor targetDirectory(openDirectory(parentOf(destination)));
    if (!targetDirectory) { result.error = nativeError(); return result; }
    struct stat sourceParent{}, targetParent{};
    if (::fstat(sourceDirectory.get(), &sourceParent) != 0 ||
        ::fstat(targetDirectory.get(), &targetParent) != 0) {
        result.error = nativeError();
        return result;
    }
    const bool sameDirectory = sameFile(sourceParent, targetParent);
    if (sameDirectory && staging.filename() == destination.filename()) {
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    const auto sourceName = staging.filename().native();
    const Descriptor source(::openat(sourceDirectory.get(), sourceName.c_str(),
        O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!source) { result.error = nativeError(); return result; }
    struct stat original{};
    if (::fstat(source.get(), &original) != 0) { result.error = nativeError(); return result; }
    if (!S_ISREG(original.st_mode)) {
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    if (original.st_dev != targetParent.st_dev) {
        result.error = std::make_error_code(std::errc::cross_device_link);
        return result;
    }
    if (!syncFile(source.get())) { result.error = nativeError(); return result; }
    if (!publishDescriptor(source.get(), targetDirectory.get(), destination.filename().c_str())) {
        result.error = nativeError();
        return result;
    }

    result.state = CommitState::CommittedDurabilityUnknown;
    result.destination = destination;
    // From this point onward no error path may delete the destination. In
    // particular a directory fsync failure cannot be treated as a failed rename.
    if (!syncFile(targetDirectory.get())) {
        result.error = nativeError();
        return result;
    }
    result.state = CommitState::Committed;

    struct stat current{};
    if (::fstatat(sourceDirectory.get(), sourceName.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
        result.cleanupError = nativeError();
        if (errno == ENOENT) { result.stagingRetained = false; result.cleanupError.clear(); }
        return result;
    }
    if (!sameFile(original, current)) {
        // A different file now owns this pathname. It is not our temporary.
        result.cleanupError = std::make_error_code(std::errc::state_not_recoverable);
        return result;
    }
    if (::unlinkat(sourceDirectory.get(), sourceName.c_str(), 0) != 0) {
        result.cleanupError = nativeError();
        return result;
    }
    result.stagingRetained = false;
    if (!syncFile(sourceDirectory.get())) {
        result.state = CommitState::CommittedDurabilityUnknown;
        result.error = nativeError();
    }
    return result;
}

} // namespace HuhuCapture::Platform
