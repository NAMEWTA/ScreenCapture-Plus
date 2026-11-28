#pragma once

#include "core/MediaClock.h"

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>

namespace HuhuCapture::Recording {

struct AudioReadResult {
    std::int64_t error{};
    std::string detail;
    bool cancelled{};
    explicit operator bool() const noexcept { return error == 0 && !cancelled; }
};

// Worker-owned PCM source. The concrete adapter negotiates/resamples to stereo
// interleaved float at 48 kHz. Both requested sources must become ready; a
// missing/failed microphone or monitor cannot silently become a silent track.
class AudioSource {
public:
    static constexpr std::uint32_t sampleRate = 48000;
    static constexpr std::uint32_t channels = 2;
    virtual ~AudioSource() = default;
    virtual AudioReadResult prepare(Core::MediaClock::time_point deadline, std::stop_token stop) = 0;
    // Mix exactly output.size()/2 frames on the same steady-clock timeline as
    // captured video. The caller bounds its wait and never invokes this on Qt.
    virtual AudioReadResult read(Core::MediaClock::time_point begin, std::span<float> output,
        Core::MediaClock::time_point deadline, std::stop_token stop) = 0;
};

} // namespace HuhuCapture::Recording
