#pragma once

#include "core/MediaFrame.h"
#include "recording/RecordingBackend.h"

#include <limits>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace HuhuCapture::Recording {

// Apply source crop, then its documented counter-clockwise transform. This is
// source-pixel geometry only; it never turns compositor positions into desktop
// coordinates. Consumers select a rectangle in this normalized pixel space.
inline std::optional<std::pair<int, int>> normalizedFrameSize(const Core::FrameLease& frame) noexcept
{
    if (!frame || !Core::validFrameMetadata(frame.metadata(), frame.format())) return {};
    const auto crop = frame.metadata().crop.value_or(Core::FrameCrop{0, 0,
        static_cast<std::uint32_t>(frame.format().width), static_cast<std::uint32_t>(frame.format().height)});
    const auto rotation = static_cast<unsigned>(frame.metadata().transform) % 4;
    return rotation % 2 ? std::pair{static_cast<int>(crop.height), static_cast<int>(crop.width)} :
        std::pair{static_cast<int>(crop.width), static_cast<int>(crop.height)};
}

// Converts the already-validated 8-bit screen formats to opaque BGRA without
// allocating. Negative/padded source strides use FrameLease::row bounds. HDR,
// alpha-bearing sources or a changed selection geometry fail explicitly.
inline bool copyFrameBgra(const Core::FrameLease& frame, PixelRegion selection,
    std::span<std::uint8_t> output, std::size_t outputStride) noexcept
{
    using namespace Core;
    const auto normalized = normalizedFrameSize(frame);
    if (!normalized || selection.x < 0 || selection.y < 0 || selection.width <= 0 || selection.height <= 0 ||
        std::int64_t{selection.x} + selection.width > normalized->first ||
        std::int64_t{selection.y} + selection.height > normalized->second) return false;
    const auto& format = frame.format();
    if ((format.pixelFormat != FramePixelFormat::Bgra8 && format.pixelFormat != FramePixelFormat::Rgba8) ||
        format.alpha != FrameAlpha::Opaque || format.color.transfer == ColorTransfer::Pq ||
        format.color.transfer == ColorTransfer::Hlg) return false;
    const auto rowBytes = static_cast<std::size_t>(selection.width) * 4;
    const auto rows = static_cast<std::size_t>(selection.height);
    if (outputStride < rowBytes || (rows > 1 && outputStride >
            ((std::numeric_limits<std::size_t>::max)() - rowBytes) / (rows - 1)) ||
        output.size() < (rows - 1) * outputStride + rowBytes) return false;
    const auto crop = frame.metadata().crop.value_or(FrameCrop{0, 0,
        static_cast<std::uint32_t>(format.width), static_cast<std::uint32_t>(format.height)});
    const auto transform = static_cast<unsigned>(frame.metadata().transform);
    const auto rotation = transform % 4;
    const bool reflected = transform >= 4;
    const bool rgba = format.pixelFormat == FramePixelFormat::Rgba8;
    if (transform == 0) {
        for (int y = 0; y < selection.height; ++y) {
            const auto source = frame.row(0, crop.y + static_cast<std::uint32_t>(selection.y + y));
            const auto first = static_cast<std::size_t>(crop.x + selection.x) * 4;
            if (source.size() < first + rowBytes) return false;
            auto* target = output.data() + static_cast<std::size_t>(y) * outputStride;
            const auto* pixel = source.data() + first;
            if (!rgba) {
                std::memcpy(target, pixel, rowBytes);
                for (int x = 0; x < selection.width; ++x) target[x * 4 + 3] = 255;
            } else {
                for (int x = 0; x < selection.width; ++x) {
                    target[x * 4] = pixel[x * 4 + 2]; target[x * 4 + 1] = pixel[x * 4 + 1];
                    target[x * 4 + 2] = pixel[x * 4]; target[x * 4 + 3] = 255;
                }
            }
        }
        return true;
    }
    for (int y = 0; y < selection.height; ++y) {
        auto* target = output.data() + static_cast<std::size_t>(y) * outputStride;
        for (int x = 0; x < selection.width; ++x) {
            const auto nx = static_cast<std::uint32_t>(selection.x + x);
            const auto ny = static_cast<std::uint32_t>(selection.y + y);
            std::uint32_t sx{}, sy{};
            switch (rotation) {
            case 0: sx = nx; sy = ny; break;
            case 1: sx = crop.width - 1 - ny; sy = nx; break;
            case 2: sx = crop.width - 1 - nx; sy = crop.height - 1 - ny; break;
            case 3: sx = ny; sy = crop.height - 1 - nx; break;
            }
            if (reflected) sx = crop.width - 1 - sx;
            sx += crop.x; sy += crop.y;
            const auto source = frame.row(0, sy);
            if (source.size() < (static_cast<std::size_t>(sx) + 1) * 4) return false;
            const auto* pixel = source.data() + static_cast<std::size_t>(sx) * 4;
            target[x * 4] = pixel[rgba ? 2 : 0];
            target[x * 4 + 1] = pixel[1];
            target[x * 4 + 2] = pixel[rgba ? 0 : 2];
            target[x * 4 + 3] = 255;
        }
    }
    return true;
}

} // namespace HuhuCapture::Recording
