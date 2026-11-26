#include "recording/GifRecording.h"
#include "services/media/GifEncoder.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <new>

namespace HuhuCapture::Recording {

BackendResult runGifRecording(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> source, const BackendEvents& events, std::stop_token stop)
{
    using Core::RecordingStage;
    using Clock = Core::MediaClock;
    BackendResult result;
    bool sourceStopped{};
    std::unique_ptr<Media::GifEncoder> encoder;
    const auto fail = [&](RecordingStage stage, std::int64_t code, const char* detail) {
        if (!result.media.firstError) result.detail = detail;
        result.media.fail(stage, code);
    };
    const auto encodedFailure = [&] {
        const auto& encoded = encoder->result();
        result.media.framesWritten = encoded.framesWritten;
        if (encoded.firstError)
            fail(encoded.firstError->stage, encoded.firstError->code, "The shared GIF encoder could not preserve a complete frame");
    };
    try {
        const auto capture = [&] {
            const auto layout = Core::pixelLayout(request.region.width, request.region.height, 4);
            if (request.format != Format::Gif || !writer || !source || !layout ||
                layout->bytes > request.maximumFrameBytes || request.region.width > 16384 ||
                request.region.height > 16384 || !request.framesPerSecond || request.framesPerSecond > 16 ||
                request.maximumDuration.count() <= 0 || request.maximumDuration > std::chrono::minutes(6) ||
                request.firstFrameTimeout.count() <= 0 || request.firstFrameTimeout > std::chrono::minutes(1) ||
                request.systemAudio || request.microphone) {
                fail(RecordingStage::Prepare, EINVAL, "Invalid GIF recording parameters"); return;
            }
            if (stop.stop_requested()) return;
            encoder = std::make_unique<Media::GifEncoder>(
                Media::GifEncoderOptions{request.region.width, request.region.height, request.maximumFrameBytes}, std::move(writer));
            if (!encoder->ready()) { encodedFailure(); return; }
            Core::FrameLease latest;
            std::optional<Core::FrameFormat> format;
            const auto firstDeadline = Clock::now() + request.firstFrameTimeout;
            const auto interval = std::chrono::nanoseconds(1'000'000'000 / request.framesPerSecond);
            auto nextFrame = Clock::now();
            std::mutex mutex;
            std::condition_variable_any wait;
            while (!stop.stop_requested()) {
                const auto now = Clock::now();
                if (result.prepared && now - result.prepared->started >= request.maximumDuration) {
                    result.durationLimitReached = true; break;
                }
                auto received = source->read(result.prepared ? nextFrame : firstDeadline, stop);
                if (stop.stop_requested()) break;
                if (received.status == FrameReadStatus::Stopped) { sourceStopped = true; break; }
                if (received.status == FrameReadStatus::Paused) {
                    fail(RecordingStage::Capture, EPIPE, "The authorized recording source paused; select an active source again"); break;
                }
                if (received.status == FrameReadStatus::Failed) {
                    if (!result.media.firstError) result.detail = std::move(received.detail);
                    result.media.fail(RecordingStage::Capture, received.error ? received.error : EIO); break;
                }
                if (received.status == FrameReadStatus::Frame) {
                    // PipeWire marks its first format frame discontinuous;
                    // only an established recording can lose clock continuity.
                    if (!received.frame || (format && (received.frame.metadata().timing.discontinuity ||
                        received.frame.format() != *format))) {
                        fail(RecordingStage::Capture, EINVAL, "The GIF source geometry, color or timeline changed"); break;
                    }
                    if (!format) format = received.frame.format();
                    latest = std::move(received.frame);
                }
                if (!result.prepared && Clock::now() >= firstDeadline) {
                    fail(RecordingStage::Capture, ETIMEDOUT, "Timed out waiting for the first recording frame"); break;
                }
                if (!latest) {
                    std::unique_lock lock(mutex);
                    wait.wait_until(lock, stop, (std::min)(firstDeadline, Clock::now() +
                        std::chrono::milliseconds(5)), [] { return false; });
                    continue;
                }
                const auto capturedAt = Clock::now();
                if (result.prepared && capturedAt - result.prepared->started >= request.maximumDuration) {
                    result.durationLimitReached = true; break;
                }
                // Source pacing and real elapsed PTS remain here; only codec,
                // buffer conversion, write/flush/close live in the shared service.
                const auto time = result.prepared ? std::chrono::duration_cast<Core::MediaTime>(
                    capturedAt - result.prepared->started) : Core::MediaTime{};
                if (!encoder->addFrame(latest, time)) { encodedFailure(); break; }
                result.media.framesWritten = encoder->result().framesWritten;
                if (!result.prepared) {
                    result.prepared = PreparedRecording{request.region, Codec::Gif, source->kind(), capturedAt, false, false};
                    nextFrame = capturedAt;
                    if (events.prepared) events.prepared(*result.prepared);
                }
                nextFrame += interval;
                const auto after = Clock::now();
                if (nextFrame < after) nextFrame += interval * ((after - nextFrame) / interval + 1);
                const auto stopAt = result.prepared->started + request.maximumDuration;
                std::unique_lock lock(mutex);
                wait.wait_until(lock, stop, (std::min)(nextFrame, stopAt), [] { return false; });
            }
        };
        capture();
    } catch (const std::bad_alloc&) { fail(RecordingStage::Encode, ENOMEM, "The GIF recording exceeded available memory"); }
    catch (...) { fail(RecordingStage::Encode, EIO, "The GIF recording worker failed"); }

    // Stop time does not include producer shutdown, codec draining or disk work.
    if (result.prepared) result.elapsed = std::chrono::duration_cast<Core::MediaTime>(
        (std::min)(Clock::now() - result.prepared->started, std::chrono::duration_cast<Clock::duration>(request.maximumDuration)));
    if (source) source->stopAndWait();
    source.reset();
    if (encoder) {
        if (result.media.framesWritten && events.finalizing) {
            try { events.finalizing(); }
            catch (...) { fail(RecordingStage::Finalize, EIO, "The finalization callback failed"); }
        }
        const auto encoded = encoder->finish();
        if (encoded.firstError) result.media.fail(encoded.firstError->stage, encoded.firstError->code);
        if (encoded.finalizationError) result.media.fail(RecordingStage::Finalize, encoded.finalizationError->code);
        result.media.framesWritten = encoded.framesWritten;
        result.media.finalized = encoded.finalized && !result.media.finalizationError;
    }
    std::error_code closeError;
    if (!writer.close(closeError)) result.media.finish(false, closeError.value());
    result.stopRequested = sourceStopped || stop.stop_requested();
    return result;
}

} // namespace HuhuCapture::Recording
