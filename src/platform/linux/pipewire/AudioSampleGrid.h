#pragma once

#include "core/MediaClock.h"
#include "recording/AudioSource.h"

#include <limits>
#include <optional>

namespace HuhuCapture::Platform::Linux {

using AudioSampleDuration = std::chrono::duration<std::int64_t,
    std::ratio<1, Recording::AudioSource::sampleRate>>;

// The requested sample is offset + ordinal/rate from a packet's first sample.
// floor(offset * rate + ordinal) == floor(offset * rate) + ordinal. Do not
// round ordinal/rate to native clock ticks and back: at 48 kHz that repeats
// sample 0 at ordinal 1 (20833 ns -> 0 samples).
inline std::optional<std::int64_t> audioSampleIndex(Core::MediaClock::duration offset,
    std::uint32_t ordinal) noexcept
{
    constexpr std::int64_t rate = Recording::AudioSource::sampleRate;
    constexpr auto minimum = (std::numeric_limits<std::int64_t>::min)();
    constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
    // Split before scaling so large clock counts cannot overflow the multiply
    // inside duration_cast. The remainder is strictly shorter than one second.
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(offset);
    if (seconds.count() > maximum / rate || seconds.count() < minimum / rate) return std::nullopt;
    const auto fraction = std::chrono::floor<AudioSampleDuration>(offset - seconds).count();
    const auto whole = seconds.count() * rate;
    if ((fraction > 0 && whole > maximum - fraction) ||
        (fraction < 0 && whole < minimum - fraction)) return std::nullopt;
    const auto base = whole + fraction;
    if (base > maximum - ordinal) return std::nullopt;
    return base + ordinal;
}

} // namespace HuhuCapture::Platform::Linux
