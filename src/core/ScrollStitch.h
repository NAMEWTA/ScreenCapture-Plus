#pragma once

#include "core/PixelBuffer.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ScreenCapture::Core {

// All images use tightly packed, top-down BGRA pixels. This layer owns no
// windows, device contexts or timers, so matching can be verified independently.
inline bool imageByteCount(int width, int height, std::size_t channels, std::size_t& bytes)
{
    const auto layout = pixelLayout(width, height, channels);
    bytes = layout ? layout->bytes : 0;
    return layout.has_value();
}

inline bool toGrayscale(std::span<const std::uint8_t> bgra, int width, int height,
                        std::vector<std::uint8_t>& gray)
{
    std::size_t bytes{};
    if (!imageByteCount(width, height, 4, bytes) || bgra.size() < bytes) return false;
    gray.resize(bytes / 4);
    for (std::size_t i = 0; i < gray.size(); ++i) {
        const auto* pixel = bgra.data() + i * 4;
        gray[i] = static_cast<std::uint8_t>((pixel[0] * 114 + pixel[1] * 587 + pixel[2] * 299) / 1000);
    }
    return true;
}

inline int findMostSimilarY(std::span<const std::uint8_t> previous, int previousHeight,
                            std::span<const std::uint8_t> current, int stripHeight, int width)
{
    std::size_t previousBytes{}, stripBytes{};
    if (!imageByteCount(width, previousHeight, 1, previousBytes) ||
        !imageByteCount(width, stripHeight, 1, stripBytes) ||
        previous.size() < previousBytes || current.size() < stripBytes || previousHeight < stripHeight) return 0;
    double bestError = std::numeric_limits<double>::max();
    int bestY = 0;
    for (int y = 0; y <= previousHeight - stripHeight; ++y) {
        double error = 0.0;
        for (int row = 0; row < stripHeight; ++row) {
            const auto oldOffset = static_cast<std::size_t>(y + row) * width;
            const auto newOffset = static_cast<std::size_t>(row) * width;
            for (int x = 0; x < width; ++x) {
                const int diff = previous[oldOffset + x] - current[newOffset + x];
                error += diff * diff;
            }
            // Squared error never decreases. A candidate that cannot beat the
            // current winner needs no more rows; ties still select the first y.
            if (error / stripHeight >= bestError) break;
        }
        const double average = error / stripHeight;
        if (average < bestError) {
            bestError = average;
            bestY = y;
            if (bestError == 0.0) break;
        }
    }
    return bestY;
}

inline int findScrollByBottomStrip(std::span<const std::uint8_t> previous,
                                   std::span<const std::uint8_t> current, int width, int stripHeight)
{
    std::size_t bytes{};
    if (!imageByteCount(width, stripHeight, 1, bytes) || previous.size() < bytes || current.size() < bytes) return 0;
    double bestError = std::numeric_limits<double>::max();
    double errorAtZero = bestError;
    int bestOffset = 0;
    for (int offset = 0; offset < stripHeight; ++offset) {
        const int rows = stripHeight - offset;
        double error = 0.0;
        for (int row = 0; row < rows; ++row) {
            const auto oldOffset = static_cast<std::size_t>(offset + row) * width;
            const auto newOffset = static_cast<std::size_t>(row) * width;
            for (int x = 0; x < width; ++x) {
                const int diff = previous[oldOffset + x] - current[newOffset + x];
                error += diff * diff;
            }
            if (offset != 0 && error / rows >= bestError) break;
        }
        const double average = error / rows;
        if (offset == 0) errorAtZero = average;
        if (average < bestError) {
            bestError = average;
            bestOffset = offset;
        }
    }
    // Keep the existing matching thresholds and row-normalized scoring exactly.
    if (bestOffset <= 0 || bestError >= errorAtZero * 0.9 || bestError > 40000.0) return 0;
    return bestOffset;
}

inline bool appendScrollFrame(std::vector<std::uint8_t>& output, int width, int& resultHeight,
                              std::span<const std::uint8_t> frame, int frameHeight,
                              int changeStartY, int scrollOffset)
{
    std::size_t originalBytes{}, frameBytes{}, resultBytes{};
    if (!imageByteCount(width, resultHeight, 4, originalBytes) || output.size() != originalBytes ||
        !imageByteCount(width, frameHeight, 4, frameBytes) || frame.size() != frameBytes ||
        frameHeight > resultHeight || changeStartY < 0 || changeStartY >= frameHeight ||
        scrollOffset <= 0 || scrollOffset > frameHeight - changeStartY ||
        resultHeight > std::numeric_limits<int>::max() - scrollOffset) return false;
    const int newHeight = resultHeight + scrollOffset;
    if (!imageByteCount(width, newHeight, 4, resultBytes)) return false;
    const auto stride = static_cast<std::size_t>(width) * 4;
    const auto destination = static_cast<std::size_t>(resultHeight - frameHeight + scrollOffset + changeStartY) * stride;
    const auto source = static_cast<std::size_t>(changeStartY) * stride;
    // Geometric vector capacity growth amortizes allocations. Only the incoming
    // frame's overlapping tail is written; the accumulated image is not recopied.
    output.resize(resultBytes);
    std::copy(frame.begin() + source, frame.end(), output.begin() + destination);
    resultHeight = newHeight;
    return true;
}

} // namespace ScreenCapture::Core
