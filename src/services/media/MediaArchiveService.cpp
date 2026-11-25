#include "services/media/MediaArchiveService.h"

#include "core/MediaFileName.h"
#include "core/PixelBuffer.h"
#include "services/files/QtPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPixelFormat>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <new>
#include <utility>

namespace HuhuCapture::Media {
namespace Detail {
struct Usage { std::size_t readers{}; bool removing{}; };
struct ArchiveState {
    QString directory;
    std::mutex mutex;
    std::map<Platform::FileIdentity, Usage> usage;
};
struct LeaseTicket {
    std::shared_ptr<ArchiveState> owner;
    Platform::FileIdentity identity;
    bool removal{};
    ~LeaseTicket()
    {
        if (!owner) return;
        std::lock_guard lock(owner->mutex);
        const auto found = owner->usage.find(identity);
        if (found == owner->usage.end()) return;
        if (removal) found->second.removing = false;
        else if (found->second.readers) --found->second.readers;
        if (!found->second.readers && !found->second.removing) owner->usage.erase(found);
    }
};
} // namespace Detail
namespace {
namespace fs = std::filesystem;
using Files::nativePath;
using Files::qtPath;

Failure systemFailure(Error error, const std::error_code& detail)
{
    return {error, QString::fromStdString(detail.message())};
}

std::wstring_view extension(Kind kind)
{
    switch (kind) {
    case Kind::Png: return L"png";
    case Kind::Gif: return L"gif";
    case Kind::Mp4: return L"mp4";
    }
    return {};
}

Kind kindFor(const Core::MediaFileName& name)
{
    return name.extension == L"png" ? Kind::Png : name.extension == L"gif" ? Kind::Gif : Kind::Mp4;
}

fs::path resolvedRoot(const Detail::ArchiveState& state, bool create, std::error_code& error)
{
    error.clear();
    if (state.directory.isEmpty() || state.directory.contains(QChar(u'\0'))) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    const auto root = nativePath(state.directory);
    if (create) {
        fs::create_directories(root, error);
        if (error) return {};
    }
    // Trusted Known Folder / XDG redirection can intentionally traverse links.
    const auto resolved = fs::canonical(root, error);
    if (error) return {};
    if (!fs::is_directory(resolved, error) && !error)
        error = std::make_error_code(std::errc::not_a_directory);
    return error ? fs::path{} : resolved;
}

bool sameOwnedFile(const StagingFile& file, Platform::FileIdentity identity)
{
    std::error_code error;
    const auto current = Platform::readFileSnapshot(nativePath(file.path()), error);
    return current && current->identity == identity;
}

ArchiveResult failed(Failure failure)
{
    ArchiveResult result;
    result.failure = std::move(failure);
    return result;
}

Failure cancelled() { return {Error::Cancelled, {}}; }

bool writeAll(QFile& destination, const char* bytes, qint64 length)
{
    for (qint64 offset = 0; offset < length;) {
        const auto count = destination.write(bytes + offset, length - offset);
        if (count <= 0) return false;
        offset += count;
    }
    return true;
}
} // namespace

Inspection inspectPng(const QString& path, const Limits& limits, std::stop_token stop)
{
    Inspection result;
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.failure = {Error::Read, file.errorString()}; return result;
    }
    if (file.size() <= 0 || static_cast<std::uint64_t>(file.size()) > limits.maximumFileBytes) {
        result.failure = {Error::Budget, QStringLiteral("Media file exceeds the byte budget")}; return result;
    }
    // The pinned Qt iod_read_fn synthesizes missing IEND CRC bytes for a
    // seekable device. Keep its mature decoder, but require the actual complete
    // zero-length IEND chunk before it can apply that compatibility behavior.
    // This is a physical completion check, not another PNG chunk parser.
    constexpr std::array<char, 12> endChunk{
        0, 0, 0, 0, 'I', 'E', 'N', 'D', char(0xae), char(0x42), char(0x60), char(0x82)};
    if (file.size() < static_cast<qint64>(endChunk.size()) ||
        !file.seek(file.size() - static_cast<qint64>(endChunk.size())) ||
        file.read(static_cast<qint64>(endChunk.size())) != QByteArray(endChunk.data(), endChunk.size()) ||
        !file.seek(0)) {
        result.failure = {Error::InvalidMedia, QStringLiteral("PNG has no complete physical IEND chunk")}; return result;
    }
    QImageReader reader(&file, "png");
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    const auto size = reader.size();
    const auto bits = QImage::toPixelFormat(reader.imageFormat()).bitsPerPixel();
    const auto layout = Core::pixelLayout(size.width(), size.height(), bits ? (bits + 7u) / 8u : 8u);
    if (!layout || layout->bytes > limits.maximumImageBytes) {
        result.failure = {Error::Budget, QStringLiteral("Decoded image exceeds the pixel budget")}; return result;
    }
    // Qt then calls png_read_image AND png_read_end; short body reads still
    // reach libpng's error path. The physical IEND check above closes Qt's
    // documented-in-source truncated-final-CRC compatibility exception.
    const auto image = reader.read();
    if (image.isNull() || image.size() != size || reader.error() != QImageReader::UnknownError) {
        result.failure = {Error::InvalidMedia, reader.errorString()}; return result;
    }
    if (!file.atEnd()) {
        result.failure = {Error::InvalidMedia, QStringLiteral("Unexpected data after the complete PNG")}; return result;
    }
    if (image.sizeInBytes() < 0 || static_cast<std::uint64_t>(image.sizeInBytes()) > limits.maximumImageBytes) {
        result.failure = {Error::Budget, QStringLiteral("Decoded image exceeds the pixel budget")}; return result;
    }
    if (file.error() != QFileDevice::NoError) {
        result.failure = {Error::Read, file.errorString()}; return result;
    }
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    result.dimensions = size;
    result.frames = 1;
    return result;
}

MediaLease::MediaLease(Entry entry, bool removal, std::shared_ptr<Detail::LeaseTicket> ticket)
    : entry_(std::move(entry)), removal_(removal), ticket_(std::move(ticket)) {}
MediaLease::~MediaLease() = default;
bool MediaLease::unchanged() const
{
    std::error_code error;
    const auto current = Platform::readFileSnapshot(nativePath(entry_.path), error);
    return current && *current == entry_.snapshot;
}

StagingFile::StagingFile() noexcept = default;
StagingFile::~StagingFile() { cleanup(); }
StagingFile::StagingFile(StagingFile&& other) noexcept
    : path_(std::exchange(other.path_, {})), identity_(other.identity_), owner_(std::move(other.owner_)) {}
StagingFile& StagingFile::operator=(StagingFile&& other) noexcept
{
    if (this != &other) {
        cleanup(); path_ = std::exchange(other.path_, {}); identity_ = other.identity_;
        owner_ = std::move(other.owner_);
    }
    return *this;
}
void StagingFile::cleanup() noexcept
{
    try {
        if (owner_ && !path_.isEmpty() && sameOwnedFile(*this, identity_)) {
            std::error_code ignored;
            fs::remove(nativePath(path_), ignored);
        }
    } catch (...) { /* A failed cleanup retains its private temporary file. */ }
    path_.clear(); owner_.reset();
}
QString StagingFile::retainForRecovery() noexcept
{
    owner_.reset(); return std::exchange(path_, {});
}
Platform::FileHandle StagingFile::openWriter(std::error_code& error) const
{
    if (!owner_ || path_.isEmpty()) {
        error = std::make_error_code(std::errc::invalid_argument); return {};
    }
    return Platform::openOwnedEmptyFileForWrite(nativePath(path_), identity_, error);
}

MediaArchiveService::MediaArchiveService(QString directory, FullMediaInspector inspector, Limits limits)
    : state_(std::make_shared<Detail::ArchiveState>()), inspectOther_(std::move(inspector)), limits_(limits)
{
    if (!directory.isEmpty() && !directory.contains(QChar(u'\0')))
        state_->directory = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
}

QString MediaArchiveService::defaultDirectory()
{
    const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return documents.isEmpty() ? QString{} : QDir(documents).filePath(QStringLiteral("huhucapture"));
}
QString MediaArchiveService::directory() const { return state_->directory; }

ListResult MediaArchiveService::list(std::stop_token stop) const
{
    ListResult result;
    if (stop.stop_requested()) { result.failure = cancelled(); return result; }
    std::error_code error;
    const auto root = resolvedRoot(*state_, false, error);
    if (error == std::errc::no_such_file_or_directory) return result;
    if (error) { result.failure = systemFailure(Error::Directory, error); return result; }
    fs::directory_iterator item(root, error), end;
    for (; !error && item != end; item.increment(error)) {
        if (stop.stop_requested()) { result.failure = cancelled(); result.entries.clear(); return result; }
        const auto name = qtPath(item->path().filename());
        const auto parsed = Core::parseMediaFileName(name.toStdWString());
        if (!parsed) continue;
        std::error_code itemError;
        const auto snapshot = Platform::readFileSnapshot(item->path(), itemError);
        if (!snapshot || !snapshot->bytes) continue;
        result.entries.push_back({qtPath(item->path()), name, kindFor(*parsed), *snapshot});
    }
    if (error) { result.failure = systemFailure(Error::Read, error); result.entries.clear(); return result; }
    std::sort(result.entries.begin(), result.entries.end(), [](const Entry& a, const Entry& b) {
        return Core::newerMediaFileName(a.name.toStdWString(), b.name.toStdWString());
    });
    return result;
}

StagingResult MediaArchiveService::createStaging() const
{
    StagingResult result;
    std::error_code error;
    const auto root = resolvedRoot(*state_, true, error);
    if (error) { result.failure = systemFailure(Error::Directory, error); return result; }
    const auto path = Platform::createExclusiveTemporaryFile(root, error);
    if (error) { result.failure = systemFailure(Error::Write, error); return result; }
    const auto snapshot = Platform::readFileSnapshot(path, error);
    if (!snapshot) {
        // Identity was never established; do not blindly unlink this name.
        result.failure = systemFailure(Error::Read, error); return result;
    }
    result.file.path_ = qtPath(path);
    result.file.identity_ = snapshot->identity;
    result.file.owner_ = state_;
    return result;
}

ArchiveResult MediaArchiveService::commitStaged(StagingFile& staging, Kind kind, std::stop_token stop,
    const std::function<bool()>& externalCancellation) const
{
    if (staging.owner_ != state_ || !staging || extension(kind).empty())
        return failed({Error::InvalidManagedFile, QStringLiteral("Invalid staging owner or media kind")});
    if (stop.stop_requested()) return failed(cancelled());
    std::error_code error;
    const auto path = nativePath(staging.path_);
    const auto original = Platform::readFileSnapshot(path, error);
    if (!original || original->identity != staging.identity_)
        return failed({Error::ChangedOnDisk, QStringLiteral("Staging file identity changed")});
    if (!original->bytes || original->bytes > limits_.maximumFileBytes)
        return failed({Error::Budget, QStringLiteral("Media file exceeds the byte budget")});
    ArchiveResult result;
    try {
        if (kind == Kind::Png) result.media = inspectPng(staging.path_, limits_, stop);
        else if (inspectOther_) result.media = inspectOther_(staging.path_, kind, stop);
        else return failed({Error::Unsupported, QStringLiteral("No complete decoder is available for this media")});
    } catch (const std::bad_alloc&) { return failed({Error::Budget, QStringLiteral("Insufficient decoder memory")}); }
    catch (...) { return failed({Error::InvalidMedia, QStringLiteral("The complete media decoder failed")}); }
    if (result.media.failure) { result.failure = result.media.failure; return result; }
    if (result.media.kind != kind || result.media.dimensions.isEmpty() || !result.media.frames)
        return failed({Error::InvalidMedia, QStringLiteral("Decoder did not validate a complete media stream")});
    const auto after = Platform::readFileSnapshot(path, error);
    if (!after || *after != *original)
        return failed({Error::ChangedOnDisk, QStringLiteral("Staging file changed during validation")});
    if (stop.stop_requested()) return failed(cancelled());
    const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")).toStdWString();
    for (std::uint32_t sequence = 0; sequence < 100000; ++sequence) {
        const auto name = Core::makeMediaFileName(stamp, extension(kind), sequence);
        if (name.empty()) return failed({Error::Commit, QStringLiteral("Invalid completion timestamp")});
        if (stop.stop_requested()) return failed(cancelled());
        try {
            if (externalCancellation && externalCancellation()) return failed(cancelled());
        } catch (...) {
            return failed({Error::Commit, QStringLiteral("Could not verify the current publication permit")});
        }
        const auto committed = Platform::commitFileNoReplace(path, path.parent_path() / name);
        if (!committed.committed()) {
            if (committed.error == std::errc::file_exists) continue;
            result.failure = systemFailure(Error::Commit, committed.error); return result;
        }
        result.state = committed.state;
        result.path = qtPath(committed.destination);
        // Never retry or remove a published name after any later error.
        if (!committed.stagingRetained) { staging.path_.clear(); staging.owner_.reset(); }
        if (!committed.durable())
            result.failure = systemFailure(Error::DurabilityUnknown, committed.error);
        return result;
    }
    return failed({Error::Commit, QStringLiteral("No unused archive name remains for this second")});
}

ArchiveResult MediaArchiveService::archiveImage(const QImage& image, std::stop_token stop) const
{
    if (image.isNull()) return failed({Error::InvalidMedia, QStringLiteral("No completed image")});
    if (image.sizeInBytes() < 0 || static_cast<std::uint64_t>(image.sizeInBytes()) > limits_.maximumImageBytes)
        return failed({Error::Budget, QStringLiteral("Image exceeds the pixel budget")});
    if (stop.stop_requested()) return failed(cancelled());
    auto staging = createStaging();
    if (staging.failure) return failed(std::move(staging.failure));
    std::error_code error;
    auto writerHandle = staging.file.openWriter(error);
    if (!writerHandle) return failed(systemFailure(Error::Write, error));
    QFile file;
    if (!file.open(writerHandle.get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle))
        return failed({Error::Write, file.errorString()});
    {
        QImageWriter writer(&file, "png");
        if (!writer.write(image)) return failed({Error::Write, writer.errorString()});
    }
    if (!file.flush()) return failed({Error::Write, file.errorString()});
    file.close();
    if (file.error() != QFileDevice::NoError) return failed({Error::Write, file.errorString()});
    if (!writerHandle.close(error)) return failed(systemFailure(Error::Write, error));
    return commitStaged(staging.file, Kind::Png, stop);
}

ArchiveResult MediaArchiveService::importFile(const QString& source, Kind kind, std::stop_token stop) const
{
    if (source.isEmpty() || source.contains(QChar(u'\0'))) return failed({Error::Read, QStringLiteral("Invalid media source")});
    if (stop.stop_requested()) return failed(cancelled());
    std::error_code error;
    auto sourceHandle = Platform::openRegularFileReadOnly(nativePath(source), error);
    if (!sourceHandle) return failed(systemFailure(Error::Read, error));
    QFile input;
    if (!input.open(sourceHandle.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return failed({Error::Read, input.errorString()});
    const auto opened = Platform::readFileDescriptorSnapshot(input.handle(), error);
    const auto named = Platform::readFileSnapshot(nativePath(source), error);
    if (!opened || !named || *opened != *named)
        return failed({Error::ChangedOnDisk, QStringLiteral("Source is not the selected ordinary file")});
    if (!opened->bytes || opened->bytes > limits_.maximumFileBytes)
        return failed({Error::Budget, QStringLiteral("Source exceeds the media byte budget")});
    auto staging = createStaging();
    if (staging.failure) return failed(std::move(staging.failure));
    auto writerHandle = staging.file.openWriter(error);
    if (!writerHandle) return failed(systemFailure(Error::Write, error));
    QFile output;
    if (!output.open(writerHandle.get(), QIODevice::ReadWrite, QFileDevice::DontCloseHandle))
        return failed({Error::Write, output.errorString()});
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t total{};
    for (;;) {
        if (stop.stop_requested()) return failed(cancelled());
        const auto count = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (count < 0) return failed({Error::Read, input.errorString()});
        if (!count) break;
        if (static_cast<std::uint64_t>(count) > opened->bytes - total)
            return failed({Error::ChangedOnDisk, QStringLiteral("Source grew during import")});
        if (!writeAll(output, buffer.data(), count)) return failed({Error::Write, output.errorString()});
        total += static_cast<std::uint64_t>(count);
    }
    const auto after = Platform::readFileDescriptorSnapshot(input.handle(), error);
    if (total != opened->bytes || !after || *after != *opened)
        return failed({Error::ChangedOnDisk, QStringLiteral("Source changed during import")});
    if (!output.flush()) return failed({Error::Write, output.errorString()});
    output.close(); input.close();
    if (output.error() != QFileDevice::NoError) return failed({Error::Write, output.errorString()});
    if (!writerHandle.close(error)) return failed(systemFailure(Error::Write, error));
    return commitStaged(staging.file, kind, stop);
}

LeaseResult MediaArchiveService::validatedLease(const Entry& entry, std::stop_token stop) const
{
    if (stop.stop_requested()) return {cancelled(), {}};
    auto reserved = lease(entry);
    if (reserved.failure) return reserved;
    Inspection inspection;
    try {
        if (entry.kind == Kind::Png) inspection = inspectPng(entry.path, limits_, stop);
        else if (inspectOther_) inspection = inspectOther_(entry.path, entry.kind, stop);
        else return {{Error::Unsupported, QStringLiteral("No complete decoder is available for this media")}, {}};
    } catch (const std::bad_alloc&) { return {{Error::Budget, QStringLiteral("Insufficient decoder memory")}, {}}; }
    catch (...) { return {{Error::InvalidMedia, QStringLiteral("The complete media decoder failed")}, {}}; }
    if (inspection.failure) return {inspection.failure, {}};
    if (inspection.kind != entry.kind || inspection.dimensions.isEmpty() || !inspection.frames)
        return {{Error::InvalidMedia, QStringLiteral("Decoder did not validate a complete media stream")}, {}};
    if (stop.stop_requested()) return {cancelled(), {}};
    if (!reserved.lease->unchanged())
        return {{Error::ChangedOnDisk, QStringLiteral("The selected media changed during validation")}, {}};
    return reserved;
}

LeaseResult MediaArchiveService::reserve(const Entry& entry, bool removal) const
{
    const auto parsed = Core::parseMediaFileName(entry.name.toStdWString());
    if (!parsed || kindFor(*parsed) != entry.kind || entry.path.isEmpty() || entry.path.contains(QChar(u'\0')))
        return {{Error::InvalidManagedFile, QStringLiteral("Not a managed media entry")}, {}};
    std::error_code error;
    const auto root = resolvedRoot(*state_, false, error);
    const auto path = nativePath(entry.path);
    if (error || path.parent_path() != root || qtPath(path.filename()) != entry.name)
        return {{Error::InvalidManagedFile, QStringLiteral("Media is outside the archive root")}, {}};
    std::lock_guard lock(state_->mutex);
    const auto current = Platform::readFileSnapshot(path, error);
    if (!current || *current != entry.snapshot || !current->bytes)
        return {{Error::ChangedOnDisk, QStringLiteral("Media changed since the history snapshot")}, {}};
    const auto found = state_->usage.find(current->identity);
    if (found != state_->usage.end() && (found->second.removing || (removal && found->second.readers)))
        return {{Error::Busy, QStringLiteral("Media is in use")}, {}};
    // Allocate before marking the reservation so allocation failure cannot
    // leave a permanent busy entry. The ticket stays unarmed until admitted.
    auto ticket = std::make_shared<Detail::LeaseTicket>();
    auto lease = std::shared_ptr<const MediaLease>(new MediaLease(entry, removal, ticket));
    auto& usage = state_->usage[current->identity];
    if (removal) usage.removing = true;
    else ++usage.readers;
    ticket->identity = current->identity;
    ticket->removal = removal;
    ticket->owner = state_;
    return {{}, std::move(lease)};
}
LeaseResult MediaArchiveService::lease(const Entry& entry) const { return reserve(entry, false); }
LeaseResult MediaArchiveService::reserveRemoval(const Entry& entry) const { return reserve(entry, true); }

} // namespace HuhuCapture::Media
