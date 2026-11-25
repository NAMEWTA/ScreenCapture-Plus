#pragma once

#include "services/media/MediaArchiveService.h"
#include "services/transfer/ReceiveSink.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace HuhuCapture::Transfer {

struct ReceiveArchiveLimits {
    static constexpr std::size_t workerCount = 2;
    static constexpr std::size_t resultSlots = 16;
    static constexpr std::size_t maximumFilesPerSession = 64;
    std::size_t maximumSessions{2};
    std::size_t maximumQueuedFiles{8};
    std::uint64_t maximumFileBytes{32ull * 1024 * 1024 * 1024};
    // Leave room for filesystem metadata and unrelated application work. This
    // is admission accounting, not a quota against another process's writes.
    std::uint64_t minimumFreeBytes{64ull * 1024 * 1024};
};

struct ReceiveArchiveStatistics {
    std::size_t sessions{};
    std::size_t queuedFiles{};
    std::size_t activeFiles{};
    std::size_t pendingResults{};
    std::uint64_t reservedBytes{}; // Accepted bytes not yet written or released.
};

// Production file consumer shared by Windows and Linux. Owns two bounded
// workers; no QObject, GUI callbacks, clipboard writes or second archive index.
// The shared archive must supply the real complete GIF/video inspectors.
class ReceiveArchiveSink final : public ReceiveSink {
public:
    explicit ReceiveArchiveSink(std::shared_ptr<Media::MediaArchiveService> archive,
        ReceiveArchiveLimits limits = {});
    ~ReceiveArchiveSink() override;
    ReceiveArchiveSink(const ReceiveArchiveSink&) = delete;
    ReceiveArchiveSink& operator=(const ReceiveArchiveSink&) = delete;

    int reserve(const QString& sessionId, const QJsonArray& acceptedFiles) override;
    int enqueue(ReceiveJob job) override;
    std::vector<ReceiveResult> takeResults() override;
    std::vector<ReceiveProgress> takeProgress() override;
    void cancel(const QString& sessionId) noexcept override;
    void finish(const QString& sessionId) noexcept override;
    void cancelAndJoin() noexcept override;
    ReceiveArchiveStatistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace HuhuCapture::Transfer
