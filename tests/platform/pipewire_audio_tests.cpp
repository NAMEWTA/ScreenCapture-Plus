#include "platform/linux/pipewire/PipeWireAudio.h"
#include "platform/linux/pipewire/AudioSampleGrid.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace linuxPlatform = HuhuCapture::Platform::Linux;
namespace core = HuhuCapture::Core;
using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* detail)
{ if (!condition) throw std::runtime_error(detail); }
std::size_t descriptors()
{
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) { static_cast<void>(entry); ++count; }
    return count;
}

void sampleGrid()
{
    using linuxPlatform::audioSampleIndex;
    for (std::uint32_t sample = 0; sample < 48'000; ++sample) {
        require(audioSampleIndex(core::MediaClock::duration::zero(), sample) == sample,
            "Every sample in an aligned second has its own exact source index, without repeated preceding PCM");
        require(audioSampleIndex(5ms, sample) == 240 + sample,
            "An integral source offset preserves all later sample positions");
    }
    require(audioSampleIndex(-1ns, 0) == -1 && audioSampleIndex(-1ns, 1) == 0,
        "A request before the first sample remains a real gap until its next sample time");
    require(audioSampleIndex(20'833ns, 0) == 0 && audioSampleIndex(20'834ns, 0) == 1,
        "Native packet offsets preserve the actual boundary on both sides of one 48 kHz sample");
    require(audioSampleIndex(-20'834ns, 0) == -2 && audioSampleIndex(-20'833ns, 0) == -1,
        "Negative packet offsets use floor rather than truncation toward zero");
    // A native timestamp is integral nanoseconds; sample ordinal itself must
    // remain exact even across packets whose start is a truncated rational ns.
    for (std::uint32_t block = 0; block < 12; ++block) {
        const auto first = block * 4096;
        const auto nativeStart = std::chrono::duration_cast<core::MediaClock::duration>(
            linuxPlatform::AudioSampleDuration{first});
        for (std::uint32_t sample = 0; sample < 4096; ++sample)
            require(audioSampleIndex(-nativeStart, first + sample) == sample,
                "Native packet boundary rounding must not repeat or skip an intra-packet sample");
    }
    const auto largest = audioSampleIndex((core::MediaClock::duration::max)(), 0);
    const auto smallest = audioSampleIndex((core::MediaClock::duration::min)(), 0);
    require(largest && *largest > 0 && smallest && *smallest < 0,
        "Scaling extreme native clock durations must not wrap before the checked sample conversion");
}
}

int main()
{
    try {
        sampleGrid();
        std::array<char, 64> directory{};
        const std::string pattern = "/tmp/huhucapture-audio-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), directory.begin());
        require(mkdtemp(directory.data()) != nullptr, "Create a private real native-audio test runtime directory");
        struct Remove { std::filesystem::path path; ~Remove() { std::error_code error; std::filesystem::remove(path, error); } } remove{directory.data()};
        require(setenv("XDG_RUNTIME_DIR", directory.data(), 1) == 0 &&
            setenv("PIPEWIRE_RUNTIME_DIR", directory.data(), 1) == 0 &&
            setenv("PIPEWIRE_REMOTE", "absent-huhucapture-test-server", 1) == 0,
            "Select a real missing private PipeWire endpoint without touching a desktop server");
        require(!linuxPlatform::makePipeWireAudio(false, false), "Silent recording allocates no native audio source");
        const auto before = descriptors();
        {
            auto bounded = linuxPlatform::makePipeWireAudio(true, true, 1);
            const auto result = bounded->prepare(core::MediaClock::now() + 1s, {});
            require(!result && result.error == ENOBUFS,
                "A budget too small for the selected queues fails before device capture");
        }
        for (const auto& enabled : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
            for (unsigned repeat = 0; repeat < 3; ++repeat) {
                auto audio = linuxPlatform::makePipeWireAudio(enabled.first, enabled.second);
                require(bool(audio), "Requested native audio has an owned adapter");
                const auto start = core::MediaClock::now();
                const auto ready = audio->prepare(start + 1s, {});
                require(!ready && ready.error && !ready.detail.empty(),
                    "An actual unavailable PipeWire daemon cannot become a ready silent audio track");
                audio.reset();
                require(core::MediaClock::now() - start < 2s, "Failed native preparation stops and joins its worker within the deadline");
            }
        }
        {
            auto audio = linuxPlatform::makePipeWireAudio(true, true);
            std::stop_source stop; stop.request_stop();
            const auto result = audio->prepare(core::MediaClock::now() + 1s, stop.get_token());
            require(result.cancelled && !result.error, "Cancellation before device preparation starts no native stream");
        }
        require(descriptors() == before, "Repeated native failure and cancellation release every capture descriptor");
        std::cout << "PASS real PipeWire missing-endpoint preparation, three requested-source combinations, cancellation, bounded join and descriptor release\n";
        std::cout << "PASS exact 48 kHz sample positions, native packet boundaries, signed gaps and clock-range arithmetic\n";
        std::cout << "NOT RUN: real monitor/microphone samples, mixing, hotplug or long-running audio/video drift\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
