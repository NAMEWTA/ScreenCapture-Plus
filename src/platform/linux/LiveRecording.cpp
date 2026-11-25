// Recording scheduling, pending-frame duration, bounded audio catch-up and
// producer-before-codec teardown reuse this project's PR #7 at 251bc0071278c6b711c840bf5188d16870bc7349.
// Copyright (c) 2026 NAMEWTA. MIT; see LICENSE and third_party/pipewire-audio/upstream.json.
#include "platform/linux/LiveRecording.h"
#include "core/GifLimits.h"
#include "platform/linux/pipewire/PipeWireAudio.h"
#include "services/media/FrameNormalizer.h"
#include "services/media/GifEncoder.h"
#include "services/media/Mp4Encoder.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

namespace HuhuCapture::Platform::Linux {
namespace {
using namespace Recording;
using Core::RecordingStage;
using Clock = Core::MediaClock;
using Time = Core::MediaTime;

struct SourceIdentity {
    Core::FrameFormat format;
    Core::FrameMetadata metadata;
    std::uint64_t generation{};
    explicit SourceIdentity(const Core::FrameLease& frame)
        : format(frame.format()), metadata(frame.metadata()), generation(frame.formatGeneration()) {}
    bool matches(const Core::FrameLease& frame) const noexcept
    {
        return frame && frame.format() == format && frame.formatGeneration() == generation &&
            frame.metadata().sessionId == metadata.sessionId && frame.metadata().sourceId == metadata.sourceId &&
            frame.metadata().crop == metadata.crop && frame.metadata().transform == metadata.transform;
    }
};

BackendResult record(const BackendRequest& request, FileHandle& writer, Core::LiveSourceConsumer source,
    const BackendEvents& events, std::stop_token stop)
{
    BackendResult result;
    std::unique_ptr<Media::Mp4Encoder> mp4;
    std::unique_ptr<Media::GifEncoder> gif;
    std::unique_ptr<AudioSource> audio;
    Core::FrameLease latest;
    bool pendingVideo{}, preparedSent{}, audioFailed{}, stoppingSource{};
    Time pendingTime{};
    std::uint64_t audioSamples{};
    auto lastAudioProgress = Clock::now();
    const auto fail = [&](RecordingStage stage, std::int64_t code, std::string_view detail) noexcept {
        const bool first = !result.media.firstError;
        result.media.fail(stage, code ? code : EIO);
        if (first) { try { result.detail.assign(detail); } catch (...) {} }
    };
    const auto terminal = [&](const Core::FrameSourceTerminal& value) {
        if (!result.sourceTerminal) result.sourceTerminal = value;
        // The GUI owner may stop its shared producer before the session's
        // separate stop token arrives. Only that actual request (or our own
        // drain) makes Stopped expected; an unrequested producer EOF is still
        // a capture failure, and permission/platform failures always survive.
        if (value.reason != Core::FrameSourceEnd::Stopped ||
            !(stoppingSource || source.stop.stop_requested() || stop.stop_requested()))
            fail(RecordingStage::Capture, value.nativeCode ? value.nativeCode : EPIPE,
                value.detail.empty() ? "The authorized recording source ended unexpectedly" : value.detail);
    };
    const auto announce = [&] {
        if (!preparedSent && result.prepared) {
            preparedSent = true;
            if (events.prepared) events.prepared(*result.prepared);
        }
    };
    const auto encodeVideo = [&](Time end) {
        if (!pendingVideo || !mp4) return;
        const auto duration = (std::max)(Time{1}, end - pendingTime);
        pendingVideo = false;
        if (!mp4->addFrame(latest, pendingTime, duration)) {
            if (const auto error = mp4->result().firstError) result.media.fail(error->stage, error->code);
            return;
        }
        announce();
    };
    std::array<float, 1024 * AudioSource::channels> pcm{};
    const auto encodeAudio = [&](Time end, bool final) {
        if (!audio || !mp4 || audioFailed || !result.prepared || !mp4->result().framesWritten) return;
        const auto limit = static_cast<std::uint64_t>(end.count()) * AudioSource::sampleRate / 10'000'000;
        while (audioSamples < limit) {
            const auto remaining = limit - audioSamples;
            if (!final && remaining < 1024) return;
            const auto count = (std::min)(remaining, std::uint64_t{1024});
            const auto at = result.prepared->started + std::chrono::nanoseconds(
                static_cast<std::int64_t>(audioSamples * 1'000'000'000 / AudioSource::sampleRate));
            const auto now = Clock::now();
            const auto received = audio->read(at, std::span(pcm).first(static_cast<std::size_t>(count) * AudioSource::channels),
                final ? now + std::chrono::seconds(2) : now, final ? std::stop_token{} : stop);
            if (received.cancelled) return;
            if (!received) {
                // Native latency may catch up while video continues. This does
                // not create silent PCM or extend a missing source indefinitely.
                if (!final && received.error == ETIMEDOUT && now - lastAudioProgress < std::chrono::seconds(2)) return;
                audioFailed = true; fail(RecordingStage::Capture, received.error, received.detail); return;
            }
            if (!mp4->addAudio(std::span(pcm).first(static_cast<std::size_t>(count) * AudioSource::channels), audioSamples)) {
                audioFailed = true;
                if (const auto error = mp4->result().firstError) result.media.fail(error->stage, error->code);
                return;
            }
            audioSamples += count;
            lastAudioProgress = Clock::now();
        }
    };
    try {
        const auto capture = [&] {
            const auto& selection = source.selection;
            if (!writer || !source.frames || !source.activity || !source.firstFrame ||
                request.coordinates != CoordinateSpace::AuthorizedSourcePixels || !source.sourceId || !source.generation ||
                request.sourceId != source.sourceId || request.generation != source.generation ||
                request.region.x < 0 || request.region.y < 0 ||
                selection.x != static_cast<unsigned>(request.region.x) || selection.y != static_cast<unsigned>(request.region.y) ||
                request.region.width <= 0 || request.region.height <= 0 ||
                selection.width != static_cast<unsigned>(request.region.width) || selection.height != static_cast<unsigned>(request.region.height) ||
                source.firstFrame.metadata().sourceId != source.sourceId ||
                !request.maximumFrameBytes || request.maximumFrameBytes > 256ull * 1024 * 1024 ||
                !request.maximumQueuedAudioBytes || request.maximumQueuedAudioBytes > 64ull * 1024 * 1024 ||
                !request.framesPerSecond || request.framesPerSecond > (request.format == Format::Gif ? 50u : 240u) ||
                request.firstFrameTimeout.count() <= 0 || request.firstFrameTimeout > std::chrono::minutes(1) ||
                request.maximumDuration.count() <= 0 || request.maximumDuration >
                    (request.format == Format::Gif ? std::chrono::minutes(6) : std::chrono::minutes(120)) ||
                (request.format == Format::Gif && (request.systemAudio || request.microphone ||
                    (request.maximumDuration.count() * request.framesPerSecond + 999) / 1000 + 1 > Core::maximumValidatedGifFrames))) {
                fail(RecordingStage::Prepare, EINVAL, "The recording request does not match its single authorized source selection or limits"); return;
            }
            if (stop.stop_requested()) return;
            const auto region = request.format == Format::Mp4 ? alignedMp4Region(request.region, request.maximumFrameBytes)
                                                            : std::optional<PixelRegion>{request.region};
            const auto layout = region ? Core::pixelLayout(region->width, region->height, 4) : std::nullopt;
            if (!region || !layout || layout->bytes > request.maximumFrameBytes) {
                fail(RecordingStage::Prepare, EINVAL, "The selected recording pixels exceed their geometry or memory limits"); return;
            }
            const SourceIdentity identity(source.firstFrame);
            const Core::FrameCrop selected{static_cast<std::uint32_t>(region->x), static_cast<std::uint32_t>(region->y),
                static_cast<std::uint32_t>(region->width), static_cast<std::uint32_t>(region->height)};
            Media::FrameNormalizer normalizer({2, static_cast<std::size_t>(request.maximumFrameBytes * 2), 16384});
            const auto accept = [&](Core::FrameLease frame) {
                if (!identity.matches(frame) || (result.prepared && frame.metadata().timing.discontinuity)) {
                    fail(RecordingStage::Capture, EPIPE, "The authorized source changed identity, format, crop, orientation or clock continuity"); return false;
                }
                auto normalized = normalizer.normalize(frame, selected);
                if (!normalized.frame) {
                    fail(RecordingStage::Capture, EINVAL, "The authorized source pixels cannot be normalized within their selected bounds and budget"); return false;
                }
                latest = std::move(normalized.frame);
                return true;
            };
            const auto firstDeadline = Clock::now() + request.firstFrameTimeout;
            std::mutex mutex;
            std::condition_variable_any wait;
            // A static compositor need not emit another damage frame while the
            // user selects. Prefer a newer queued frame, otherwise consume the
            // same real first-frame lease; never reinterpret its old source PTS
            // as time already recorded before this worker actually starts.
            while (!latest && !stop.stop_requested()) {
                auto read = source.frames->takeLatest();
                if (read.terminal) { terminal(*read.terminal); return; }
                if (source.frames->closed()) continue;
                if (source.streaming()) {
                    if (!read.frame) read.frame = std::move(source.firstFrame);
                    else source.firstFrame.reset();
                    if (read.frame && !accept(std::move(read.frame))) return;
                } else if (read.frame) source.firstFrame = std::move(read.frame);
                if (latest) break;
                if (Clock::now() >= firstDeadline) { fail(RecordingStage::Capture, ETIMEDOUT, "No active authorized source supplied the first recording frame"); return; }
                std::unique_lock lock(mutex);
                wait.wait_until(lock, stop, (std::min)(firstDeadline, Clock::now() + std::chrono::milliseconds(5)), [] { return false; });
            }
            source.firstFrame.reset();
            if (stop.stop_requested()) return;
            if (request.format == Format::Mp4) {
                Media::Mp4EncoderOptions options{region->width, region->height, request.framesPerSecond};
                options.maximumFrameBytes = request.maximumFrameBytes;
                options.maximumQueuedAudioBytes = request.maximumQueuedAudioBytes;
                if (request.systemAudio || request.microphone) options.audio = Media::Mp4AudioFormat{AudioSource::sampleRate, AudioSource::channels};
                mp4 = std::make_unique<Media::Mp4Encoder>(options, std::move(writer));
                if (!mp4->ready()) { result.media = mp4->finish(); return; }
                audio = makePipeWireAudio(request.systemAudio, request.microphone, request.maximumQueuedAudioBytes);
                if (audio) {
                    const auto ready = audio->prepare(firstDeadline, stop);
                    if (ready.cancelled) return;
                    if (!ready) { fail(RecordingStage::Prepare, ready.error, ready.detail); return; }
                }
            } else {
                gif = std::make_unique<Media::GifEncoder>(Media::GifEncoderOptions{region->width, region->height,
                    request.maximumFrameBytes}, std::move(writer));
                if (!gif->ready()) { result.media = gif->finish(); return; }
            }
            if (stop.stop_requested()) return;
            // Audio preparation may take time: refresh from the same grant.
            auto refreshed = source.frames->takeLatest();
            if (refreshed.terminal) { terminal(*refreshed.terminal); return; }
            if (refreshed.frame && !accept(std::move(refreshed.frame))) return;
            if (!source.streaming()) { fail(RecordingStage::Capture, EPIPE, "The authorized source stopped before recording began"); return; }
            result.prepared = PreparedRecording{*region, gif ? Codec::Gif : Codec::H264, CaptureSource::PipeWire,
                Clock::now(), request.systemAudio, request.microphone, request.coordinates, request.sourceId, request.generation};
            lastAudioProgress = result.prepared->started;
            if (gif) {
                if (!gif->addFrame(latest, Time{})) { result.media = gif->result(); return; }
                announce();
            } else { pendingVideo = true; pendingTime = {}; }
            const auto interval = std::chrono::nanoseconds(1'000'000'000 / request.framesPerSecond);
            const auto stopAt = result.prepared->started + request.maximumDuration;
            auto nextFrame = result.prepared->started + interval;
            while (!stop.stop_requested() && !result.media.firstError) {
                { std::unique_lock lock(mutex); wait.wait_until(lock, stop, (std::min)(nextFrame, stopAt), [] { return false; }); }
                if (stop.stop_requested()) break;
                if (Clock::now() >= stopAt) { result.durationLimitReached = true; break; }
                auto read = source.frames->takeLatest();
                if (read.terminal) { terminal(*read.terminal); break; }
                if (source.frames->closed()) continue;
                const auto capturedAt = (std::min)(Clock::now(), stopAt);
                const auto time = std::chrono::duration_cast<Time>(capturedAt - result.prepared->started);
                encodeVideo(time);
                if (result.media.firstError) break;
                if (read.frame && !accept(std::move(read.frame))) break;
                // Quiet STREAMING allows repetition of real static content.
                // PAUSED/Closed never masquerades as a healthy idle source.
                if (source.streaming()) {
                    if (gif) {
                        if (!gif->addFrame(latest, time)) { result.media = gif->result(); break; }
                    } else { pendingTime = time; pendingVideo = true; }
                }
                encodeAudio(time, false);
                nextFrame += interval;
                const auto after = Clock::now();
                if (nextFrame < after) nextFrame += interval * ((after - nextFrame) / interval + 1);
            }
        };
        capture();
        if (result.prepared) {
            result.elapsed = std::chrono::duration_cast<Time>((std::min)(Clock::now() - result.prepared->started,
                std::chrono::duration_cast<Clock::duration>(request.maximumDuration)));
            encodeVideo((std::max)(Time{1}, result.elapsed));
            encodeAudio(result.elapsed, true);
        }
    } catch (const std::bad_alloc&) { fail(RecordingStage::Encode, ENOMEM, "The recording exceeded its available memory"); }
    catch (...) { fail(RecordingStage::Encode, EIO, "The recording worker could not complete its media work"); }

    result.stopRequested = stop.stop_requested() || source.stop.stop_requested();
    stoppingSource = true;
    source.stop.request_stop();
    source.firstFrame.reset(); latest.reset();
    // No GUI callback is needed to close this queue: the producer publishes
    // its terminal on its own worker. The GUI owner separately waits for its
    // actual native return as well as this consumer's completion.
    if (source.frames) {
        try {
            for (;;) {
                auto read = source.frames->waitNext();
                if (read.terminal) { terminal(*read.terminal); break; }
            }
        } catch (...) { result.media.fail(RecordingStage::Capture, ENOMEM); }
    }
    audio.reset();
    if (result.prepared) {
        try { if (events.finalizing) events.finalizing(); }
        catch (...) { result.media.fail(RecordingStage::Finalize, EIO); }
    }
    if (mp4 || gif) {
        const auto encoded = mp4 ? mp4->finish() : gif->finish();
        if (encoded.firstError) result.media.fail(encoded.firstError->stage, encoded.firstError->code);
        if (encoded.finalizationError) result.media.fail(RecordingStage::Finalize, encoded.finalizationError->code);
        result.media.framesWritten = encoded.framesWritten;
        result.media.finalized = encoded.finalized && !result.media.finalizationError;
    }
    return result;
}
}

Recording::Backend makeLiveRecordingBackend(Core::LiveSourceConsumer source)
{
    struct Claim { std::mutex mutex; std::optional<Core::LiveSourceConsumer> source; };
    auto claim = std::make_shared<Claim>(); claim->source = std::move(source);
    return [claim](const BackendRequest& request, FileHandle writer, const BackendEvents& events, std::stop_token stop) {
        std::optional<Core::LiveSourceConsumer> owned;
        { std::lock_guard lock(claim->mutex); owned.swap(claim->source); }
        BackendResult result;
        try {
            if (!owned) result.media.fail(RecordingStage::Prepare, EALREADY);
            else result = record(request, writer, std::move(*owned), events, stop);
        } catch (...) { result.media.fail(RecordingStage::Encode, ENOMEM); }
        result.stopRequested = result.stopRequested || stop.stop_requested();
        std::error_code error;
        if (!writer.close(error)) result.media.finish(false, error.value());
        return result;
    };
}

} // namespace HuhuCapture::Platform::Linux
