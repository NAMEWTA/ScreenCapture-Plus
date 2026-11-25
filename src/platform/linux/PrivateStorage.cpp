#include "platform/PrivateStorage.h"

#include <cerrno>
#include <fcntl.h>
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

bool initializeCreatedPrivateFile(int descriptor, FileIdentity expected,
    std::error_code& error, PrivateFileCreation& observation)
{
    error.clear();
    observation = {};
    observation.stage = PrivateFileCreationStage::Inspect;
    struct stat value{};
    if (::fstat(descriptor, &value) != 0) {
        error = {errno, std::generic_category()};
        return false;
    }
    const FileIdentity actual{static_cast<std::uint64_t>(value.st_dev),
        static_cast<std::uint64_t>(value.st_ino)};
    if (!S_ISREG(value.st_mode) || value.st_nlink > 1 || value.st_size != 0 || actual != expected) {
        error = std::make_error_code(std::errc::state_not_recoverable);
        return false;
    }
    if (value.st_nlink == 0) {
        // Qt's actual Linux QSaveFile engine first opens O_TMPFILE and links
        // the object only during commit. Do not accept an ordinary deleted fd.
        const auto flags = ::fcntl(descriptor, F_GETFL);
        if (flags < 0) { error = {errno, std::generic_category()}; return false; }
        if ((flags & O_TMPFILE) != O_TMPFILE) {
            error = std::make_error_code(std::errc::state_not_recoverable);
            return false;
        }
        observation.unnamedTemporary = true;
    }
    observation.before.ownerObserved = true;
    observation.before.ownerMatchesUser = value.st_uid == ::geteuid();
    observation.before.accessObserved = true;
    observation.before.accessPrivate = (value.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    if (!observation.before.ownerMatchesUser || !observation.before.accessPrivate) {
        error = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    // Exclusive creation already applies 0600 on Linux. Do not repair the
    // permissions of a different object or change an existing file's mode.
    observation.after = observation.before;
    observation.stage = PrivateFileCreationStage::Ready;
    return true;
}

} // namespace HuhuCapture::Platform
