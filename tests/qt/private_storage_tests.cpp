#include "platform/FileCommitter.h"
#include "platform/FileIdentity.h"
#include "platform/PrivateStorage.h"
#include "services/files/QtPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#else
#include <sys/stat.h>
#endif

namespace platform = HuhuCapture::Platform;
using HuhuCapture::Files::nativePath;
using HuhuCapture::Files::qtPath;

namespace {
constexpr auto ownerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr auto ownerDirectory = ownerFile | QFileDevice::ExeOwner;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void requireNative(bool condition, const char* message, const std::error_code& error)
{
    if (!condition) throw std::runtime_error(std::string(message) + ": " +
        error.category().name() + "=" + std::to_string(error.value()));
}

platform::FileSnapshot snapshot(int descriptor)
{
    std::error_code error;
    const auto value = platform::readFileDescriptorSnapshot(descriptor, error);
    requireNative(value.has_value(), "Read the actual borrowed descriptor identity", error);
    return *value;
}

platform::FileSnapshot snapshot(const QString& path)
{
    std::error_code error;
    const auto value = platform::readFileSnapshot(nativePath(path), error);
    requireNative(value.has_value(), "Read the actual named file identity", error);
    return *value;
}

QByteArray read(const QString& path)
{
    QFile input(path);
    require(input.open(QIODevice::ReadOnly), "Open actual test file");
    const auto bytes = input.readAll();
    require(input.error() == QFileDevice::NoError, "Read complete actual test bytes");
    return bytes;
}

void createPrivate(const QString& path, const QByteArray& bytes)
{
    QFile output(path);
    require(output.open(QIODevice::NewOnly | QIODevice::ReadWrite, ownerFile),
        "Create a new file with explicit owner permissions");
    require(output.write(bytes) == bytes.size() && output.flush(), "Write exact fixture bytes");
    output.close();
    require(output.error() == QFileDevice::NoError, "Close fixture file");
}

void requirePrivate(const QString& path)
{
    std::error_code error;
    requireNative(platform::isPrivateStorage(nativePath(path), false, error),
        "Actual published file satisfies the unchanged owner and private ACL rule", error);
}

#ifdef _WIN32
struct SecuritySnapshot {
    QByteArray owner;
    QByteArray dacl;
};

HANDLE borrowedNative(int descriptor)
{
    require(descriptor >= 0, "Valid borrowed CRT descriptor");
    const auto value = _get_osfhandle(descriptor);
    require(value != -1, "Borrow the original Windows HANDLE without taking ownership");
    return reinterpret_cast<HANDLE>(value);
}

SecuritySnapshot security(int descriptor)
{
    PSID owner{};
    PACL dacl{};
    PSECURITY_DESCRIPTOR allocation{};
    const auto status = GetSecurityInfo(borrowedNative(descriptor), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &allocation);
    const auto cleanup = qScopeGuard([&] { if (allocation) LocalFree(allocation); });
    require(status == ERROR_SUCCESS && owner && IsValidSid(owner) && dacl && IsValidAcl(dacl),
        "Read actual owner and DACL bytes without logging security identifiers");
    return {QByteArray(static_cast<const char*>(owner), static_cast<qsizetype>(GetLengthSid(owner))),
        QByteArray(reinterpret_cast<const char*>(dacl), static_cast<qsizetype>(dacl->AclSize))};
}

void requireMetadataReopen(int descriptor)
{
    // Exercise this even if the current runner's default owner already equals
    // TokenUser: QSaveFile's real Win32NonShared handle must remain open here.
    const auto metadata = ReOpenFile(borrowedNative(descriptor), READ_CONTROL | WRITE_OWNER,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    const std::error_code error(metadata == INVALID_HANDLE_VALUE ? static_cast<int>(GetLastError()) : 0,
        std::system_category());
    requireNative(metadata != INVALID_HANDLE_VALUE,
        "Metadata-only ReOpenFile coexists with the actual exclusive QSaveFile handle", error);
    require(CloseHandle(metadata) != FALSE, "Close only the separately owned metadata handle");
    (void)snapshot(descriptor);
}
#endif

void prepare(int descriptor)
{
    const auto before = snapshot(descriptor);
    require(before.bytes == 0, "Preparation begins on a truly empty object");
#ifdef _WIN32
    const auto originalSecurity = security(descriptor);
#endif
    std::error_code error;
    requireNative(platform::preparePrivateStagingFile(descriptor, error),
        "Prepare the exact new private staging object", error);
    require(!error, "Successful preparation clears the native error");
    const auto after = snapshot(descriptor);
    require(after.identity == before.identity && after.bytes == 0,
        "Preparation preserves the object and leaves the borrowed descriptor open and empty");
#ifdef _WIN32
    require(security(descriptor).dacl == originalSecurity.dacl,
        "Owner preparation never changes any DACL bytes");
#else
    struct stat value{};
    require(::fstat(descriptor, &value) == 0 && value.st_nlink <= 1 && (value.st_mode & 07777) == 0600,
        "Real Linux staging retains 0600 and at most one link, including O_TMPFILE");
#endif
}

void genericStaging(const QString& directory)
{
    std::error_code error;
    const auto staging = platform::createExclusiveTemporaryFile(nativePath(directory), error);
    requireNative(!staging.empty(), "Create real generic staging with platform default ownership", error);
    const auto claim = platform::readFileSnapshot(staging, error);
    requireNative(claim.has_value(), "Claim the created staging identity", error);
    auto handle = platform::openOwnedEmptyFileForWrite(staging, claim->identity, error);
    requireNative(bool(handle), "Open only the claimed empty staging file", error);
    prepare(handle.get());
    requirePrivate(qtPath(staging));
    QFile output;
    require(output.open(handle.get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle),
        "Qt borrows the prepared descriptor without owning it");
    const QByteArray bytes("private test payload");
    require(output.write(bytes) == bytes.size() && output.flush(), "Borrowed descriptor remains writable");
    const auto written = snapshot(handle.get());
#ifdef _WIN32
    const auto originalSecurity = security(handle.get());
#endif
    require(!platform::preparePrivateStagingFile(handle.get(), error) && error,
        "Reject a nonempty file before any owner or ACL modification");
    require(snapshot(handle.get()) == written, "Nonempty rejection preserves the actual open file");
#ifdef _WIN32
    const auto after = security(handle.get());
    require(after.owner == originalSecurity.owner && after.dacl == originalSecurity.dacl,
        "Nonempty rejection preserves both owner and DACL bytes");
#endif
    output.close();
    require(output.error() == QFileDevice::NoError, "Close Qt's borrowed wrapper");
    requireNative(handle.close(error), "The original owner can still close the descriptor", error);
    require(read(qtPath(staging)) == bytes, "Nonempty rejection retains all written bytes");
}

void saveFileStaging(const QString& directory)
{
    const auto path = QDir(directory).filePath(QStringLiteral("trusted-peers.json"));
    const QByteArray original("existing private target");
    createPrivate(path, original);
    const auto originalIdentity = snapshot(path).identity;
    {
        QSaveFile cancelled(path);
        cancelled.setDirectWriteFallback(false);
        require(cancelled.open(QIODevice::WriteOnly), "Open actual QSaveFile cancellation staging");
        prepare(cancelled.handle());
        require(snapshot(path).identity == originalIdentity && read(path) == original,
            "Preparation does not change the existing target");
        require(cancelled.write("cancelled") == 9, "Write only to the new temporary object");
        cancelled.cancelWriting();
        require(!cancelled.commit(), "A cancelled save is never published");
    }
    require(snapshot(path).identity == originalIdentity && read(path) == original,
        "Cancelled prepared QSaveFile preserves the original file and bytes");
    requirePrivate(path);

    const QByteArray replacement("replacement private target");
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    require(output.open(QIODevice::WriteOnly), "Open actual QSaveFile replacement staging");
#ifdef _WIN32
    requireMetadataReopen(output.handle());
#endif
    prepare(output.handle());
    const auto stagedIdentity = snapshot(output.handle()).identity;
    require(stagedIdentity != originalIdentity, "QSaveFile uses a distinct new staging object");
    require(output.write(replacement) == replacement.size() && output.flush(),
        "Prepared QSaveFile's original descriptor remains writable");
    require(snapshot(path).identity == originalIdentity && read(path) == original,
        "The original target stays unchanged until QSaveFile's own atomic commit");
    require(output.commit(), "Use the existing actual QSaveFile atomic replacement");
    require(snapshot(path).identity == stagedIdentity && read(path) == replacement,
        "Commit publishes the exact prepared object and complete replacement bytes");
    requirePrivate(path);
}

void rejectedStaging(const QString& directory)
{
    std::error_code error;
    require(!platform::preparePrivateStagingFile(-1, error) && error,
        "An invalid borrowed descriptor is rejected");

    const auto linkedPath = QDir(directory).filePath(QStringLiteral("linked.part"));
    createPrivate(linkedPath, {});
    const auto alias = QDir(directory).filePath(QStringLiteral("second-name.part"));
    std::filesystem::create_hard_link(nativePath(linkedPath), nativePath(alias), error);
    requireNative(!error, "Create an actual second hard link to an empty fixture", error);
    const auto linkedIdentity = snapshot(linkedPath).identity;
    auto linked = platform::openOwnedEmptyFileForWrite(nativePath(linkedPath), linkedIdentity, error);
    requireNative(bool(linked), "Open the actual multiply linked empty object", error);
    const auto before = snapshot(linked.get());
    require(!platform::preparePrivateStagingFile(linked.get(), error) && error,
        "Multiple hard links are refused before private bytes are written");
    require(snapshot(linked.get()) == before && snapshot(alias).identity == before.identity,
        "Hard-link rejection preserves the borrowed descriptor and both original names");

    const auto broadPath = QDir(directory).filePath(QStringLiteral("readable-by-others.part"));
    QFile broad(broadPath);
    require(broad.open(QIODevice::NewOnly | QIODevice::ReadWrite, ownerFile | QFileDevice::ReadOther),
        "Create an actual empty fixture granting another principal content read access");
#ifndef _WIN32
    // Creation mode is filtered by umask; make this negative fixture's wider
    // POSIX mode explicit without changing any production permission path.
    require(broad.setPermissions(ownerFile | QFileDevice::ReadOther), "Set the new negative fixture's actual mode");
#endif
    const auto broadBefore = snapshot(broad.handle());
#ifdef _WIN32
    const auto originalSecurity = security(broad.handle());
#else
    struct stat originalPermissions{};
    require(::fstat(broad.handle(), &originalPermissions) == 0, "Read actual original mode");
#endif
    require(!platform::preparePrivateStagingFile(broad.handle(), error) && error,
        "An unsafe data permission is refused rather than repaired");
    require(snapshot(broad.handle()) == broadBefore, "Permission rejection leaves the object and descriptor unchanged");
#ifdef _WIN32
    const auto after = security(broad.handle());
    require(after.owner == originalSecurity.owner && after.dacl == originalSecurity.dacl,
        "Unsafe DACL rejection changes neither owner nor DACL");
#else
    struct stat after{};
    require(::fstat(broad.handle(), &after) == 0 && after.st_mode == originalPermissions.st_mode,
        "Linux verification never chmods an unsafe staging file");
#endif
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Create actual isolated test directory");
        const auto directory = QDir(temporary.path()).filePath(QStringLiteral("private"));
        require(QDir().mkpath(directory, ownerDirectory), "Create actual private directory with explicit permissions");
        std::error_code error;
        requireNative(platform::isPrivateStorage(nativePath(directory), true, error),
            "Actual parent directory has private owner and data permissions", error);
        genericStaging(directory);
        saveFileStaging(directory);
        rejectedStaging(directory);
        std::cout << "PASS actual generic and QSaveFile private staging ownership, atomic publication and refusal contracts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL actual private staging: " << error.what() << '\n';
        return 1;
    }
}
