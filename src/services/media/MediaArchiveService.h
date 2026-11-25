#pragma once

#include "platform/FileCommitter.h"
#include "platform/FileIdentity.h"

#include <QImage>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace HuhuCapture::Media {

enum class Kind { Png, Gif, Mp4 };
enum class Error {
    None, Directory, InvalidManagedFile, ChangedOnDisk, Busy, Read, Write,
    InvalidMedia, Unsupported, Budget, Cancelled, Commit, DurabilityUnknown
};

struct Failure {
    Error code{Error::None};
    QString detail;
    explicit operator bool() const noexcept { return code != Error::None; }
};

struct Entry {
    QString path;
    QString name;
    Kind kind{Kind::Png};
    Platform::FileSnapshot snapshot;
    bool copyAsFile() const noexcept { return kind != Kind::Png; }
};

struct Inspection {
    Failure failure;
    Kind kind{Kind::Png};
    QSize dimensions;
    std::uint64_t frames{};
};

struct Limits {
    std::uint64_t maximumFileBytes{32ull * 1024 * 1024 * 1024};
    std::uint64_t maximumImageBytes{512ull * 1024 * 1024};
};

// Synchronous worker API. PNG uses Qt's complete libpng read/read_end path.
// GIF/MP4 are supplied by the shared codec service and MUST decode the entire
// input, including stream completion, with bounded memory and cancellation.
// A missing decoder returns Unsupported; it can never authorize publication.
using FullMediaInspector = std::function<Inspection(const QString&, Kind, std::stop_token)>;
Inspection inspectPng(const QString& path, const Limits& limits, std::stop_token stop = {});

namespace Detail { struct ArchiveState; struct LeaseTicket; }

class MediaLease final {
public:
    ~MediaLease();
    MediaLease(const MediaLease&) = delete;
    MediaLease& operator=(const MediaLease&) = delete;
    const Entry& entry() const noexcept { return entry_; }
    bool removal() const noexcept { return removal_; }
    bool unchanged() const;
private:
    friend class MediaArchiveService;
    MediaLease(Entry entry, bool removal, std::shared_ptr<Detail::LeaseTicket> ticket);
    Entry entry_;
    bool removal_{};
    std::shared_ptr<Detail::LeaseTicket> ticket_;
};

struct LeaseResult {
    Failure failure;
    std::shared_ptr<const MediaLease> lease;
};

// Only createStaging can construct a live staging token. Destruction removes
// only the still-owned temporary inode. Recording failures can explicitly
// retain the unique recovery path instead of losing their only source.
class StagingFile final {
public:
    StagingFile() noexcept;
    ~StagingFile();
    StagingFile(StagingFile&&) noexcept;
    StagingFile& operator=(StagingFile&&) noexcept;
    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;
    const QString& path() const noexcept { return path_; }
    explicit operator bool() const noexcept { return !path_.isEmpty(); }
    // Adopt with QFile::DontCloseHandle and ReadWrite; identity is checked on
    // this nontruncating writer. Close it explicitly before media publication.
    Platform::FileHandle openWriter(std::error_code& error) const;
    QString retainForRecovery() noexcept;
private:
    friend class MediaArchiveService;
    void cleanup() noexcept;
    QString path_;
    Platform::FileIdentity identity_;
    std::shared_ptr<Detail::ArchiveState> owner_;
};

struct StagingResult { Failure failure; StagingFile file; };
struct ListResult { Failure failure; std::vector<Entry> entries; };
struct ArchiveResult {
    Failure failure;
    Platform::CommitState state{Platform::CommitState::NotCommitted};
    // Always preserved after publication, even when durability is unknown.
    QString path;
    Inspection media;
    bool durable() const noexcept { return !failure && state == Platform::CommitState::Committed; }
};

class MediaArchiveService final {
public:
    // Pass the real DocumentsLocation/huhucapture path in production, or an
    // explicit isolated directory in tests. No constructor writes user data.
    explicit MediaArchiveService(QString archiveDirectory,
        FullMediaInspector gifAndVideoInspector = {}, Limits limits = {});
    static QString defaultDirectory();
    QString directory() const;
    ListResult list(std::stop_token stop = {}) const;
    StagingResult createStaging() const;

    // Writers must be closed before calling this. Validation and the final
    // cancellation check precede the existing exclusive durable commit.
    // On failure the caller still owns staging; no unique recording is lost.
    // An optional external cancellation check (e.g. the receive permit) runs
    // on this worker immediately before publication, never on the GUI thread.
    ArchiveResult commitStaged(StagingFile& staging, Kind kind, std::stop_token stop = {},
        const std::function<bool()>& externalCancellation = {}) const;
    ArchiveResult archiveImage(const QImage& image, std::stop_token stop = {}) const;
    ArchiveResult importFile(const QString& source, Kind kind, std::stop_token stop = {}) const;

    // A read lease protects the identity across hard-link aliases in this app.
    // The native send lease additionally holds the actual open OS file object.
    // Hold this shared token through SEND_END, never only through admission.
    LeaseResult lease(const Entry& entry) const;
    // Send preparation calls this on a worker, then retains the returned lease
    // through the native send terminal. A plausible history filename alone is
    // not proof that an externally modified file is complete media.
    LeaseResult validatedLease(const Entry& entry, std::stop_token stop = {}) const;
    // Call only after the UI's actual clear confirmation. The caller owns this
    // exclusive reservation through the platform recycle operation. It must
    // recheck unchanged() immediately before using the path, and must never
    // silently fall back to permanent deletion when recycling fails.
    LeaseResult reserveRemoval(const Entry& entry) const;
private:
    LeaseResult reserve(const Entry& entry, bool removal) const;
    std::shared_ptr<Detail::ArchiveState> state_;
    FullMediaInspector inspectOther_;
    Limits limits_;
};

} // namespace HuhuCapture::Media
