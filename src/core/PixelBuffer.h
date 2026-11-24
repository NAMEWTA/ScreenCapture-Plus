#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace ScreenCapture::Core {

struct PixelLayout {
    std::size_t stride;
    std::size_t bytes;
};

// BI_RGB screen capture does not define the fourth byte. Normalize it once at
// the capture boundary before PREMULTIPLIED/WIC consumers or frame comparisons.
inline bool makeOpaque(std::span<std::uint8_t> bgra) noexcept
{
    if (bgra.size() % 4 != 0) return false;
    for (std::size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    return true;
}

// Check before multiplying dimensions from a window/selection or allocating a frame.
inline std::optional<PixelLayout> pixelLayout(int width, int height,
                                             std::size_t bytesPerPixel) noexcept
{
    if (width <= 0 || height <= 0 || bytesPerPixel == 0) return std::nullopt;
    constexpr auto limit = (std::numeric_limits<std::size_t>::max)();
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > limit / bytesPerPixel) return std::nullopt;
    const auto stride = w * bytesPerPixel;
    if (h > limit / stride) return std::nullopt;
    return PixelLayout{ stride, stride * h };
}

// Both buffers are top-down and tightly packed. Alpha is deliberately ignored:
// screen capture is opaque, while gifski/cgif expect RGB input.
inline bool bgraToRgb(std::span<const std::uint8_t> source,
                      std::span<std::uint8_t> destination,
                      int width, int height) noexcept
{
    const auto bgra = pixelLayout(width, height, 4);
    const auto rgb = pixelLayout(width, height, 3);
    if (!bgra || !rgb || source.size() < bgra->bytes || destination.size() < rgb->bytes)
        return false;
    for (std::size_t s = 0, d = 0; s < bgra->bytes; s += 4, d += 3) {
        destination[d] = source[s + 2];
        destination[d + 1] = source[s + 1];
        destination[d + 2] = source[s];
    }
    return true;
}

} // namespace ScreenCapture::Core
