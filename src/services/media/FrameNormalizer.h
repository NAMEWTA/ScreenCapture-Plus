#pragma once

#include "core/MediaFrame.h"
#include <optional>

namespace HuhuCapture::Media {

struct FrameExtent { int width{}, height{}; bool operator==(const FrameExtent&) const = default; };
// Native crop first, then its documented transform. A later user selection is
// expressed in this displayed source extent, not in compositor coordinates.
std::optional<FrameExtent> normalizedFrameExtent(const Core::FrameFormat& format,
    const Core::FrameMetadata& metadata) noexcept;

enum class FrameNormalizationError { None, InvalidFrame, UnsupportedFormat, InvalidSelection, Budget, Busy, Allocation };
// Copy into caller-owned storage using the same crop/transform traversal as
// the pooled normalizer. The span and stride are validated before any write;
// no intermediate full-image allocation or alpha/color reinterpretation occurs.
FrameNormalizationError copyNormalizedBgra(const Core::FrameLease& source,
    std::optional<Core::FrameCrop> selection, std::span<std::uint8_t> destination,
    std::size_t destinationStride) noexcept;

struct NormalizedFrame {
    Core::FrameLease frame;
    FrameNormalizationError error{FrameNormalizationError::None};
};

// Worker-owned normalization of the actual BGRA/RGBA CPU screen formats.
// There is no Qt or native capture object here. The returned BGRA lease has its
// crop/rotation applied, source identity/timing preserved, and unchanged color
// and alpha semantics. This is not HDR tone mapping or a gamut conversion.
// Unknown/unsupported pixel formats are rejected rather than guessed.
class FrameNormalizer final {
public:
    explicit FrameNormalizer(Core::FramePoolLimits limits = {3, 128u * 1024u * 1024u, 16384});
    NormalizedFrame normalize(const Core::FrameLease& source,
        std::optional<Core::FrameCrop> selection = {}) noexcept;
    std::size_t allocatedBytes() const noexcept { return pool_.allocatedBytes(); }
private:
    Core::FramePool pool_;
    std::optional<Core::FrameFormat> format_;
};

} // namespace HuhuCapture::Media
