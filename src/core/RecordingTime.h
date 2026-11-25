#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace HuhuCapture::Core {

struct RecordingTailTime {
    std::int64_t originalEnd{};
    std::int64_t targetEnd{};
};

// Keep the source's complete final interval. The target is the first rounded
// nominal-FPS boundary at or after it, in the same signed 100 ns media units.
// Only the still-owned, unsubmitted final sample may adopt this duration.
constexpr std::optional<RecordingTailTime> recordingTailTime(
    std::int64_t timestamp, std::int64_t duration, std::int32_t fps) noexcept
{
    constexpr std::int64_t ticksPerSecond = 10'000'000;
    constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
    if (timestamp < 0 || duration <= 0 || duration > maximum - timestamp ||
        fps <= 0 || fps > ticksPerSecond) return std::nullopt;

    const auto end = timestamp + duration;
    // Split whole seconds before multiplying: even INT64_MAX media times do
    // not need __int128, floating point, or an overflowing end * fps product.
    auto frame = (end / ticksPerSecond) * fps +
        ((end % ticksPerSecond) * fps) / ticksPerSecond;
    const auto boundary = [fps](std::int64_t index) -> std::optional<std::int64_t> {
        const auto seconds = index / fps;
        const auto fraction = ((index % fps) * ticksPerSecond + fps / 2) / fps;
        if (seconds > maximum / ticksPerSecond) return std::nullopt;
        const auto whole = seconds * ticksPerSecond;
        if (fraction > maximum - whole) return std::nullopt;
        return whole + fraction;
    };
    auto target = boundary(frame);
    if (!target) return std::nullopt;
    // A rounded-up fractional boundary may already equal the integer end.
    // Testing it first avoids needlessly adding a full frame in that case.
    if (*target < end) {
        if (frame == maximum) return std::nullopt;
        target = boundary(++frame);
        if (!target) return std::nullopt;
    }
    return RecordingTailTime{end, *target};
}

} // namespace HuhuCapture::Core
