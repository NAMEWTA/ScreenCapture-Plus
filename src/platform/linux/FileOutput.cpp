#include "platform/FileOutput.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <unistd.h>

namespace HuhuCapture::Platform {

std::error_code writeFileDescriptor(int descriptor, std::span<const std::uint8_t> bytes) noexcept
{
    while (!bytes.empty()) {
        const auto size = (std::min)(bytes.size(), std::size_t{1024 * 1024});
        const auto written = ::write(descriptor, bytes.data(), size);
        if (written < 0) {
            const int error = errno;
            if (error == EINTR) continue;
            return {error, std::generic_category()};
        }
        if (!written) return std::make_error_code(std::errc::io_error);
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return {};
}

std::error_code flushFileDescriptor(int descriptor) noexcept
{
    while (::fsync(descriptor) != 0) {
        const int error = errno;
        if (error != EINTR) return {error, std::generic_category()};
    }
    return {};
}

std::error_code seekFileDescriptor(int descriptor, std::uint64_t absolutePosition) noexcept
{
    if (absolutePosition > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)()))
        return std::make_error_code(std::errc::value_too_large);
    for (;;) {
        const auto position = ::lseek(descriptor, static_cast<off_t>(absolutePosition), SEEK_SET);
        if (position >= 0) return static_cast<std::uint64_t>(position) == absolutePosition
            ? std::error_code{} : std::make_error_code(std::errc::io_error);
        const int error = errno;
        if (error != EINTR) return {error, std::generic_category()};
    }
}

} // namespace HuhuCapture::Platform
