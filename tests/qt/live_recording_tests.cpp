// Reuses PR #7's controlled-pixel -> production codec/session test method.
// Copyright (c) 2026 NAMEWTA, MIT. These are real codec/filesystem tests with
// explicit generated input, never evidence of Portal or audio-device capture.
#include "recording/PlatformRecording.h"
#include "services/capture/LiveFrameReader.h"
#include "recording/RecordingSession.h"
#include "services/media/GifInspection.h"
#include "services/media/VideoInspection.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace core = HuhuCapture::Core;
namespace rec = HuhuCapture::Recording;
namespace media = HuhuCapture::Media;
namespace capture = HuhuCapture::Capture;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* detail)
{ if (!value) throw std::runtime_error(detail); }

// The frame pool, queue, pacing and separate producer termination are real.
// Generated content isolates the worker/codec contract from native hardware.
class Pixels final {
public:
    enum class Mode { Static, Changing, PermissionLost, OrientationChanged, RequestedStop, UnexpectedStop, Paused, Discontinuity };
    static constexpr std::uint64_t sourceId = 41;
    explicit Pixels(Mode mode) : mode_(mode), pool_({4, 64u * 48u * 4u * 4u, 128})
    {
        auto format = core::packedFrameFormat(core::FramePixelFormat::Bgra8, 64, 48);
        require(format && pool_.configure(*format) == core::FramePoolStatus::Ready, "Allocate actual source pool");
        seed_ = frame(0);
        require(bool(seed_), "Produce an actual first-frame selection lease");
        producer_ = std::jthread([this](std::stop_token emergency) {
            std::stop_callback terminate(emergency, [this] { stop_.request_stop(); });
            core::FrameSourceTerminal terminal;
            try {
                {
                    std::unique_lock lock(beginMutex_);
                    begin_.wait(lock, stop_.get_token(), [this] { return begun_; });
                }
                unsigned sequence{};
                std::mutex mutex; std::condition_variable_any wait;
                while (!stop_.stop_requested()) {
                    std::unique_lock lock(mutex);
                    wait.wait_for(lock, stop_.get_token(), mode_ == Mode::Static ? 5s : 20ms, [] { return false; });
                    if (stop_.stop_requested()) break;
                    ++sequence;
                    if (mode_ == Mode::Paused && sequence == 6) {
                        activity_->store(core::LiveSourceActivity::Paused, std::memory_order_release);
                        wait.wait_for(lock, stop_.get_token(), 5s, [] { return false; });
                        continue;
                    }
                    if (mode_ == Mode::PermissionLost && sequence == 6) {
                        terminal = {core::FrameSourceEnd::PermissionDenied, EACCES, "The explicit input fixture revoked its source"}; break;
                    }
                    if ((mode_ == Mode::RequestedStop || mode_ == Mode::UnexpectedStop) && sequence == 6) {
                        // The session's separate token is deliberately not
                        // stopped. Only the real shared source request makes
                        // the first of these two identical queue ends normal.
                        if (mode_ == Mode::RequestedStop) stop_.request_stop();
                        break;
                    }
                    auto value = frame(sequence);
                    if (!value || !queue_->push(std::move(value))) {
                        terminal = {core::FrameSourceEnd::BudgetExceeded, ENOBUFS, "The actual source queue exhausted its frame slots"}; break;
                    }
                }
            } catch (...) { terminal = {core::FrameSourceEnd::PlatformFailure, EIO, "The controlled pixel producer failed"}; }
            activity_->store(core::LiveSourceActivity::Closed, std::memory_order_release);
            queue_->close(std::move(terminal));
            returned_.store(true, std::memory_order_release);
            finished_.set_value();
        });
    }
    ~Pixels() { stop_.request_stop(); if (producer_.joinable()) producer_.join(); }
    void beginProduction()
    {
        { std::lock_guard lock(beginMutex_); begun_ = true; }
        begin_.notify_all();
    }
    rec::Backend backend(unsigned invalidContract = 0)
    {
        require(bool(seed_), "One actual selected preview can be consumed only once");
        rec::FrameSourceContract contract{seed_.format(), seed_.metadata(), seed_.formatGeneration(), {64, 48}};
        readerState_ = std::make_shared<capture::LiveFrameReaderState>();
        auto& consumer = readerState_->consumer;
        consumer.sourceId = sourceId; consumer.generation = seed_.formatGeneration();
        consumer.selection = {4, 2, 56, 44}; consumer.frames = queue_; consumer.activity = activity_; consumer.stop = stop_;
        consumer.producerFinished = finished_.get_future().share(); consumer.firstFrame = std::move(seed_);
        consumer.claimedAt = core::MediaClock::now();
        if (invalidContract == 1) ++contract.preview.sourceId;
        if (invalidContract == 2) ++contract.generation;
        return rec::frameRecordingBackend(capture::makeLiveFrameReader(readerState_), contract);
    }
    bool returned() const { return returned_.load(std::memory_order_acquire); }
    bool drained() const
    { return returned() && queue_->closed() && !queue_->statistics().queued && readerState_ && readerState_->returned.load(std::memory_order_acquire); }
private:
    core::FrameLease frame(unsigned sequence)
    {
        auto target = pool_.tryAcquire();
        if (!target) return {};
        for (unsigned y = 0; y < 48; ++y) {
            auto row = target->row(0, y);
            for (unsigned x = 0; x < 64; ++x) {
                row[x * 4] = static_cast<std::uint8_t>(x * 3 + sequence * 7);
                row[x * 4 + 1] = static_cast<std::uint8_t>(y * 5 + sequence * 3);
                row[x * 4 + 2] = static_cast<std::uint8_t>((x ^ y) * 3); row[x * 4 + 3] = 255;
            }
        }
        core::FrameMetadata metadata;
        metadata.sessionId = 7; metadata.sourceId = sourceId;
        metadata.timing.receivedAt = core::MediaClock::now();
        metadata.timing.sourceSequence = sequence;
        metadata.timing.discontinuity = sequence == 0 || (mode_ == Mode::Discontinuity && sequence >= 6); // actual PW first-format convention
        if (mode_ == Mode::OrientationChanged && sequence >= 6) metadata.transform = core::FrameTransform::Rotate180;
        return std::move(*target).publish(metadata);
    }
    Mode mode_;
    core::FramePool pool_;
    std::shared_ptr<core::FrameQueue> queue_{std::make_shared<core::FrameQueue>(2)};
    std::shared_ptr<std::atomic<core::LiveSourceActivity>> activity_{
        std::make_shared<std::atomic<core::LiveSourceActivity>>(core::LiveSourceActivity::Streaming)};
    std::stop_source stop_;
    core::FrameLease seed_;
    std::atomic_bool returned_{};
    std::promise<void> finished_;
    std::shared_ptr<capture::LiveFrameReaderState> readerState_;
    std::mutex beginMutex_;
    std::condition_variable_any begin_;
    bool begun_{};
    std::jthread producer_;
};

rec::BackendRequest request(rec::Format format)
{
    rec::BackendRequest value;
    value.format = format; value.region = {4, 2, 56, 44};
    value.framesPerSecond = format == rec::Format::Gif ? 16 : 30;
    value.maximumDuration = 240ms; value.firstFrameTimeout = 1s;
    value.systemAudio = false; value.microphone = false;
    return value;
}
std::shared_ptr<media::MediaArchiveService> archive(const QString& path)
{
    return std::make_shared<media::MediaArchiveService>(path,
        [](const QString& file, media::Kind kind, std::stop_token stop) {
            return kind == media::Kind::Gif ? media::inspectGif(file, {}, stop) : media::inspectVideo(file, {}, stop);
        });
}
void encode(media::MediaArchiveService& archive, rec::Format format, Pixels::Mode mode)
{
    Pixels pixels(mode);
    auto backend = pixels.backend();
    auto input = request(format);
    if (mode != Pixels::Mode::Static && mode != Pixels::Mode::Changing) input.maximumDuration = 750ms;
    auto staging = archive.createStaging(); require(!staging.failure, "Create actual owned live-recording output");
    std::error_code error; auto writer = staging.file.openWriter(error); require(writer && !error, "Open identity-checked output");
    unsigned prepared{}, finalizing{};
    const rec::BackendEvents events{
        [&](const rec::PreparedRecording& value) {
            ++prepared;
            require(value.source == rec::CaptureSource::PipeWire && value.region == input.region,
                "Actual prepared media preserves its exact selected pixels and actual source kind");
            pixels.beginProduction();
        }, [&] {
            require(pixels.drained(), "The actual producer future and queue drain before codec finalization");
            ++finalizing;
        }};
    const auto result = backend(input, std::move(writer), events, {});
    require(pixels.drained() && prepared == 1 && finalizing == 1 && result.media.framesWritten >= 2,
        "The real media worker drains the producer queue and announces prepared/finalizing once");
    const bool failure = mode == Pixels::Mode::PermissionLost || mode == Pixels::Mode::OrientationChanged ||
        mode == Pixels::Mode::UnexpectedStop || mode == Pixels::Mode::Paused || mode == Pixels::Mode::Discontinuity;
    if (failure) {
        require(result.media.firstError && result.media.firstError->stage == core::RecordingStage::Capture &&
            result.media.finalized && !result.media.canArchive() && !result.detail.empty(),
            "Real input identity/terminal failure survives complete codec cleanup");
        const auto retained = staging.file.retainForRecovery();
        const auto inspection = format == rec::Format::Gif ? media::inspectGif(retained) : media::inspectVideo(retained);
        require(!inspection.failure && inspection.frames >= 1 && (format == rec::Format::Gif || inspection.frames == result.media.framesWritten),
            "Failure retains every actual submitted video frame in recoverable finalized media");
    } else {
        if (!result.media.canArchive()) throw std::runtime_error("Actual live encoder failed: " + result.detail);
        if (mode == Pixels::Mode::RequestedStop) {
            require(result.stopRequested && !result.durationLimitReached &&
                result.elapsed < std::chrono::duration_cast<core::MediaTime>(input.maximumDuration),
                "An actually requested producer stop survives the later session-token ordering without a false capture failure");
        } else {
            require(result.durationLimitReached && result.elapsed == std::chrono::duration_cast<core::MediaTime>(input.maximumDuration),
                "Automatic stop preserves the bounded real recording timeline");
        }
        const auto saved = archive.commitStaged(staging.file, format == rec::Format::Gif ? media::Kind::Gif : media::Kind::Mp4);
        require(saved.durable() && saved.media.dimensions == QSize(56, 44) && saved.media.frames >= 1 &&
            (format == rec::Format::Gif || saved.media.frames == result.media.framesWritten),
            "Strict actual decoding and durable archive retain selected pixels and every H.264 frame");
    }
    auto repeated = archive.createStaging(); require(!repeated.failure, "Create separate real reuse-rejection output");
    auto repeatedWriter = repeated.file.openWriter(error);
    const auto duplicate = backend(input, std::move(repeatedWriter), {}, {});
    require(duplicate.media.firstError && duplicate.media.firstError->stage == core::RecordingStage::Prepare &&
        !duplicate.media.framesWritten && QFileInfo(repeated.file.path()).size() == 0,
        "Copies of a source provider cannot consume one authorization twice");
    require(!repeated.file.discard(), "Rejected provider closes and discards only its owned empty output");
}
void admission(media::MediaArchiveService& archive)
{
    for (unsigned variant = 0; variant < 5; ++variant) {
        Pixels pixels(Pixels::Mode::Static);
        auto backend = pixels.backend(variant == 1 ? 1 : variant == 2 ? 2 : 0);
        auto input = request(rec::Format::Mp4);
        std::stop_source stop;
        if (variant == 0) stop.request_stop();
        if (variant == 3) input.region.width = 68; // even alignment cannot put it inside the authorized 64px source
        if (variant == 4) input.maximumFrameBytes = 0;
        auto staged = archive.createStaging(); require(!staged.failure, "Create real preflight output");
        std::error_code error; auto writer = staged.file.openWriter(error);
        const auto result = backend(input, std::move(writer), {}, stop.get_token());
        require(pixels.drained() && !result.prepared && !result.media.framesWritten &&
            (variant == 0 ? !result.media.firstError && result.stopRequested : result.media.firstError.has_value()) &&
            QFileInfo(staged.file.path()).size() == 0 && !staged.file.discard(),
            "Pre-cancel/identity/generation/selection/budget refusal drains the actual source and empty output");
    }
}

void session(const QString& path)
{
    auto files = archive(path);
    rec::RecordingSession recording(files, {});
    Pixels pixels(Pixels::Mode::Changing);
    std::optional<rec::SessionResult> completed;
    QObject::connect(&recording, &rec::RecordingSession::finished, &recording,
        [&](const auto& value) { completed = value; });
    QObject::connect(&recording, &rec::RecordingSession::prepared, &recording,
        [&](quint64, const auto&) { pixels.beginProduction(); });
    const auto token = recording.start(request(rec::Format::Mp4), pixels.backend());
    require(token && !recording.start(request(rec::Format::Mp4)), "A source-specific provider uses the one shared session admission");
    QElapsedTimer until; until.start();
    unsigned beats{};
    while (!completed) {
        require(until.elapsed() < 20000, "The actual codec/archive task did not return");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5); ++beats;
        std::this_thread::sleep_for(1ms);
    }
    require(completed->token == token && completed->state == rec::SessionState::Completed &&
        completed->archive.durable() && completed->archivedEntry && !recording.busy() && beats > 10 && pixels.drained(),
        "The single Qt session returns only after real finalize, strict decode and owned archive publication");
    recording.shutdown();
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        if (argc == 2) require(QDir().mkpath(QString::fromLocal8Bit(argv[1])), "Create actual evidence parent");
        QTemporaryDir directory(argc == 2 ? QDir(QString::fromLocal8Bit(argv[1])).absoluteFilePath(QStringLiteral("run-XXXXXX"))
                                         : QDir::tempPath() + QStringLiteral("/huhucapture-live-recording-XXXXXX"));
        require(directory.isValid(), "Create an exclusive live-recording test directory");
        if (argc == 2) { directory.setAutoRemove(false); std::cout << "Evidence: " << directory.path().toStdString() << '\n'; }
        auto files = archive(directory.filePath(QStringLiteral("archive")));
        encode(*files, rec::Format::Mp4, Pixels::Mode::Static);
        encode(*files, rec::Format::Gif, Pixels::Mode::Static);
        encode(*files, rec::Format::Mp4, Pixels::Mode::Changing);
        encode(*files, rec::Format::Mp4, Pixels::Mode::PermissionLost);
        encode(*files, rec::Format::Mp4, Pixels::Mode::OrientationChanged);
        encode(*files, rec::Format::Mp4, Pixels::Mode::RequestedStop);
        encode(*files, rec::Format::Mp4, Pixels::Mode::UnexpectedStop);
        encode(*files, rec::Format::Mp4, Pixels::Mode::Paused);
        encode(*files, rec::Format::Mp4, Pixels::Mode::Discontinuity);
        encode(*files, rec::Format::Gif, Pixels::Mode::Discontinuity);
        admission(*files); session(directory.filePath(QStringLiteral("session")));
        std::cout << "PASS actual frame pool/queue, single production reader, shared normalization, GIF/H.264, strict decoding, requested/unexpected terminal and pause, source-specific Qt session and durable archive\n";
        std::cout << "NOT RUN here: real desktop authorization/capture, physical audio, pause recovery or A/V drift; these inputs are explicit codec/worker fixtures\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL live recording: " << error.what() << '\n'; return 1; }
}
