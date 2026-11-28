#include "services/media/ArchiveExport.h"
#include "core/MediaFileName.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <utility>

namespace HuhuCapture::Media {
namespace {
std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toStdWString();
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
QString qtPath(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native());
#else
    return QString::fromUtf8(path.native());
#endif
}
struct Destination {
    Failure failure;
    std::optional<Platform::FileSnapshot> snapshot;
};
struct TemporaryFile {
    std::filesystem::path path;
    Platform::FileIdentity identity;
    ~TemporaryFile()
    {
        if (path.empty()) return;
        std::error_code error;
        const auto current = Platform::readFileSnapshot(path, error);
        if (current && current->identity == identity) std::filesystem::remove(path, error);
    }
};
Destination inspectDestination(const std::filesystem::path& path)
{
    std::error_code error;
    const auto type = std::filesystem::symlink_status(path, error).type();
    if (type == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory)) return {};
    if (error || type != std::filesystem::file_type::regular)
        return {{Error::Write, QStringLiteral("The selected destination is not an ordinary file")}, {}};
    auto snapshot = Platform::readFileSnapshot(path, error);
    if (!snapshot) return {{Error::Read, QString::fromStdString(error.message())}, {}};
    return {{}, snapshot};
}
}

DestinationResult inspectExportDestination(const QString& selectedPath)
{
    if (selectedPath.isEmpty() || !QDir::isAbsolutePath(selectedPath) || selectedPath.contains(QChar::Null))
        return {{Error::Write, QStringLiteral("An explicitly selected absolute destination is required")}, {}};
    const auto requested = nativePath(QDir::cleanPath(selectedPath));
    std::error_code error;
    const auto parent = std::filesystem::canonical(requested.parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error))
        return {{Error::Directory, QStringLiteral("The selected destination directory is unavailable")}, {}};
    const auto target = parent / requested.filename();
    const auto current = inspectDestination(target);
    if (current.failure) return {current.failure, {}};
    return {{}, {qtPath(target), current.snapshot}};
}

ExportResult exportArchiveFile(const MediaArchiveService& archive, const Entry& source,
    const ExportDestination& confirmedDestination, std::stop_token stop)
{
    ExportResult result;
    const auto fail = [&](Error code, QString detail = {}) {
        result.failure = {code, std::move(detail)}; return result;
    };
    if (stop.stop_requested()) return fail(Error::Cancelled);
    auto sourceLease = archive.lease(source);
    if (sourceLease.failure) { result.failure = sourceLease.failure; return result; }
    const auto selected = inspectExportDestination(confirmedDestination.path);
    if (selected.failure || selected.destination.path != confirmedDestination.path ||
        selected.destination.existing != confirmedDestination.existing)
        return fail(Error::ChangedOnDisk, QStringLiteral("The destination changed after confirmation"));
    const auto target = nativePath(confirmedDestination.path);
    const auto parent = target.parent_path();
    std::error_code error;
    result.path = confirmedDestination.path;
    const auto& original = confirmedDestination.existing;
    // Selecting the same file or a hard-link alias requires no destructive write.
    if (original && original->identity == source.snapshot.identity) {
        if (*original != source.snapshot || !sourceLease.lease->unchanged())
            return fail(Error::ChangedOnDisk, QStringLiteral("The selected archive file changed"));
        result.committed = true; return result;
    }

    std::shared_ptr<const MediaLease> targetReservation;
    if (original) {
        const auto archiveRoot = std::filesystem::canonical(nativePath(archive.directory()), error);
        const auto parsed = Core::parseMediaFileName(qtPath(target.filename()).toStdWString());
        if (!error && parent == archiveRoot && parsed) {
            const auto kind = parsed->extension == L"png" ? Kind::Png : parsed->extension == L"gif" ? Kind::Gif : Kind::Mp4;
            const Entry entry{result.path, qtPath(target.filename()), kind, *original};
            auto reserved = archive.reserveRemoval(entry);
            if (reserved.failure) { result.failure = reserved.failure; return result; }
            targetReservation = std::move(reserved.lease);
        }
    }
    auto handle = Platform::openRegularFileReadOnly(nativePath(source.path), error);
    if (!handle) return fail(Error::Read, QString::fromStdString(error.message()));
    QFile input;
    if (!input.open(handle.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return fail(Error::Read, input.errorString());
    const auto opened = Platform::readFileDescriptorSnapshot(input.handle(), error);
    if (!opened || *opened != source.snapshot)
        return fail(Error::ChangedOnDisk, QStringLiteral("The selected source changed before export"));
    TemporaryFile temporary;
    std::optional<Platform::FileHandle> writerHandle;
    QFile created;
    QSaveFile replacement(result.path);
    replacement.setDirectWriteFallback(false);
    QFileDevice* output{};
    if (original) {
        if (!replacement.open(QIODevice::WriteOnly)) return fail(Error::Write, replacement.errorString());
        output = &replacement;
    } else {
        const auto path = Platform::createExclusiveTemporaryFile(parent, error);
        if (error || path.empty()) return fail(Error::Write, QString::fromStdString(error.message()));
        const auto identity = Platform::readFileSnapshot(path, error);
        if (!identity) return fail(Error::Write, QStringLiteral("Could not establish ownership of the export temporary file"));
        temporary.path = path; temporary.identity = identity->identity;
        writerHandle.emplace(Platform::openOwnedEmptyFileForWrite(path, identity->identity, error));
        if (!*writerHandle) return fail(Error::Write, QString::fromStdString(error.message()));
        if (!created.open(writerHandle->get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle))
            return fail(Error::Write, created.errorString());
        output = &created;
    }
    std::array<char, 64 * 1024> bytes{};
    std::uint64_t total{};
    while (total < source.snapshot.bytes) {
        if (stop.stop_requested()) return fail(Error::Cancelled);
        const auto count = input.read(bytes.data(), static_cast<qint64>((std::min)(
            static_cast<std::uint64_t>(bytes.size()), source.snapshot.bytes - total)));
        if (count <= 0) return fail(Error::Read, input.errorString());
        qint64 written{};
        while (written < count) {
            const auto part = output->write(bytes.data() + written, count - written);
            if (part <= 0) return fail(Error::Write, output->errorString());
            written += part;
        }
        total += static_cast<std::uint64_t>(count);
    }
    const auto after = Platform::readFileDescriptorSnapshot(input.handle(), error);
    if (!after || *after != source.snapshot || !sourceLease.lease->unchanged() || !input.atEnd())
        return fail(Error::ChangedOnDisk, QStringLiteral("The selected source changed during export"));
    if (!output->flush()) return fail(Error::Write, output->errorString());
    if (stop.stop_requested()) return fail(Error::Cancelled);
    const auto current = inspectDestination(target);
    if (current.failure || current.snapshot != original ||
        (targetReservation && !targetReservation->unchanged()))
        return fail(Error::ChangedOnDisk, QStringLiteral("The destination changed after confirmation"));
    if (original) {
        if (!replacement.commit()) return fail(Error::Write, replacement.errorString());
    } else {
        created.close();
        if (created.error() != QFileDevice::NoError || !writerHandle->close(error))
            return fail(Error::Write, QStringLiteral("Could not close the completed export writer"));
        const auto ready = Platform::readFileSnapshot(temporary.path, error);
        if (!ready || ready->identity != temporary.identity || ready->bytes != source.snapshot.bytes)
            return fail(Error::ChangedOnDisk, QStringLiteral("The owned export temporary file changed"));
        const auto committed = Platform::commitFileNoReplace(temporary.path, target);
        result.committed = committed.committed();
        if (!committed.durable()) return fail(committed.committed() ? Error::DurabilityUnknown : Error::Commit,
            QString::fromStdString(committed.error.message()));
    }
    result.committed = true;
    return result;
}

} // namespace HuhuCapture::Media
