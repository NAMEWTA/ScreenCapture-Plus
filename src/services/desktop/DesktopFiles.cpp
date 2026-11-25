#include "services/desktop/DesktopFiles.h"

#include <QDir>
#include <QFile>
#include <array>
#include <filesystem>
#include <new>

#ifdef Q_OS_WIN
#include "platform/windows/DesktopFiles.h"
#endif

namespace HuhuCapture::Desktop {
namespace {
std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    const auto encoded = QFile::encodeName(path);
    return std::filesystem::path(encoded.constData(), encoded.constData() + encoded.size());
#endif
}
Failure fromArchive(const Media::Failure& failure)
{
    const auto code = failure.code == Media::Error::Busy ? Error::Busy :
        failure.code == Media::Error::ChangedOnDisk ? Error::ChangedOnDisk :
        failure.code == Media::Error::Cancelled ? Error::Cancelled : Error::InvalidArgument;
    return {code, 0, failure.detail};
}
Failure cancelled() { return {Error::Cancelled, 0, QStringLiteral("The file operation was cancelled")}; }
Failure changed() { return {Error::ChangedOnDisk, 0, QStringLiteral("The confirmed media changed on disk; refresh history before trying again")}; }

// QFile's fixed Unix trash implementation may successfully make the trash
// hardlink but fail to unlink the original while still returning true. Verify
// that this exact original is absent instead of treating the bool as evidence.
bool originalRemoved(const Media::Entry& entry)
{
    std::error_code error;
    const auto current = Platform::readFileSnapshot(nativePath(entry.path), error);
    if (current) return current->identity != entry.snapshot.identity;
    if (error == std::errc::no_such_file_or_directory) return true;
    // A newly introduced symlink or special file is not ours to delete. Without
    // positive evidence of the original's removal, keep an explicit failure.
    return false;
}

#ifdef Q_OS_WIN
Failure fromNative(const Platform::Windows::DesktopFileResult& result)
{
    using Native = Platform::Windows::DesktopFileError;
    const auto code = result.error == Native::Changed ? Error::ChangedOnDisk :
        result.error == Native::Cancelled ? Error::Cancelled : Error::RecycleFailed;
    return {code, result.nativeCode, QStringLiteral("Windows could not complete this file operation")};
}
#endif
}

RemovalResult recycle(const Media::MediaArchiveService& archive, const Media::Entry& entry, std::stop_token stop)
{
    RemovalResult output;
    try {
        output.entry = entry;
        if (stop.stop_requested()) { output.failure = cancelled(); return output; }
        auto reservation = archive.reserveRemoval(entry);
        if (reservation.failure) { output.failure = fromArchive(reservation.failure); return output; }
        if (!reservation.lease || !reservation.lease->removal()) {
            output.failure = {Error::InvalidArgument, 0, QStringLiteral("No removal reservation was granted")}; return output;
        }
        if (stop.stop_requested()) { output.failure = cancelled(); return output; }
        if (!reservation.lease->unchanged()) { output.failure = changed(); return output; }
        bool recycled = false;
#ifdef Q_OS_WIN
        const std::array paths{nativePath(entry.path)};
        const auto native = Platform::Windows::recycleDesktopFiles(paths, nullptr,
            Platform::Windows::RecyclePolicy::RequireRecycling,
            [&reservation](const auto&) { return reservation.lease->unchanged(); }, stop);
        recycled = native.completed();
        if (!native.recycledPath.empty()) output.recycledPath = QDir::fromNativeSeparators(QString::fromStdWString(native.recycledPath.wstring()));
        if (!recycled) output.failure = fromNative(native);
#else
        QFile file(entry.path);
        recycled = file.moveToTrash();
        if (recycled) output.recycledPath = file.fileName();
        else output.failure = {Error::RecycleFailed, static_cast<std::int64_t>(file.error()), file.errorString()};
#endif
        if (recycled && originalRemoved(entry)) { output.effect = RemovalEffect::Recycled; return output; }
        if (recycled) output.failure = {Error::RecycleFailed, 0, QStringLiteral("The system trash operation left the original file in place")};
        if (output.failure.code == Error::Cancelled || stop.stop_requested()) {
            output.failure = cancelled(); return output;
        }
        if (!reservation.lease->unchanged()) { output.failure = changed(); return output; }
        if (output.failure.code == Error::ChangedOnDisk) return output;
        output.permanentPermit = std::shared_ptr<const PermanentRemovalPermit>(new PermanentRemovalPermit(std::move(reservation.lease)));
        return output;
    } catch (const std::bad_alloc&) {
        output.failure = {Error::Allocation, 0, QStringLiteral("Insufficient memory for the file operation")};
        return output;
    }
}

RemovalResult permanentlyRemove(const std::shared_ptr<const PermanentRemovalPermit>& permit, std::stop_token stop)
{
    RemovalResult output;
    if (!permit || !permit->lease_ || !permit->lease_->removal()) {
        output.failure = {Error::InvalidArgument, 0, QStringLiteral("Permanent deletion requires the previous failed recycle permit")}; return output;
    }
    output.entry = permit->entry();
    if (stop.stop_requested()) { output.failure = cancelled(); return output; }
    if (!permit->lease_->unchanged()) { output.failure = changed(); return output; }
    if (permit->consumed_.exchange(true)) {
        output.failure = {Error::InvalidArgument, 0, QStringLiteral("This permanent removal permit has already been used")}; return output;
    }
    // Only the explicitly confirmed single regular file is removed. QFile does
    // not recursively remove directories; no new archive enumeration occurs.
    QFile file(output.entry.path);
    if (!file.remove()) { output.failure = {Error::FileOperation, static_cast<std::int64_t>(file.error()), file.errorString()}; return output; }
    if (!originalRemoved(output.entry)) {
        output.failure = {Error::FileOperation, 0, QStringLiteral("The original file still exists after deletion")}; return output;
    }
    output.effect = RemovalEffect::PermanentlyRemoved;
    return output;
}

} // namespace HuhuCapture::Desktop
