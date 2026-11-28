/* PipeWire audio-src.c at 4287f31510771fc999f8a58a024c81ca10b8d55f.
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 * SPDX-License-Identifier: MIT
 * Adapted only for an explicitly named, owned CI graph, two distinguishable
 * signals and checked buffer/stream cleanup. This is never a device substitute
 * in the production application. See third_party/pipewire-audio/upstream.json.
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string_view>

namespace {
struct Signal {
    pw_main_loop* loop{};
    pw_stream* stream{};
    float phase{};
    float frequency{};
    float volume{0.2f};
    unsigned rate{};
    std::atomic_int error{};
    std::atomic_uint64_t frames{};
    void fail(int value) noexcept { error.store(value ? value : EIO); pw_main_loop_quit(loop); }
};
void process(void* opaque) noexcept
{
    auto& signal = *static_cast<Signal*>(opaque);
    auto* buffer = pw_stream_dequeue_buffer(signal.stream);
    if (!buffer) return;
    struct Return { pw_stream* stream; pw_buffer* buffer; ~Return() { pw_stream_queue_buffer(stream, buffer); } } returned{signal.stream, buffer};
    if (!buffer->buffer || buffer->buffer->n_datas != 1) { signal.fail(EINVAL); return; }
    auto& plane = buffer->buffer->datas[0];
    constexpr unsigned stride = sizeof(float) * 2;
    if (!plane.data || !plane.chunk || plane.maxsize < stride) { signal.fail(EINVAL); return; }
    auto frames = plane.maxsize / stride;
    if (buffer->requested) frames = static_cast<unsigned>((std::min)(std::uint64_t{frames}, buffer->requested));
    if (!frames) { signal.fail(EINVAL); return; }
    auto* output = static_cast<float*>(plane.data);
    constexpr auto full = 2 * std::numbers::pi_v<float>;
    for (unsigned frame = 0; frame < frames; ++frame) {
        signal.phase += full * signal.frequency / signal.rate;
        if (signal.phase >= full) signal.phase -= full;
        const auto value = std::sin(signal.phase) * signal.volume;
        for (unsigned channel = 0; channel < 2; ++channel) *output++ = value;
    }
    plane.chunk->offset = 0; plane.chunk->stride = stride; plane.chunk->size = frames * stride; plane.chunk->flags = 0;
    buffer->size = frames;
    signal.frames.fetch_add(frames, std::memory_order_relaxed);
}
void stateChanged(void* opaque, pw_stream_state, pw_stream_state next, const char*) noexcept
{
    auto& signal = *static_cast<Signal*>(opaque);
    if (next == PW_STREAM_STATE_ERROR) signal.fail(EPIPE);
}
void quit(void* opaque, int) noexcept
{ pw_main_loop_quit(static_cast<Signal*>(opaque)->loop); }
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3 || (std::string_view(argv[1]) != "system" && std::string_view(argv[1]) != "microphone") ||
        (argc == 3 && std::string_view(argv[2]) != "--clip")) {
        std::cerr << "Use system|microphone [--clip] only inside the owned PipeWire test graph\n"; return 2;
    }
    const bool system = std::string_view(argv[1]) == "system";
    pw_init(nullptr, nullptr);
    Signal signal;
    signal.rate = system ? 44100 : 48000; // exercise the real graph rate conversion
    signal.frequency = system ? 440.0f : 880.0f;
    if (argc == 3) signal.volume = 0.8f;
    signal.loop = pw_main_loop_new(nullptr);
    if (!signal.loop) { pw_deinit(); return 1; }
    auto* loop = pw_main_loop_get_loop(signal.loop);
    auto* interrupt = pw_loop_add_signal(loop, SIGINT, quit, &signal);
    auto* terminate = pw_loop_add_signal(loop, SIGTERM, quit, &signal);
    auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_MEDIA_CLASS, system ? "Stream/Output/Audio" : "Audio/Source",
        PW_KEY_NODE_NAME, system ? "huhu-audio-tone-system" : "huhu-audio-tone-microphone",
        PW_KEY_NODE_GROUP, "pipewire.dummy", PW_KEY_NODE_ALWAYS_PROCESS, "true",
        "node.virtual", "true", nullptr);
    pw_stream_events events{}; events.version = PW_VERSION_STREAM_EVENTS;
    events.process = process; events.state_changed = stateChanged;
    if (properties) signal.stream = pw_stream_new_simple(loop, system ? "huhu system signal" : "huhu microphone signal",
        properties, &events, &signal);
    int status = -ENOMEM;
    if (signal.stream && interrupt && terminate) {
        std::uint8_t storage[1024]{}; spa_pod_builder builder{};
        spa_pod_builder_init(&builder, storage, sizeof(storage));
        spa_audio_info_raw format{}; format.format = SPA_AUDIO_FORMAT_F32;
        format.channels = 2; format.rate = signal.rate;
        format.position[0] = SPA_AUDIO_CHANNEL_FL; format.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* parameters[]{spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
        status = pw_stream_connect(signal.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), parameters, 1);
        if (status >= 0) {
            std::cout << "{\"event\":\"signal-created\",\"rate\":" << signal.rate << ",\"frequency\":" << signal.frequency << "}" << std::endl;
            status = pw_main_loop_run(signal.loop);
        }
    }
    if (signal.stream) pw_stream_destroy(signal.stream);
    if (interrupt) pw_loop_destroy_source(loop, interrupt);
    if (terminate) pw_loop_destroy_source(loop, terminate);
    pw_main_loop_destroy(signal.loop);
    pw_deinit();
    std::cout << "{\"event\":\"signal-stopped\",\"frames\":" << signal.frames.load() << ",\"error\":" << signal.error.load() << "}" << std::endl;
    return status < 0 || signal.error.load() ? 1 : 0;
}
