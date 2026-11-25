#include "platform/linux/pipewire/PipeWireAudio.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
}

int main()
{
    try {
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
        std::cout << "NOT RUN: real monitor/microphone samples, mixing, hotplug or long-running audio/video drift\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
