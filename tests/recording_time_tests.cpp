#include "core/RecordingTime.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace core = HuhuCapture::Core;
namespace {
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
void expect(std::int64_t timestamp, std::int64_t duration, std::int32_t fps, std::int64_t target)
{
    const auto result = core::recordingTailTime(timestamp, duration, fps);
    require(result && result->originalEnd == timestamp + duration && result->targetEnd == target,
        "The original final interval must round upward to the expected CFR boundary");
}
}

int main()
{
    try {
        // Actual failed Windows interval: keep its full 747.8781 ms and extend
        // the last real image to the next 30 fps boundary, never round down.
        expect(7'028'074, 450'707, 30, 7'666'667);
        expect(7'113'304, 481'283, 30, 7'666'667);
        expect(6'918'132, 347'374, 30, 7'333'333);
        // PR #6 actual graceful-shutdown input endpoint: 717.2215 ms.
        expect(0, 7'172'215, 30, 7'333'333);
        expect(0, 1, 30, 333'333);
        expect(0, 333'333, 30, 333'333);
        expect(0, 333'334, 30, 666'667);
        expect(0, 666'666, 30, 666'667);
        expect(0, 666'667, 30, 666'667);
        expect(0, 666'668, 30, 1'000'000);
        expect(0, 166'667, 60, 166'667);
        expect(0, 166'668, 60, 333'333);
        expect(0, 333'333, 60, 333'333);
        expect(0, 333'334, 60, 500'000);
        expect(0, 39'062, 256, 39'063);
        expect(0, 39'063, 256, 39'063);
        expect(0, 39'064, 256, 78'125);
        expect(72'000'000'000 - 1, 1, 60, 72'000'000'000);

        require(!core::recordingTailTime(-1, 1, 30) && !core::recordingTailTime(0, -1, 30) &&
            !core::recordingTailTime(0, 0, 30) && !core::recordingTailTime(0, 1, 0) &&
            !core::recordingTailTime(0, 1, -1) && !core::recordingTailTime(0, 1, 10'000'001),
            "Invalid intervals and sub-tick frame periods must be refused");
        constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
        require(!core::recordingTailTime(maximum, 1, 30) &&
            !core::recordingTailTime(maximum - 1, 2, 60) &&
            !core::recordingTailTime(maximum - 1, 1, 30),
            "Input and rounded-boundary overflow must fail instead of wrapping");
        expect(maximum - 1, 1, 10'000'000, maximum);
        constexpr auto lastWholeSecond = maximum / 10'000'000 * 10'000'000;
        expect(lastWholeSecond - 1, 1, 1, lastWholeSecond);
        expect(lastWholeSecond - 1, 1, 240, lastWholeSecond);

        // Independently enumerate small exact rational boundaries, including
        // both sides of rounding and second crossings for varied nominal FPS.
        for (const auto fps : std::array{1, 7, 24, 25, 30, 59, 60, 120, 144, 240}) {
            std::int64_t previous{};
            for (std::int64_t index = 1; index <= fps * 3; ++index) {
                const auto current = (index * 10'000'000 + fps / 2) / fps;
                for (const auto end : {previous + 1, current - 1, current}) {
                    const auto result = core::recordingTailTime(0, end, fps);
                    require(result && result->originalEnd == end && result->targetEnd == current &&
                        previous < result->originalEnd && result->originalEnd <= result->targetEnd &&
                        result->targetEnd - result->originalEnd < current - previous,
                        "Choose the first rounded boundary and extend by less than one frame");
                }
                previous = current;
            }
        }
        std::cout << "PASS recording tail intervals, fractional CFR boundaries, minimal rounding and checked integer limits\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
