#include "platform/PrivateStorage.h"

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace HuhuCapture::Platform {

bool isPrivateStorage(const std::filesystem::path& path, bool directory, std::error_code& error)
{
    error.clear();
    struct stat value{};
    if (::lstat(path.c_str(), &value) != 0) {
        error = {errno, std::generic_category()};
        return false;
    }
    if (value.st_uid != ::geteuid() || (value.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
        (directory ? !S_ISDIR(value.st_mode) : !S_ISREG(value.st_mode))) {
        error = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    return true;
}

} // namespace HuhuCapture::Platform
