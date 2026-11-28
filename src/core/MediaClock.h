#pragma once

#include <chrono>
#include <cstdint>
#include <ratio>

namespace HuhuCapture::Core {

using MediaClock = std::chrono::steady_clock;
using MediaTime = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;

// Media Foundation timestamps are 100 ns, irrespective of the clock's native
// period. Callers use elapsed steady-clock durations, never wall-clock epochs.
template<class Rep, class Period>
constexpr std::int64_t mediaTicks(std::chrono::duration<Rep, Period> elapsed) noexcept
{
    return std::chrono::duration_cast<MediaTime>(elapsed).count();
}

} // namespace HuhuCapture::Core
