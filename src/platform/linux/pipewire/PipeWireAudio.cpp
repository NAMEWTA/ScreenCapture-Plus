/* Stream negotiation and dequeue/process/queue lifecycle adapted from
 * PipeWire 1.6.11 src/examples/audio-capture.c (MIT).
 * Copyright © 2022 Wim Taymans. See third_party/pipewire-audio/.
 */
#include "platform/linux/pipewire/PipeWireAudio.h"
#include "platform/linux/pipewire/AudioSampleGrid.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace HuhuCapture::Platform::Linux {
namespace {
using namespace Recording;
using Clock = Core::MediaClock;
constexpr std::size_t maximumPacketFrames = 4096;
constexpr std::size_t packetCount = 64;
using SampleDuration = AudioSampleDuration;

struct Packet {
    Clock::time_point begin;
    std::uint32_t frames{};
    std::uint64_t nativeCycle{}, reportTicks{};
    std::int64_t reportTime{}, reportDelay{};
    std::uint64_t reportBuffered{};
    std::array<float, maximumPacketFrames * AudioSource::channels> samples;
};
struct Ring {
    bool enabled{}, streaming{}, received{};
    std::size_t head{}, count{};
    std::vector<Packet> packets;
};

class PipeWireAudio final : public AudioSource {
public:
    PipeWireAudio(bool systemAudio, bool microphone, std::uint64_t maximumQueuedBytes)
    {
        rings_[0].enabled = systemAudio; rings_[1].enabled = microphone;
        const auto sources = static_cast<unsigned>(systemAudio) + static_cast<unsigned>(microphone);
        const auto capacity = sources ? (std::min)(std::uint64_t{packetCount},
            maximumQueuedBytes / sources / sizeof(Packet)) : 0;
        if (!sources || capacity < 2 || maximumQueuedBytes > 64ull * 1024 * 1024) {
            budgetError_ = true; return;
        }
        for (auto& ring : rings_) if (ring.enabled) ring.packets.resize(static_cast<std::size_t>(capacity));
    }
    ~PipeWireAudio() override
    {
        worker_.request_stop();
        // The thread is independent of Qt and its loop checks cancellation at
        // most every 50 ms. Joining happens on the recording worker only.
        if (worker_.joinable()) worker_.join();
    }
    AudioReadResult prepare(Clock::time_point deadline, std::stop_token stop) override
    {
        if (stop.stop_requested()) return {0, {}, true};
        if (budgetError_) return {ENOBUFS, "The selected audio sources exceed their combined PCM queue budget", false};
        // Opening codecs and rejecting invalid requests must not start native
        // device capture. The recording worker starts the requested streams only
        // when preparation reaches this boundary.
        if (!worker_.joinable()) worker_ = std::jthread([this](std::stop_token token) { run(token); });
        std::unique_lock lock(mutex_);
        changed_.wait_until(lock, stop, deadline, [this] {
            if (failure_ || finished_) return true;
            for (const auto& ring : rings_) if (ring.enabled && !ring.received) return false;
            return true;
        });
        if (stop.stop_requested()) return {0, {}, true};
        if (failure_) return *failure_;
        if (finished_) return {EPIPE, "The audio capture source stopped", false};
        for (const auto& ring : rings_) if (ring.enabled && !ring.received)
            return {ETIMEDOUT, "A requested system or microphone source produced no audio before the deadline", false};
        return {};
    }
    AudioReadResult read(Clock::time_point begin, std::span<float> output,
        Clock::time_point deadline, std::stop_token stop) override
    {
        if (output.empty() || output.size() % channels || output.size() / channels > maximumPacketFrames)
            return {EINVAL, "Invalid bounded audio output buffer", false};
        const auto frames = output.size() / channels;
        std::unique_lock lock(mutex_);
        reading_ = true;
        const auto expired = [begin](const Packet& packet) {
            const auto index = audioSampleIndex(begin - packet.begin, 0);
            return index && *index >= packet.frames;
        };
        for (auto& ring : rings_) {
            while (ring.count && expired(ring.packets[ring.head])) {
                ring.head = (ring.head + 1) % ring.packets.size(); --ring.count;
            }
        }
        const auto ready = [this, begin, frames] {
            if (failure_ || finished_) return true;
            for (const auto& ring : rings_) {
                if (!ring.enabled) continue;
                if (!ring.count) return false;
                const auto& last = ring.packets[(ring.head + ring.count - 1) % ring.packets.size()];
                const auto index = audioSampleIndex(begin - last.begin, static_cast<std::uint32_t>(frames - 1));
                // Invalid arithmetic is reported by read, rather than waiting
                // indefinitely for a packet that cannot repair the request.
                if (index && *index >= last.frames) return false;
            }
            return true;
        };
        changed_.wait_until(lock, stop, deadline, ready);
        if (stop.stop_requested()) return {0, {}, true};
        if (failure_) return *failure_;
        if (!ready() || finished_) return {finished_ ? EPIPE : ETIMEDOUT,
            "A requested audio source stopped delivering samples", false};
        std::fill(output.begin(), output.end(), 0.0f);
        for (auto& ring : rings_) {
            if (!ring.enabled) continue;
            while (ring.count && expired(ring.packets[ring.head])) {
                ring.head = (ring.head + 1) % ring.packets.size(); --ring.count;
            }
            std::size_t packetOffset{};
            for (std::size_t sample = 0; sample < frames; ++sample) {
                std::optional<std::int64_t> position;
                while (packetOffset < ring.count) {
                    const auto& packet = ring.packets[(ring.head + packetOffset) % ring.packets.size()];
                    position = audioSampleIndex(begin - packet.begin, static_cast<std::uint32_t>(sample));
                    if (!position) return {EOVERFLOW, "The requested PCM sample position exceeds the clock range", false};
                    if (*position < packet.frames) break;
                    ++packetOffset;
                }
                if (packetOffset >= ring.count) return {EPIPE, "Audio timeline ended before the requested samples", false};
                const auto& packet = ring.packets[(ring.head + packetOffset) % ring.packets.size()];
                if (*position < 0) {
                    // Only native GAP/EMPTY buffers represent actual silence.
                    // A missing interval is not a licence to fabricate up to
                    // 100 ms of PCM and then report that both sources arrived.
                    const auto at = begin + std::chrono::duration_cast<Clock::duration>(SampleDuration{sample});
                    const auto nanoseconds = [](auto value) {
                        return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
                    };
                    return {EPIPE, "The requested audio interval has no captured samples (gap_ns=" +
                        std::to_string(nanoseconds(packet.begin - at)) + ", request_frame=" + std::to_string(sample) +
                        ", packet_frames=" + std::to_string(packet.frames) + ", native_cycle=" +
                        std::to_string(packet.nativeCycle) + ", report_now=" + std::to_string(packet.reportTime) +
                        ", report_ticks=" + std::to_string(packet.reportTicks) + ", delay=" +
                        std::to_string(packet.reportDelay) + ", buffered=" + std::to_string(packet.reportBuffered) + ")", false};
                }
                const auto index = static_cast<std::size_t>(*position);
                if (index >= packet.frames) return {EPIPE, "The captured PCM clock does not cover the requested sample", false};
                for (std::size_t channel = 0; channel < channels; ++channel)
                    output[sample * channels + channel] += packet.samples[index * channels + channel];
            }
        }
        // Match the existing sum-and-limit mixer rather than changing single
        // source gain. No audio source is omitted because another is silent.
        for (auto& sample : output) sample = std::clamp(sample, -1.0f, 1.0f);
        return {};
    }
private:
    struct Stream {
        PipeWireAudio* owner{};
        std::size_t index{};
        pw_stream* stream{};
        bool formatted{}, streaming{};
    };
    std::array<Ring, 2> rings_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<AudioReadResult> failure_;
    bool finished_{}, reading_{}, budgetError_{};
    std::jthread worker_;
    // Only the native worker accesses this calibration. Both requested
    // streams use the same mapping; callback scheduling does not move it.
    struct ClockAnchor { Clock::time_point steady; std::uint64_t native{}; };
    std::optional<ClockAnchor> clock_;

    bool calibrate(pw_stream* stream) noexcept
    {
        if (clock_) return true;
        auto shortest = Clock::duration::max();
        ClockAnchor anchor;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            const auto before = Clock::now();
            const auto native = pw_stream_get_nsec(stream);
            const auto after = Clock::now();
            if (after - before < shortest) {
                shortest = after - before;
                anchor = {before + shortest / 2, native};
            }
        }
        if (!anchor.native || shortest > std::chrono::duration_cast<Clock::duration>(SampleDuration{1}) / 2) {
            fail(EIO, "The native audio clock could not be related to the recording clock within half a sample");
            return false;
        }
        clock_ = anchor;
        return true;
    }

    void fail(int error, const char* detail) noexcept
    {
        try {
            std::lock_guard lock(mutex_);
            if (!failure_) failure_ = AudioReadResult{error ? error : EIO, detail ? detail : "Audio capture failed", false};
        } catch (...) {
            std::lock_guard lock(mutex_); finished_ = true;
        }
        changed_.notify_all();
    }
    static void stateChanged(void* opaque, pw_stream_state, pw_stream_state next, const char* error) noexcept
    {
        auto& state = *static_cast<Stream*>(opaque);
        if (next == PW_STREAM_STATE_STREAMING) {
            state.streaming = true;
            std::lock_guard lock(state.owner->mutex_); state.owner->rings_[state.index].streaming = true;
        } else if (next == PW_STREAM_STATE_PAUSED) {
            std::lock_guard lock(state.owner->mutex_); state.owner->rings_[state.index].streaming = false;
        } else if (next == PW_STREAM_STATE_ERROR || (state.streaming && next == PW_STREAM_STATE_UNCONNECTED))
            state.owner->fail(EPIPE, error ? error : "The selected audio device disconnected");
    }
    static void formatChanged(void* opaque, std::uint32_t id, const spa_pod* parameter) noexcept
    {
        auto& state = *static_cast<Stream*>(opaque);
        if (id != SPA_PARAM_Format) return;
        if (!parameter) { state.formatted = false; return; }
        spa_audio_info_raw format{};
        std::uint32_t type{}, subtype{};
        if (spa_format_parse(parameter, &type, &subtype) < 0 || type != SPA_MEDIA_TYPE_audio ||
            subtype != SPA_MEDIA_SUBTYPE_raw || spa_format_audio_raw_parse(parameter, &format) < 0 ||
            format.format != SPA_AUDIO_FORMAT_F32 || format.rate != sampleRate || format.channels != channels ||
            format.position[0] != SPA_AUDIO_CHANNEL_FL || format.position[1] != SPA_AUDIO_CHANNEL_FR ||
            (format.flags & SPA_AUDIO_FLAG_UNPOSITIONED)) {
            state.owner->fail(ENOTSUP, "PipeWire did not provide the requested stereo 48 kHz float audio"); return;
        }
        state.formatted = true;
    }
    static void process(void* opaque) noexcept
    {
        auto& state = *static_cast<Stream*>(opaque);
        auto* buffer = pw_stream_dequeue_buffer(state.stream);
        if (!buffer) return;
        struct ReturnBuffer { pw_stream* stream; pw_buffer* buffer; ~ReturnBuffer() { pw_stream_queue_buffer(stream, buffer); } } returned{state.stream, buffer};
        if (!state.formatted || !buffer->buffer || buffer->buffer->n_datas != 1) {
            state.owner->fail(EINVAL, "The audio stream returned an invalid buffer layout"); return;
        }
        const auto& plane = buffer->buffer->datas[0];
        if (!plane.data || !plane.chunk || (plane.type != SPA_DATA_MemPtr && plane.type != SPA_DATA_MemFd) ||
            plane.chunk->offset > plane.maxsize ||
            plane.chunk->size > plane.maxsize - plane.chunk->offset || plane.chunk->size % (sizeof(float) * channels)) {
            state.owner->fail(EINVAL, "The audio stream returned an incomplete PCM buffer"); return;
        }
        if ((plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
            (plane.chunk->stride && plane.chunk->stride != static_cast<int>(sizeof(float) * channels))) {
            state.owner->fail(EINVAL, "The audio stream returned corrupted or noninterleaved PCM"); return;
        }
        const auto* header = static_cast<const spa_meta_header*>(spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header, sizeof(spa_meta_header)));
        if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) {
            state.owner->fail(EINVAL, "The audio stream marked its capture buffer corrupted"); return;
        }
        const auto frames = plane.chunk->size / (sizeof(float) * channels);
        if (!frames) return;
        if (frames > maximumPacketFrames) { state.owner->fail(ENOBUFS, "The audio quantum exceeds the recording budget"); return; }
        pw_time timing{};
        if (pw_stream_get_time_n(state.stream, &timing, sizeof(timing)) < 0 || !timing.rate.num || !timing.rate.denom) {
            state.owner->fail(EIO, "The audio stream did not provide its graph clock"); return;
        }
        const auto graphNow = pw_stream_get_nsec(state.stream);
        if (timing.now < 0 || graphNow < static_cast<std::uint64_t>(timing.now) ||
            graphNow - static_cast<std::uint64_t>(timing.now) > 1'000'000'000ull || !buffer->time ||
            buffer->time > graphNow || graphNow - buffer->time > 1'000'000'000ull) {
            state.owner->fail(EINVAL, "The audio graph clock discontinuity is too large"); return;
        }
        const double delaySeconds = std::max(0.0, static_cast<double>(timing.delay) * timing.rate.num / timing.rate.denom) +
            (static_cast<double>(timing.buffered) + frames) / sampleRate;
        if (!std::isfinite(delaySeconds) || delaySeconds > 2.0) {
            state.owner->fail(EINVAL, "The audio device latency exceeds the bounded timeline"); return;
        }
        if (!state.owner->calibrate(state.stream)) return;
        const auto& anchor = *state.owner->clock_;
        const auto forward = buffer->time >= anchor.native;
        const auto distance = forward ? buffer->time - anchor.native : anchor.native - buffer->time;
        if (distance > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            state.owner->fail(EOVERFLOW, "The native audio clock exceeds the recording timeline"); return;
        }
        const auto elapsed = std::chrono::nanoseconds(static_cast<std::int64_t>(distance));
        // stream.c stamps pw_buffer.time with this buffer's actual graph cycle.
        // pw_time.now may already describe a newer cycle when the main loop
        // dequeues an older buffer. Re-estimating an offset on every callback
        // also turns two clock-call latencies into artificial packet gaps.
        const auto begin = anchor.steady + (forward ? elapsed : -elapsed) -
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delaySeconds));
        try {
            std::lock_guard lock(state.owner->mutex_);
            auto& ring = state.owner->rings_[state.index];
            if (header && (header->flags & SPA_META_HEADER_FLAG_DISCONT) && ring.received && state.owner->reading_) {
                if (!state.owner->failure_) state.owner->failure_ = AudioReadResult{EPIPE,
                    "The requested audio source reported a discontinuity", false};
                state.owner->changed_.notify_all(); return;
            }
            if (ring.count == ring.packets.size() && !state.owner->reading_) {
                // Samples preceding the recording origin are not recorded
                // media. Keep the latest preparation window within the budget.
                ring.head = (ring.head + 1) % ring.packets.size(); --ring.count;
            }
            if (ring.count == ring.packets.size()) {
                if (!state.owner->failure_) state.owner->failure_ = AudioReadResult{ENOBUFS,
                    "The recording encoder cannot keep up with the bounded audio queue", false};
            } else {
                auto& packet = ring.packets[(ring.head + ring.count) % ring.packets.size()];
                packet.begin = begin; packet.frames = frames;
                packet.nativeCycle = buffer->time; packet.reportTime = timing.now;
                packet.reportTicks = timing.ticks; packet.reportDelay = timing.delay; packet.reportBuffered = timing.buffered;
                const auto* data = static_cast<const std::uint8_t*>(plane.data) + plane.chunk->offset;
                if ((plane.chunk->flags & SPA_CHUNK_FLAG_EMPTY) || (header && (header->flags & SPA_META_HEADER_FLAG_GAP)))
                    std::fill_n(packet.samples.data(), frames * channels, 0.0f);
                else std::memcpy(packet.samples.data(), data, plane.chunk->size);
                for (std::size_t index = 0; index < frames * channels; ++index)
                    if (!std::isfinite(packet.samples[index])) {
                        if (!state.owner->failure_) state.owner->failure_ = AudioReadResult{EINVAL,
                            "The audio device returned nonfinite PCM samples", false};
                        state.owner->changed_.notify_all(); return;
                    }
                ++ring.count; ring.received = true;
            }
        } catch (...) { state.owner->fail(ENOMEM, "Could not preserve the captured audio samples"); }
        state.owner->changed_.notify_all();
    }
    void run(std::stop_token stop) noexcept
    {
        pw_init(nullptr, nullptr);
        auto* main = pw_main_loop_new(nullptr);
        std::array<Stream, 2> streams{{{this, 0, nullptr, false, false}, {this, 1, nullptr, false, false}}};
        if (!main) fail(ENOMEM, "Cannot create the audio capture loop");
        if (main) {
            auto* loop = pw_main_loop_get_loop(main);
            pw_stream_events events{};
            events.version = PW_VERSION_STREAM_EVENTS; events.state_changed = &stateChanged;
            events.param_changed = &formatChanged; events.process = &process;
            for (std::size_t index = 0; index < streams.size() && !stop.stop_requested(); ++index) {
                if (!rings_[index].enabled) continue;
                auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                    PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, "huhucapture", nullptr);
                if (!properties) { fail(ENOMEM, "Cannot allocate audio stream properties"); break; }
                if (index == 0 && pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true") < 0) {
                    pw_properties_free(properties); fail(ENOMEM, "Cannot select the system sink monitor"); break;
                }
                streams[index].stream = pw_stream_new_simple(loop,
                    index == 0 ? "huhucapture system audio" : "huhucapture microphone", properties, &events, &streams[index]);
                if (!streams[index].stream) { fail(errno, "Cannot create a requested audio capture stream"); break; }
                std::array<std::uint8_t, 1024> bytes{};
                spa_pod_builder builder{}; spa_pod_builder_init(&builder, bytes.data(), bytes.size());
                spa_audio_info_raw format{}; format.format = SPA_AUDIO_FORMAT_F32;
                format.rate = sampleRate; format.channels = channels;
                format.position[0] = SPA_AUDIO_CHANNEL_FL; format.position[1] = SPA_AUDIO_CHANNEL_FR;
                const spa_pod* parameters[]{spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
                const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                    PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT);
                const auto status = pw_stream_connect(streams[index].stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, parameters, 1);
                if (status < 0) { fail(status, "Cannot connect to a requested system or microphone device"); break; }
            }
            pw_loop_enter(loop);
            while (!stop.stop_requested()) {
                { std::lock_guard lock(mutex_); if (failure_ || finished_) break; }
                const auto status = pw_loop_iterate(loop, 50);
                if (status < 0 && status != -EINTR) { fail(status, "The audio capture event loop failed"); break; }
            }
            pw_loop_leave(loop);
            for (auto& stream : streams) if (stream.stream) pw_stream_destroy(stream.stream);
            pw_main_loop_destroy(main);
        }
        pw_deinit();
        { std::lock_guard lock(mutex_); finished_ = true; }
        changed_.notify_all();
    }
};
}

std::unique_ptr<Recording::AudioSource> makePipeWireAudio(bool systemAudio, bool microphone,
    std::uint64_t maximumQueuedBytes)
{
    if (!systemAudio && !microphone) return {};
    return std::make_unique<PipeWireAudio>(systemAudio, microphone, maximumQueuedBytes);
}

} // namespace HuhuCapture::Platform::Linux
