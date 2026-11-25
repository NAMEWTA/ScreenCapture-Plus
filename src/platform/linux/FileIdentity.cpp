#include "platform/FileIdentity.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace HuhuCapture::Platform {

ReadFileHandle::~ReadFileHandle() { if (value_ >= 0) ::close(value_); }

ReadFileHandle openRegularFileReadOnly(const std::filesystem::path& path, std::error_code& error)
{
    error.clear();
    if (path.empty() || path.native().find('\0') != std::string::npos) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) { error = {errno, std::generic_category()}; return {}; }
    ReadFileHandle owner(descriptor);
    if (!readFileDescriptorSnapshot(descriptor, error)) return {};
    return owner;
}

std::optional<FileSnapshot> readFileDescriptorSnapshot(int descriptor,
    std::error_code& error)
{
    error.clear();
    struct stat value{};
    if (::fstat(descriptor, &value) != 0) { error = {errno, std::generic_category()}; return {}; }
    if (!S_ISREG(value.st_mode) || value.st_size < 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }
    return FileSnapshot{
        {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino)},
        static_cast<std::uint64_t>(value.st_size),
        {static_cast<std::uint64_t>(value.st_mtim.tv_sec), static_cast<std::uint64_t>(value.st_mtim.tv_nsec),
         static_cast<std::uint64_t>(value.st_ctim.tv_sec), static_cast<std::uint64_t>(value.st_ctim.tv_nsec)}};
}

std::optional<FileSnapshot> readFileSnapshot(const std::filesystem::path& path,
    std::error_code& error)
{
    error.clear();
    if (path.empty() || path.native().find('\0') != std::string::npos) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) { error = {errno, std::generic_category()}; return {}; }
    auto result = readFileDescriptorSnapshot(descriptor, error);
    // Linux close(EINTR) must not be retried on a potentially reused descriptor.
    ::close(descriptor);
    return result;
}

} // namespace HuhuCapture::Platform
