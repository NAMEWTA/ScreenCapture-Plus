#pragma once

#include "core/PixelBuffer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace ScreenCapture::Core {

struct ImagePreviewSize {
    int width;
    int height;
};

// Sample the visible tail of a bottom-aligned, nearest-neighbour BGRA preview.
// Use the original float scaling/rounding so clipping changes no visible pixel.
// Source and destination must be separate buffers. Invalid input leaves the
// destination unchanged; allocation errors propagate to the UI fallback.
inline std::optional<ImagePreviewSize> makeImagePreview(
    std::span<const std::uint8_t> source, int sourceWidth, int sourceHeight,
    float targetWidth, int visibleHeight, int maximumBitmapSize,
    std::vector<std::uint8_t>& destination)
{
    const auto sourceLayout = pixelLayout(sourceWidth, sourceHeight, 4);
    if (!sourceLayout || source.size() < sourceLayout->bytes ||
        !std::isfinite(targetWidth) || targetWidth <= 0 ||
        visibleHeight <= 0 || maximumBitmapSize <= 0) return std::nullopt;

    const float scale = targetWidth / static_cast<float>(sourceWidth);
    const float scaledWidth = static_cast<float>(sourceWidth) * scale;
    const float scaledHeight = static_cast<float>(sourceHeight) * scale;
    constexpr double integerLimit = (std::numeric_limits<int>::max)();
    if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) ||
        scaledWidth > integerLimit || scaledHeight > integerLimit) return std::nullopt;

    const int width = static_cast<int>(scaledWidth);
    const int fullHeight = static_cast<int>(scaledHeight);
    const int height = std::min({ fullHeight, visibleHeight, maximumBitmapSize });
    const auto outputLayout = pixelLayout(width, height, 4);
    if (!outputLayout || width > maximumBitmapSize) return std::nullopt;

    const int firstRow = fullHeight - height;
    destination.resize(outputLayout->bytes);
    for (int y = 0; y < height; ++y) {
        // Division remains float; the double clamp only keeps the conversion to
        // int defined for unusually large dimensions near the integer limit.
        const int sourceY = static_cast<int>(std::min<double>(
            static_cast<float>(firstRow + y) / scale, sourceHeight - 1));
        for (int x = 0; x < width; ++x) {
            const int sourceX = static_cast<int>(std::min<double>(static_cast<float>(x) / scale, sourceWidth - 1));
            const auto sourceOffset = (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4;
            const auto destinationOffset = (static_cast<std::size_t>(y) * width + x) * 4;
            std::copy_n(source.data() + sourceOffset, 4, destination.data() + destinationOffset);
        }
    }
    return ImagePreviewSize{ width, height };
}

} // namespace ScreenCapture::Core
