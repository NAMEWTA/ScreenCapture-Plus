#include "services/media/ThumbnailCache.h"
#include "core/PixelBuffer.h"

#include <QCache>
#include <QFile>
#include <QImageReader>
#include <algorithm>
#include <filesystem>
#include <mutex>

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
QString cacheKey(const Entry& entry, QSize size)
{
    QString result = entry.path + QChar::Null;
    const auto append = [&](std::uint64_t value) { result += QString::number(value, 16) + QChar(u':'); };
    append(entry.snapshot.identity.volume); append(entry.snapshot.identity.file);
    append(entry.snapshot.bytes);
    for (const auto value : entry.snapshot.revision) append(value);
    append(static_cast<unsigned>(entry.kind)); append(size.width()); append(size.height());
    return result;
}
VideoThumbnailResult failed(Error code, const char* detail = "")
{ return {{code, QString::fromLatin1(detail)}, {}}; }
}

struct ThumbnailCache::State {
    std::shared_ptr<MediaArchiveService> archive;
    VideoCoverReader videoCover;
    Limits limits;
    std::mutex mutex;
    QCache<QString, QImage> images{64 * 1024}; // KiB, at most 64 MiB of decoded covers.
};
ThumbnailCache::ThumbnailCache(std::shared_ptr<MediaArchiveService> archive,
    VideoCoverReader videoCover, Limits limits) : state_(std::make_unique<State>())
{
    state_->archive = std::move(archive); state_->videoCover = std::move(videoCover); state_->limits = limits;
}
ThumbnailCache::~ThumbnailCache() = default;
void ThumbnailCache::clear() { std::lock_guard lock(state_->mutex); state_->images.clear(); }

VideoThumbnailResult ThumbnailCache::load(const Entry& entry, QSize maximumSize, std::stop_token stop)
{
    if (stop.stop_requested()) return failed(Error::Cancelled);
    if (!state_->archive || maximumSize.width() <= 0 || maximumSize.height() <= 0 ||
        maximumSize.width() > 2048 || maximumSize.height() > 2048)
        return failed(Error::Budget, "Invalid thumbnail dimensions");
    auto reserved = state_->archive->lease(entry);
    if (reserved.failure) return {reserved.failure, {}};
    if (entry.snapshot.bytes > state_->limits.maximumFileBytes)
        return failed(Error::Budget, "Media exceeds the preview byte budget");
    const auto key = cacheKey(entry, maximumSize);
    {
        std::lock_guard lock(state_->mutex);
        if (const auto* image = state_->images.object(key)) {
            const QImage value = *image;
            if (!reserved.lease->unchanged()) return failed(Error::ChangedOnDisk, "Media changed before preview");
            return {{}, value};
        }
    }
    QImage image;
    if (entry.kind == Kind::Png) {
        const auto inspected = inspectPng(entry.path, state_->limits, stop, &image);
        if (inspected.failure) return {inspected.failure, {}};
    } else if (entry.kind == Kind::Mp4) {
        if (!state_->videoCover) return failed(Error::Unsupported, "No video cover reader is connected");
        auto preview = state_->videoCover(entry.path,
            static_cast<unsigned>((std::max)(maximumSize.width(), maximumSize.height())), state_->limits, stop);
        if (preview.failure) return preview;
        image = std::move(preview.image);
    } else {
        std::error_code error;
        auto handle = Platform::openRegularFileReadOnly(nativePath(entry.path), error);
        if (!handle) return failed(Error::Read, "Cannot open the selected GIF preview");
        QFile file;
        if (!file.open(handle.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
            return {{Error::Read, file.errorString()}, {}};
        const auto opened = Platform::readFileDescriptorSnapshot(file.handle(), error);
        if (!opened || *opened != entry.snapshot) return failed(Error::ChangedOnDisk, "GIF changed before preview");
        QImageReader reader(&file, "gif");
        const auto dimensions = reader.size();
        const auto layout = Core::pixelLayout(dimensions.width(), dimensions.height(), 4);
        if (!layout || layout->bytes > state_->limits.maximumImageBytes)
            return failed(Error::Budget, "GIF preview exceeds the decoded-frame budget");
        image = reader.read();
        if (image.isNull() || image.size() != dimensions)
            return {{Error::InvalidMedia, reader.errorString()}, {}};
    }
    if (stop.stop_requested()) return failed(Error::Cancelled);
    if (!reserved.lease->unchanged()) return failed(Error::ChangedOnDisk, "Media changed while decoding its cover");
    if (image.isNull() || image.sizeInBytes() < 0 ||
        static_cast<std::uint64_t>(image.sizeInBytes()) > state_->limits.maximumImageBytes)
        return failed(Error::InvalidMedia, "The preview reader returned no bounded frame");
    if (image.width() > maximumSize.width() || image.height() > maximumSize.height())
        image = image.scaled(maximumSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (image.isNull()) return failed(Error::Budget, "Insufficient thumbnail memory");
    {
        std::lock_guard lock(state_->mutex);
        state_->images.insert(key, new QImage(image), static_cast<qsizetype>((image.sizeInBytes() + 1023) / 1024));
    }
    return {{}, std::move(image)};
}

} // namespace HuhuCapture::Media
