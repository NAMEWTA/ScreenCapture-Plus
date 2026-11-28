#include "recording/SoftwareMp4Recording.h"
#include "services/media/Mp4Encoder.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <new>

namespace HuhuCapture::Recording {
namespace {
using Core::RecordingStage;
using Clock = Core::MediaClock;
using SampleTime = std::chrono::duration<std::int64_t, std::ratio<1, AudioSource::sampleRate>>;
}

BackendResult runSoftwareMp4Recording(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> video, std::unique_ptr<AudioSource> audio,
    const BackendEvents& events, std::stop_token stop)
{
    BackendResult result;
    std::unique_ptr<Media::Mp4Encoder> encoder;
    Core::FrameLease latest;
    std::optional<Core::FrameFormat> format;
    std::optional<Clock::time_point> origin;
    Core::MediaTime latestTime{};
    bool pendingVideo{}, audioFailed{}, sourceStopped{};
    std::uint64_t audioSamples{};
    std::array<float, 1024 * AudioSource::channels> pcm;
    auto lastAudioProgress = Clock::now();
    const auto fail = [&](RecordingStage stage, std::int64_t code, std::string detail) {
        if (!result.media.firstError) result.detail = std::move(detail);
        result.media.fail(stage, code ? code : EIO);
    };
    const auto encodedFailure = [&] {
        const auto& encoded = encoder->result();
        result.media.framesWritten = encoded.framesWritten;
        if (encoded.firstError) fail(encoded.firstError->stage, encoded.firstError->code,
            "The shared H.264/AAC encoder could not preserve complete media");
    };
    const auto encodeVideo = [&](Core::MediaTime end) {
        if (!pendingVideo || !encoder || !encoder->ready()) return;
        // Consume this input exactly once, even if the encoder accepted it but
        // its subsequent native mux/write failed. Finalization never retries it.
        pendingVideo = false;
        if (end <= latestTime) {
            fail(RecordingStage::Encode, EINVAL, "The final video interval is not strictly positive"); return;
        }
        if (!encoder->addFrame(latest, latestTime, end - latestTime)) { encodedFailure(); return; }
        result.media.framesWritten = encoder->result().framesWritten;
        if (!result.prepared) {
            result.prepared = PreparedRecording{request.region, Codec::H264, video->kind(), *origin,
                request.systemAudio, request.microphone};
            if (events.prepared) events.prepared(*result.prepared);
        }
    };
    const auto encodeAudio = [&](Core::MediaTime end, bool final) {
        if (!audio || audioFailed || !encoder || !encoder->ready() || !result.prepared) return;
        const auto limit = static_cast<std::uint64_t>(std::chrono::duration_cast<SampleTime>(end).count());
        while (audioSamples < limit) {
            constexpr std::size_t blockFrames = 1024;
            const auto remaining = limit - audioSamples;
            if (!final && remaining < blockFrames) return;
            const auto frames = static_cast<std::size_t>((std::min)(remaining, std::uint64_t{blockFrames}));
            const auto at = *origin + std::chrono::duration_cast<Clock::duration>(SampleTime{audioSamples});
            const auto now = Clock::now();
            const auto received = audio->read(at, {pcm.data(), frames * AudioSource::channels},
                final ? now + std::chrono::seconds(2) : now, final ? std::stop_token{} : stop);
            if (received.cancelled) return;
            if (!received) {
                // Device latency does not stall video. A real PCM stall still
                // fails within the bounded catch-up window; no silence is added.
                if (!final && received.error == ETIMEDOUT && now - lastAudioProgress < std::chrono::seconds(2)) return;
                audioFailed = true; fail(RecordingStage::Capture, received.error, received.detail); return;
            }
            if (!encoder->addAudio({pcm.data(), frames * AudioSource::channels}, audioSamples)) {
                audioFailed = true; encodedFailure(); return;
            }
            audioSamples += frames;
            lastAudioProgress = Clock::now();
        }
    };
    try {
        const auto capture = [&] {
            const auto aligned = alignedMp4Region(request.region, request.maximumFrameBytes);
            if (request.format != Format::Mp4 || !writer || !video || !aligned || *aligned != request.region ||
                !request.framesPerSecond || request.framesPerSecond > 60 || request.maximumDuration.count() <= 0 ||
                request.maximumDuration > std::chrono::minutes(120) || request.firstFrameTimeout.count() <= 0 ||
                request.firstFrameTimeout > std::chrono::minutes(1) || !request.maximumQueuedAudioBytes ||
                request.maximumQueuedAudioBytes > 64ull * 1024 * 1024 ||
                static_cast<bool>(audio) != (request.systemAudio || request.microphone)) {
                fail(RecordingStage::Prepare, EINVAL, "Invalid software MP4 recording parameters"); return;
            }
            if (stop.stop_requested()) return;
            const auto firstDeadline = Clock::now() + request.firstFrameTimeout;
            std::mutex mutex;
            std::condition_variable_any wait;
            while (!latest && !stop.stop_requested()) {
                auto received = video->read(firstDeadline, stop);
                if (stop.stop_requested()) return;
                if (received.status == FrameReadStatus::Stopped) { sourceStopped = true; return; }
                if (received.status == FrameReadStatus::Paused) {
                    fail(RecordingStage::Capture, EPIPE, "The authorized recording source paused; select an active source again"); return;
                }
                if (received.status == FrameReadStatus::Failed) {
                    fail(RecordingStage::Capture, received.error, received.detail); return;
                }
                if (received.status == FrameReadStatus::Frame) latest = std::move(received.frame);
                if (Clock::now() >= firstDeadline) {
                    fail(RecordingStage::Capture, ETIMEDOUT, "Timed out waiting for the first recording frame"); return;
                }
                if (!latest) {
                    std::unique_lock lock(mutex);
                    wait.wait_until(lock, stop, (std::min)(firstDeadline, Clock::now() + std::chrono::milliseconds(5)), [] { return false; });
                }
            }
            if (stop.stop_requested()) return;
            if (!latest) {
                fail(RecordingStage::Prepare, EINVAL, "The screen source returned an invalid initial timeline"); return;
            }
            format = latest.format();
            Media::Mp4EncoderOptions options;
            options.width = request.region.width; options.height = request.region.height;
            options.framesPerSecond = request.framesPerSecond;
            options.maximumFrameBytes = request.maximumFrameBytes;
            options.maximumQueuedAudioBytes = request.maximumQueuedAudioBytes;
            if (audio) options.audio = Media::Mp4AudioFormat{AudioSource::sampleRate, AudioSource::channels};
            encoder = std::make_unique<Media::Mp4Encoder>(options, std::move(writer));
            if (!encoder->ready()) { encodedFailure(); return; }
            if (audio) {
                const auto ready = audio->prepare(Clock::now() + request.firstFrameTimeout, stop);
                if (ready.cancelled) return;
                if (!ready) { fail(RecordingStage::Prepare, ready.error, ready.detail); return; }
            }
            if (stop.stop_requested()) return;
            origin = Clock::now(); lastAudioProgress = *origin;
            pendingVideo = true;
            const auto interval = std::chrono::nanoseconds(1'000'000'000 / request.framesPerSecond);
            const auto stopAt = *origin + request.maximumDuration;
            auto nextFrame = *origin + interval;
            while (!stop.stop_requested() && !result.media.firstError) {
                { std::unique_lock lock(mutex); wait.wait_until(lock, stop, (std::min)(nextFrame, stopAt), [] { return false; }); }
                if (stop.stop_requested()) break;
                if (Clock::now() >= stopAt) { result.durationLimitReached = true; break; }
                auto received = video->read(nextFrame, stop);
                if (stop.stop_requested()) break;
                if (received.status == FrameReadStatus::Stopped) { sourceStopped = true; break; }
                if (received.status == FrameReadStatus::Paused) {
                    fail(RecordingStage::Capture, EPIPE, "The authorized recording source paused; select an active source again"); break;
                }
                if (received.status == FrameReadStatus::Failed) {
                    fail(RecordingStage::Capture, received.error, received.detail); break;
                }
                if (received.status == FrameReadStatus::Frame &&
                    (!received.frame || received.frame.metadata().timing.discontinuity || received.frame.format() != *format)) {
                    fail(RecordingStage::Capture, EINVAL, "The recording source changed geometry, layout, color or timeline"); break;
                }
                const auto capturedAt = (std::min)(Clock::now(), stopAt);
                const auto pts = std::chrono::duration_cast<Core::MediaTime>(capturedAt - *origin);
                encodeVideo(pts);
                if (result.media.firstError) break;
                encodeAudio(pts, false);
                if (capturedAt == stopAt) { result.durationLimitReached = true; break; }
                if (received.status == FrameReadStatus::Frame) latest = std::move(received.frame);
                latestTime = pts; pendingVideo = true;
                nextFrame += interval;
                const auto after = Clock::now();
                if (nextFrame < after) nextFrame += interval * ((after - nextFrame) / interval + 1);
            }
        };
        capture();
        if (origin) {
            result.elapsed = std::chrono::duration_cast<Core::MediaTime>((std::min)(Clock::now() - *origin,
                std::chrono::duration_cast<Clock::duration>(request.maximumDuration)));
            const auto end = (std::max)(Core::MediaTime{1}, result.elapsed);
            encodeVideo(end);
            encodeAudio(end, true);
        }
    } catch (const std::bad_alloc&) { fail(RecordingStage::Encode, ENOMEM, "The MP4 recording exceeded available memory"); }
    catch (...) { fail(RecordingStage::Encode, EIO, "The MP4 recording worker failed"); }

    result.stopRequested = sourceStopped || stop.stop_requested();
    // No codec state survives an active native producer. Its explicit worker
    // barrier does not need a GUI callback and preserves already captured media.
    latest.reset();
    if (video) video->stopAndWait();
    video.reset(); audio.reset();
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
    return result;
}

} // namespace HuhuCapture::Recording
