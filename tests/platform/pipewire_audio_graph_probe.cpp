#include "platform/linux/pipewire/PipeWireAudio.h"

#include <pipewire/pipewire.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace native = HuhuCapture::Platform::Linux;
namespace rec = HuhuCapture::Recording;
using Clock = HuhuCapture::Core::MediaClock;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
std::size_t descriptors()
{
    std::size_t count{};
    for (const auto& value : std::filesystem::directory_iterator("/proc/self/fd")) { static_cast<void>(value); ++count; }
    return count;
}
struct Samples {
    std::uint64_t frames{};
    std::array<double, 2> energy{};
    std::array<std::array<std::complex<double>, 2>, 2> frequency{};
    double peak{};
    void add(std::span<const float> pcm)
    {
        for (std::size_t frame = 0; frame < pcm.size() / 2; ++frame) {
            const auto at = static_cast<double>(frames + frame) / rec::AudioSource::sampleRate;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto value = pcm[frame * 2 + channel];
                require(std::isfinite(value) && std::abs(value) <= 1.00001f, "The real PCM mixer emitted nonfinite or unclamped samples");
                energy[channel] += value * value; peak = (std::max)(peak, static_cast<double>(std::abs(value)));
                for (std::size_t tone = 0; tone < 2; ++tone) {
                    const auto angle = 2 * std::numbers::pi * (tone ? 880.0 : 440.0) * at;
                    frequency[channel][tone] += static_cast<double>(value) * std::complex<double>{std::cos(angle), -std::sin(angle)};
                }
            }
        }
        frames += pcm.size() / 2;
    }
    double amplitude(std::size_t channel, std::size_t tone) const
    { return 2 * std::abs(frequency[channel][tone]) / static_cast<double>(frames); }
};
}

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "Provide system, microphone, mixed, clipped-mix, daemon-loss or source-loss");
        const std::string mode = argv[1];
        const bool fault = mode == "daemon-loss" || mode == "source-loss";
        const bool clip = mode == "clipped-mix";
        require(mode == "system" || mode == "microphone" || mode == "mixed" || clip || fault, "Unknown real audio graph probe mode");
        require(std::string_view(pw_get_library_version()) == "1.6.11" && std::string_view(pw_get_headers_version()) == "1.6.11",
            "Use the fixed actual PipeWire client ABI");
        const bool system = mode != "microphone", microphone = mode != "system";
        const auto before = descriptors();
        auto source = native::makePipeWireAudio(system, microphone);
        const auto ready = source->prepare(Clock::now() + 15s, {});
        if (!ready) throw std::runtime_error("Actual native audio preparation failed (" + std::to_string(ready.error) + "): " + ready.detail);
        std::cout << "{\"event\":\"prepared\",\"system\":" << (system ? "true" : "false")
            << ",\"microphone\":" << (microphone ? "true" : "false") << "}" << std::endl;
        // Give the owned real graph a short startup window. This moves the
        // requested recording origin; it neither shifts received timestamps
        // nor supplies PCM in place of a requested source.
        const auto origin = Clock::now() + 200ms;
        constexpr std::uint64_t target = 3ull * rec::AudioSource::sampleRate;
        std::array<float, 1024 * 2> pcm{};
        Samples samples;
        bool requestedFault{}, sawFault{};
        auto faultAt = Clock::time_point{};
        rec::AudioReadResult failure;
        while (samples.frames < target) {
            const auto count = static_cast<std::size_t>((std::min)(std::uint64_t{1024}, target - samples.frames));
            const auto begin = origin + std::chrono::nanoseconds(static_cast<std::int64_t>(samples.frames * 1'000'000'000 / rec::AudioSource::sampleRate));
            const auto read = source->read(begin, std::span(pcm).first(count * 2), Clock::now() + 2s, {});
            if (!read) {
                if (!fault || !requestedFault || read.cancelled || read.error == ETIMEDOUT || !read.error)
                    throw std::runtime_error("Actual native PCM read failed after " + std::to_string(samples.frames) + " frames (" +
                        std::to_string(read.error) + "): " + read.detail);
                failure = read; sawFault = true;
                require(Clock::now() - faultAt < 3s, "An actual lost graph/source must propagate without waiting indefinitely");
                break;
            }
            samples.add(std::span(pcm).first(count * 2));
            if (fault && !requestedFault && samples.frames >= rec::AudioSource::sampleRate / 2) {
                requestedFault = true; faultAt = Clock::now();
                std::cout << "{\"event\":\"fault-ready\",\"frames\":" << samples.frames << "}" << std::endl;
            }
        }
        const auto stopping = Clock::now(); source.reset();
        require(Clock::now() - stopping < 2s && descriptors() == before, "The actual audio streams and worker must release their descriptors promptly");
        if (fault) require(sawFault && samples.frames >= rec::AudioSource::sampleRate / 2,
            "The owned native failure must occur after actual nonempty capture and propagate as an error");
        else {
            require(samples.frames == target, "Read an exact continuous three-second native PCM interval");
            for (std::size_t channel = 0; channel < 2; ++channel) {
                require(std::sqrt(samples.energy[channel] / samples.frames) > 0.08,
                    "The requested native source cannot masquerade as a silent track");
                for (std::size_t tone = 0; tone < 2; ++tone) {
                    const bool selected = tone ? microphone : system;
                    const auto amplitude = samples.amplitude(channel, tone);
                    if (!selected) require(amplitude < 0.025, "An unselected source entered the actual PCM mix");
                    else if (!clip) require(amplitude > 0.16 && amplitude < 0.24,
                        "The actual source rate conversion/clock/mix must preserve its known tone amplitude");
                    else require(amplitude > 0.45 && amplitude < 0.85, "The clipped sum must retain both actual input tones");
                }
            }
            if (clip) require(samples.peak >= 0.9999 && samples.peak <= 1.00001, "The actual sum-and-limit mixer must clamp overlapping native inputs");
        }
        std::cout << "{\"event\":\"completed\",\"frames\":" << samples.frames << ",\"peak\":" << samples.peak
            << ",\"left440\":" << samples.amplitude(0, 0) << ",\"left880\":" << samples.amplitude(0, 1)
            << ",\"right440\":" << samples.amplitude(1, 0) << ",\"right880\":" << samples.amplitude(1, 1)
            << ",\"nativeError\":" << failure.error << ",\"descriptorDelta\":0,\"physicalDevices\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
