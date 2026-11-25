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

bool preparePrivateStagingFile(int borrowedDescriptor, std::error_code& error)
{
    error.clear();
    struct stat value{};
    if (::fstat(borrowedDescriptor, &value) != 0) {
        error = {errno, std::generic_category()};
        return false;
    }
    // QSaveFile can hold an unnamed O_TMPFILE until commit(), so zero links is
    // valid here. A second name could expose a supposedly private new object.
    constexpr mode_t permissionBits = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
    if (!S_ISREG(value.st_mode) || value.st_size != 0 || value.st_nlink > 1 ||
        value.st_uid != ::geteuid() || (value.st_mode & permissionBits) != (S_IRUSR | S_IWUSR)) {
        error = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    return true;
}

} // namespace HuhuCapture::Platform
