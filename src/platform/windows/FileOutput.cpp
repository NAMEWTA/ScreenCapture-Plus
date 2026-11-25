#include "platform/FileOutput.h"

#include <algorithm>
#include <cerrno>
#include <io.h>

namespace HuhuCapture::Platform {

std::error_code writeFileDescriptor(int descriptor, std::span<const std::uint8_t> bytes) noexcept
{
    while (!bytes.empty()) {
        const auto size = (std::min)(bytes.size(), std::size_t{1024 * 1024});
        const auto written = _write(descriptor, bytes.data(), static_cast<unsigned>(size));
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
    return _commit(descriptor) == 0 ? std::error_code{} : std::error_code{errno, std::generic_category()};
}

} // namespace HuhuCapture::Platform
