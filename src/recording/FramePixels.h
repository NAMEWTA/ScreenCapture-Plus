#pragma once

#include "recording/RecordingBackend.h"
#include "services/media/FrameNormalizer.h"

#include <optional>
#include <span>
#include <utility>

namespace HuhuCapture::Recording {

// Compatibility seam for capture previews, scrolling and recording. Crop and
// all eight native transforms use the single shared normalizer implementation.
inline std::optional<std::pair<int, int>> normalizedFrameSize(const Core::FrameLease& frame) noexcept
{
    if (!frame) return {};
    const auto extent = Media::normalizedFrameExtent(frame.format(), frame.metadata());
    if (!extent) return {};
    return std::pair{extent->width, extent->height};
}

// Recording selects opaque SDR pixels; the generic normalizer itself preserves
// alpha and color semantics for other consumers. No extra complete frame copy
// or source-to-desktop position inference is introduced by this wrapper.
inline bool copyFrameBgra(const Core::FrameLease& frame, PixelRegion selection,
    std::span<std::uint8_t> output, std::size_t outputStride) noexcept
{
    if (!frame || selection.x < 0 || selection.y < 0 || selection.width <= 0 || selection.height <= 0)
        return false;
    const auto& format = frame.format();
    if (format.alpha != Core::FrameAlpha::Opaque || format.color.transfer == Core::ColorTransfer::Pq ||
        format.color.transfer == Core::ColorTransfer::Hlg) return false;
    return Media::copyNormalizedBgra(frame,
        Core::FrameCrop{static_cast<std::uint32_t>(selection.x), static_cast<std::uint32_t>(selection.y),
            static_cast<std::uint32_t>(selection.width), static_cast<std::uint32_t>(selection.height)},
        output, outputStride) == Media::FrameNormalizationError::None;
}

} // namespace HuhuCapture::Recording
