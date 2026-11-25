#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <tuple>

namespace HuhuCapture::Capture {

// Exact WinCap percentage rounding, shared by the old capture and Qt sample.
inline std::tuple<int, int, int, int> cmyk(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    const double r = red / 255.0, g = green / 255.0, b = blue / 255.0;
    const double k = 1.0 - (std::max)(r, (std::max)(g, b));
    const double c = k == 1.0 ? 0.0 : (1.0 - r - k) / (1.0 - k);
    const double m = k == 1.0 ? 0.0 : (1.0 - g - k) / (1.0 - k);
    const double y = k == 1.0 ? 0.0 : (1.0 - b - k) / (1.0 - k);
    return {static_cast<int>(std::round(c * 100)), static_cast<int>(std::round(m * 100)),
        static_cast<int>(std::round(y * 100)), static_cast<int>(std::round(k * 100))};
}

} // namespace HuhuCapture::Capture
