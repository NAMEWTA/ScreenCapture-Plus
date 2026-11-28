#include "services/transfer/ReceiveArchiveSink.h"

#include <QFile>
#include <QStorageInfo>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace HuhuCapture::Transfer {
namespace {
constexpr auto protocolMaximumFileBytes = 32ull * 1024 * 1024 * 1024;
constexpr std::uint32_t readWaitMilliseconds = 100;

std::optional<Media::Kind> mediaKind(const QString& mime)
{
    if (mime == QStringLiteral("image/png")) return Media::Kind::Png;
    if (mime == QStringLiteral("image/gif")) return Media::Kind::Gif;
    if (mime == QStringLiteral("video/mp4")) return Media::Kind::Mp4;
    return {};
}

std::uint32_t nativeKind(Media::Kind kind) noexcept
{
    switch (kind) {
    case Media::Kind::Png: return HUHU_PNG;
    case Media::Kind::Gif: return HUHU_GIF;
    case Media::Kind::Mp4: return HUHU_MP4;
    }
    return 0;
}

int statusFor(Media::Error error) noexcept
{
    switch (error) {
    case Media::Error::None: return HUHU_OK;
    case Media::Error::Cancelled: return HUHU_CANCELLED;
    case Media::Error::Budget: return HUHU_LIMIT_EXCEEDED;
    case Media::Error::InvalidMedia:
    case Media::Error::Unsupported: return HUHU_INVALID_MEDIA;
    case Media::Error::DurabilityUnknown: return HUHU_DURABILITY_UNKNOWN;
    default: return HUHU_IO;
    }
}

bool identifier(const QString& text)
{
    return !text.isEmpty() && !text.contains(QChar(u'\0')) && text.toUtf8().size() <= 1024;
}

// Completion failures have a narrower native contract than command errors.
void reject(ReceiveJob& job, int status) noexcept
{
    if (!job.receive) return;
    const auto completion = status == HUHU_LIMIT_EXCEEDED || status == HUHU_CANCELLED ||
        status == HUHU_INVALID_MEDIA ? status : HUHU_IO;
    (void)huhu_receive_complete(job.receive.get(), completion, 0);
    job.receive.reset();
}
} // namespace

struct ReceiveArchiveSink::Impl {
    struct FilePlan {
        QJsonObject metadata;
        std::uint64_t size{};
        std::uint64_t unstartedBytes{};
        std::uint64_t latestAttempt{};
        Media::Kind kind{Media::Kind::Png};
        bool published{};
    };
    struct Session {
        QString id;
        std::map<QString, FilePlan> files;
        std::stop_source stop;
        std::size_t jobs{};
        bool closed{};
    };
    enum class SlotState { Free, Pending, Ready };
    struct OutcomeSlot {
        SlotState state{SlotState::Free};
        ReceiveResult result;
        std::uint64_t written{};
        std::uint64_t total{};
        bool progressChanged{};
    };
    struct Task {
        ReceiveJob job;
        std::shared_ptr<Session> session;
        std::size_t slot{};
        std::uint64_t size{};
        std::uint64_t remaining{}; // Accessed only under mutex after admission.
        Media::Kind kind{Media::Kind::Png};
    };

    std::shared_ptr<Media::MediaArchiveService> archive;
    ReceiveArchiveLimits limits;
    mutable std::mutex mutex;
    std::condition_variable_any work;
    std::map<QString, std::shared_ptr<Session>> sessions;
    std::deque<Task> queue;
    std::array<OutcomeSlot, ReceiveArchiveLimits::resultSlots> outcomes;
    std::uint64_t reservedBytes{};
    std::size_t activeFiles{};
    bool closed{};
    // Destroy workers before their mutex/queue, including partial construction.
    std::vector<std::jthread> workers;

    Impl(std::shared_ptr<Media::MediaArchiveService> value, ReceiveArchiveLimits policy)
        : archive(std::move(value)), limits(policy)
    {
        if (!archive || archive->directory().isEmpty() || !limits.maximumSessions ||
            limits.maximumSessions > 8 || !limits.maximumQueuedFiles || limits.maximumQueuedFiles > 8 ||
            !limits.maximumFileBytes || limits.maximumFileBytes > protocolMaximumFileBytes)
            throw std::invalid_argument("Invalid receive archive or bounded worker policy");
        workers.reserve(ReceiveArchiveLimits::workerCount);
        for (std::size_t index = 0; index < ReceiveArchiveLimits::workerCount; ++index)
            workers.emplace_back([this](std::stop_token stop) { run(stop); });
    }

    // Caller holds mutex; bytesAvailable already accounts for bytes written to
    // staging. Only future bytes remain reserved, avoiding double charging an
    // active transfer while preventing concurrent accepts from oversubscribing.
    int spaceFor(std::uint64_t additional) const
    {
        QStorageInfo storage(archive->directory());
        storage.refresh();
        if (!storage.isValid() || !storage.isReady() || storage.isReadOnly() || storage.bytesAvailable() < 0)
            return HUHU_IO;
        const auto available = static_cast<std::uint64_t>(storage.bytesAvailable());
        if (available < limits.minimumFreeBytes) return HUHU_IO;
        const auto usable = available - limits.minimumFreeBytes;
        if (reservedBytes > usable || additional > usable - reservedBytes) return HUHU_IO;
        return HUHU_OK;
    }

    int reserve(const QString& id, const QJsonArray& accepted)
    {
        if (!identifier(id) || accepted.isEmpty()) return HUHU_INVALID_ARGUMENT;
        if (accepted.size() > static_cast<qsizetype>(ReceiveArchiveLimits::maximumFilesPerSession))
            return HUHU_LIMIT_EXCEEDED;
        auto session = std::make_shared<Session>();
        session->id = id;
        std::uint64_t total{};
        for (const auto& value : accepted) {
            if (!value.isObject()) return HUHU_INVALID_ARGUMENT;
            auto file = value.toObject();
            const auto fileId = file.value(QStringLiteral("id")).toString();
            const auto kind = mediaKind(file.value(QStringLiteral("fileType")).toString());
            const auto sizeValue = file.value(QStringLiteral("size"));
            const auto size = sizeValue.toDouble(-1);
            if (!identifier(fileId) || !kind || !sizeValue.isDouble() || !std::isfinite(size) ||
                size <= 0 || std::floor(size) != size) return HUHU_INVALID_ARGUMENT;
            if (size > static_cast<double>(limits.maximumFileBytes)) return HUHU_LIMIT_EXCEEDED;
            const auto bytes = static_cast<std::uint64_t>(size);
            if (bytes > std::numeric_limits<std::uint64_t>::max() - total) return HUHU_LIMIT_EXCEEDED;
            if (!session->files.emplace(fileId, FilePlan{std::move(file), bytes, bytes, 0, *kind, false}).second)
                return HUHU_INVALID_ARGUMENT;
            total += bytes;
        }
        // An actual exclusive empty file proves the configured archive is
        // usable before acceptance. Its owned token removes only that file.
        auto writable = archive->createStaging();
        if (writable.failure) return statusFor(writable.failure.code);
        std::lock_guard lock(mutex);
        if (closed) return HUHU_CANCELLED;
        if (sessions.contains(id)) return HUHU_INVALID_STATE;
        if (sessions.size() >= limits.maximumSessions) return HUHU_LIMIT_EXCEEDED;
        if (const auto status = spaceFor(total); status != HUHU_OK) return status;
        sessions.emplace(id, std::move(session)); // May throw before charging.
        reservedBytes += total;
        return HUHU_OK;
    }

    int enqueue(ReceiveJob job)
    {
        if (!job.receive || !job.attempt) { reject(job, HUHU_INVALID_ARGUMENT); return HUHU_INVALID_ARGUMENT; }
        std::unique_lock lock(mutex);
        auto fail = [&](int status) {
            lock.unlock(); reject(job, status); return status;
        };
        if (closed) return fail(HUHU_CANCELLED);
        const auto found = sessions.find(job.sessionId);
        if (found == sessions.end() || found->second->closed) return fail(HUHU_CANCELLED);
        auto session = found->second;
        auto file = session->files.find(job.fileId);
        if (file == session->files.end() || file->second.metadata != job.file ||
            job.attempt <= file->second.latestAttempt) return fail(HUHU_PROTOCOL);
        auto& plan = file->second;
        if (plan.published) return fail(HUHU_INVALID_STATE);
        auto slot = std::find_if(outcomes.begin(), outcomes.end(), [](const OutcomeSlot& item) {
            return item.state == SlotState::Free;
        });
        if (queue.size() >= limits.maximumQueuedFiles || slot == outcomes.end()) return fail(HUHU_LIMIT_EXCEEDED);
        // A real checksum retry can arrive while the old consumer is releasing
        // its failed attempt. Give it separate accounting and a separate result
        // slot; a late old outcome can never replace the new attempt's result.
        const auto extra = plan.unstartedBytes ? std::uint64_t{} : plan.size;
        if (extra) if (const auto status = spaceFor(extra); status != HUHU_OK) return fail(status);
        ReceiveResult result{job.sessionId, job.fileId, job.attempt, {}, HUHU_INVALID_STATE, false};
        const auto attempt = job.attempt;
        const auto index = static_cast<std::size_t>(std::distance(outcomes.begin(), slot));
        queue.push_back(Task{std::move(job), session, index, plan.size, plan.size, plan.kind});
        // All allocations preceded the following nonthrowing admission state.
        slot->result = std::move(result);
        slot->state = SlotState::Pending;
        slot->written = 0;
        slot->total = plan.size;
        slot->progressChanged = true;
        plan.unstartedBytes = 0;
        plan.latestAttempt = attempt;
        reservedBytes += extra;
        ++session->jobs;
        lock.unlock();
        work.notify_one();
        return HUHU_OK;
    }

    void releaseUnstarted(Session& session) noexcept
    {
        for (auto& [id, file] : session.files) {
            (void)id;
            reservedBytes -= file.unstartedBytes;
            file.unstartedBytes = 0;
        }
    }

    void closeSession(const QString& id, bool cancel) noexcept
    {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            const auto found = sessions.find(id);
            if (found == sessions.end()) return;
            session = found->second;
            session->closed = true;
            releaseUnstarted(*session);
            if (!session->jobs) sessions.erase(found);
        }
        // Stop callbacks must never run under the accounting mutex.
        if (cancel) session->stop.request_stop();
        work.notify_all();
    }

    bool cancelled(const Task& task) const
    {
        return task.session->stop.stop_requested() || huhu_receive_is_cancelled(task.job.receive.get()) != 0;
    }

    int consume(Task& task, ReceiveResult& outcome)
    {
        if (cancelled(task)) return HUHU_CANCELLED;
        auto temporary = archive->createStaging();
        if (temporary.failure) return statusFor(temporary.failure.code);
        std::error_code error;
        auto writerHandle = temporary.file.openWriter(error);
        if (!writerHandle) return HUHU_IO;
        QFile file;
        if (!file.open(writerHandle.get(), QIODevice::ReadWrite | QIODevice::Unbuffered,
            QFileDevice::DontCloseHandle)) return HUHU_IO;
        std::array<std::uint8_t, 64 * 1024> bytes{};
        std::uint64_t written{};
        for (;;) {
            if (cancelled(task)) return HUHU_CANCELLED;
            std::size_t count{};
            const auto status = huhu_receive_read(task.job.receive.get(), bytes.data(), bytes.size(),
                &count, readWaitMilliseconds);
            if (status == HUHU_TIMEOUT) continue;
            if (status == HUHU_END) break;
            if (status != HUHU_OK) return status;
            if (!count || count > bytes.size()) return HUHU_IO;
            // Test the remaining allowance before the first overflowing block
            // touches disk, even though the controlled core also checks length.
            if (count > task.size - written) return HUHU_LENGTH_MISMATCH;
            for (std::size_t offset = 0; offset < count;) {
                const auto amount = file.write(reinterpret_cast<const char*>(bytes.data() + offset),
                    static_cast<qint64>(count - offset));
                if (amount <= 0) return HUHU_IO;
                offset += static_cast<std::size_t>(amount);
                written += static_cast<std::uint64_t>(amount);
                std::lock_guard lock(mutex);
                task.remaining -= static_cast<std::uint64_t>(amount);
                reservedBytes -= static_cast<std::uint64_t>(amount);
                outcomes[task.slot].written = written;
                outcomes[task.slot].progressChanged = true;
            }
        }
        if (written != task.size) return HUHU_LENGTH_MISMATCH;
        HuhuVerification verified{};
        for (;;) {
            if (cancelled(task)) return HUHU_CANCELLED;
            const auto status = huhu_receive_verify(task.job.receive.get(), readWaitMilliseconds, &verified);
            if (status == HUHU_TIMEOUT) continue;
            if (status != HUHU_OK) return status;
            break;
        }
        if (verified.received_bytes != written) return HUHU_LENGTH_MISMATCH;
        if (verified.media_kind != nativeKind(task.kind)) return HUHU_INVALID_MEDIA;
        if (task.job.file.contains(QStringLiteral("sha256")) &&
            !task.job.file.value(QStringLiteral("sha256")).isNull() && !verified.sender_checksum_matched)
            return HUHU_CHECKSUM_MISMATCH;
        if (!file.flush()) return HUHU_IO;
        file.close();
        if (file.error() != QFileDevice::NoError) return HUHU_IO;
        if (!writerHandle.close(error)) return HUHU_IO;
        if (cancelled(task)) return HUHU_CANCELLED;
        auto committed = archive->commitStaged(temporary.file, task.kind, task.session->stop.get_token(),
            [&] { return cancelled(task); });
        outcome.path = std::move(committed.path);
        outcome.published = committed.state != Platform::CommitState::NotCommitted;
        if (committed.failure) return statusFor(committed.failure.code);
        if (!committed.durable()) return HUHU_DURABILITY_UNKNOWN;
        return HUHU_OK;
    }

    void process(Task& task) noexcept
    {
        // IDs and the slot already exist. Publication into a reserved slot uses
        // only moves and never blocks waiting for the actor to drain results.
        auto& outcome = outcomes[task.slot].result;
        int status;
        try { status = consume(task, outcome); }
        catch (const std::bad_alloc&) { status = HUHU_LIMIT_EXCEEDED; }
        catch (...) { status = HUHU_IO; }
        if (status > 0 || status == HUHU_INVALID_STATE || status == HUHU_PROTOCOL ||
            status == HUHU_CLOSED || status == HUHU_PANIC || status == HUHU_INVALID_ARGUMENT)
            status = HUHU_IO;
        if (outcome.published) {
            std::lock_guard lock(mutex);
            // Set the no-replay barrier BEFORE waking the HTTP completion.
            // A peer retry after a durability error must not race this worker's
            // later cleanup and publish a second file in the same session.
            task.session->files.at(task.job.fileId).published = true;
        }
        const auto acknowledged = huhu_receive_complete(task.job.receive.get(), status, nativeKind(task.kind));
        if (status == HUHU_OK && acknowledged != HUHU_OK) {
            // Late cancellation or a lost receipt cannot undo a durable local
            // publication. Keep the actual path and expose the uncertain result.
            status = acknowledged;
            if (acknowledged == HUHU_CANCELLED)
                (void)huhu_receive_complete(task.job.receive.get(), HUHU_CANCELLED, 0);
        }
        task.job.receive.reset();
        std::lock_guard lock(mutex);
        reservedBytes -= task.remaining;
        task.remaining = 0;
        --activeFiles;
        --task.session->jobs;
        outcome.status = status;
        outcomes[task.slot].state = SlotState::Ready;
        if (task.session->closed && !task.session->jobs) {
            const auto found = sessions.find(task.session->id);
            if (found != sessions.end() && found->second == task.session) sessions.erase(found);
        }
    }

    void run(std::stop_token stop) noexcept
    {
        for (;;) {
            std::optional<Task> task;
            {
                std::unique_lock lock(mutex);
                work.wait(lock, stop, [&] { return closed || !queue.empty(); });
                if (queue.empty()) {
                    if (closed || stop.stop_requested()) return;
                    continue;
                }
                task.emplace(std::move(queue.front()));
                queue.pop_front();
                ++activeFiles;
            }
            // Closing still consumes queued ownership to produce every promised
            // outcome. Each session's stop was requested before worker joining.
            process(*task);
        }
    }

    void join() noexcept
    {
        std::array<std::shared_ptr<Session>, 8> stopping;
        std::size_t count{};
        {
            std::lock_guard lock(mutex);
            closed = true;
            for (auto& [id, session] : sessions) {
                (void)id;
                session->closed = true;
                releaseUnstarted(*session);
                stopping[count++] = session;
            }
        }
        for (std::size_t index = 0; index < count; ++index) stopping[index]->stop.request_stop();
        for (auto& worker : workers) worker.request_stop();
        work.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        sessions.clear();
    }
};

ReceiveArchiveSink::ReceiveArchiveSink(std::shared_ptr<Media::MediaArchiveService> archive,
    ReceiveArchiveLimits limits) : impl_(std::make_unique<Impl>(std::move(archive), limits)) {}
ReceiveArchiveSink::~ReceiveArchiveSink() { cancelAndJoin(); }

int ReceiveArchiveSink::reserve(const QString& id, const QJsonArray& files)
{
    try { return impl_->reserve(id, files); }
    catch (const std::bad_alloc&) { return HUHU_LIMIT_EXCEEDED; }
    catch (...) { return HUHU_IO; }
}
int ReceiveArchiveSink::enqueue(ReceiveJob job)
{
    try { return impl_->enqueue(std::move(job)); }
    catch (const std::bad_alloc&) { return HUHU_LIMIT_EXCEEDED; }
    catch (...) { return HUHU_IO; }
}
std::vector<ReceiveResult> ReceiveArchiveSink::takeResults()
{
    static_assert(std::is_nothrow_move_constructible_v<ReceiveResult>);
    std::vector<ReceiveResult> result;
    result.reserve(ReceiveArchiveLimits::resultSlots);
    std::lock_guard lock(impl_->mutex);
    for (auto& slot : impl_->outcomes) if (slot.state == Impl::SlotState::Ready) {
        result.push_back(std::move(slot.result));
        slot.state = Impl::SlotState::Free;
        slot.progressChanged = false;
    }
    return result;
}
std::vector<ReceiveProgress> ReceiveArchiveSink::takeProgress()
{
    std::vector<ReceiveProgress> result;
    result.reserve(ReceiveArchiveLimits::resultSlots);
    std::lock_guard lock(impl_->mutex);
    for (auto& slot : impl_->outcomes) if (slot.state == Impl::SlotState::Pending && slot.progressChanged) {
        result.push_back({slot.result.sessionId, slot.result.fileId, slot.result.attempt, slot.written, slot.total});
        slot.progressChanged = false;
    }
    return result;
}
void ReceiveArchiveSink::cancel(const QString& id) noexcept { impl_->closeSession(id, true); }
void ReceiveArchiveSink::finish(const QString& id) noexcept { impl_->closeSession(id, false); }
void ReceiveArchiveSink::cancelAndJoin() noexcept { if (impl_) impl_->join(); }

ReceiveArchiveStatistics ReceiveArchiveSink::statistics() const
{
    std::lock_guard lock(impl_->mutex);
    const auto results = std::count_if(impl_->outcomes.begin(), impl_->outcomes.end(), [](const auto& slot) {
        return slot.state == Impl::SlotState::Ready;
    });
    return {impl_->sessions.size(), impl_->queue.size(), impl_->activeFiles,
        static_cast<std::size_t>(results), impl_->reservedBytes};
}

} // namespace HuhuCapture::Transfer
