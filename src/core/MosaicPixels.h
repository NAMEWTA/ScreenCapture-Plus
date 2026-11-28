#pragma once

#include "core/PixelBuffer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace HuhuCapture::Core {

// Extracted from the production ShapeMosaic block-average implementation.
// All four bytes, including premultiplied alpha, are averaged independently.
// Padding is retained. Validate the entire borrowed layout before any write.
inline bool mosaicPixels(std::span<std::uint8_t> pixels, std::size_t stride,
    int width, int height, int blockSize) noexcept
{
    const auto layout = pixelLayout(width, height, 4);
    if (!layout || blockSize <= 0 || stride < layout->stride) return false;
    const auto rows = static_cast<std::size_t>(height);
    if (rows > (std::numeric_limits<std::size_t>::max)() / stride ||
        pixels.size() < rows * stride) return false;
    const auto w = static_cast<std::size_t>(width), h = rows;
    const auto block = static_cast<std::size_t>(blockSize);
    const auto maxColumns = (std::min)(w, block), maxRows = (std::min)(h, block);
    if (maxRows > (std::numeric_limits<std::uint64_t>::max)() / 255 / maxColumns)
        return false;
    for (std::size_t y = 0; y < h;) {
        const auto yEnd = y + (std::min)(block, h - y);
        for (std::size_t x = 0; x < w;) {
            const auto xEnd = x + (std::min)(block, w - x);
            std::array<std::uint64_t, 4> sums{};
            const auto count = static_cast<std::uint64_t>(yEnd - y) * (xEnd - x);
            for (auto yy = y; yy < yEnd; ++yy)
                for (auto xx = x; xx < xEnd; ++xx)
                    for (std::size_t channel = 0; channel < sums.size(); ++channel)
                        sums[channel] += pixels[yy * stride + xx * 4 + channel];
            for (auto yy = y; yy < yEnd; ++yy)
                for (auto xx = x; xx < xEnd; ++xx)
                    for (std::size_t channel = 0; channel < sums.size(); ++channel)
                        pixels[yy * stride + xx * 4 + channel] = static_cast<std::uint8_t>(sums[channel] / count);
            x = xEnd;
        }
        y = yEnd;
    }
    return true;
}

} // namespace HuhuCapture::Core
