#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace HuhuCapture::UI {

// Logical pixels throughout. Ling applies the current monitor DPI once.
inline constexpr float toolbarButtonSize = 44.f;
inline constexpr float toolbarIconSize = 24.f;
inline constexpr float toolbarBorder = 1.f;
inline constexpr float toolbarDivider = 1.f;
inline constexpr float toolbarDividerHeight = 24.f;
inline constexpr float toolbarUnlimitedWidth = (std::numeric_limits<float>::max)();

struct ToolbarExtent {
    float width{};
    float height{};
    std::size_t rows{};
};

// Matches a wrapping row of fixed-width, non-shrinking controls. The window
// includes its border so every button keeps the full 44-pixel hit target.
// A control wider than the work area keeps its width; callers clamp position.
inline ToolbarExtent measureToolbar(std::span<const float> widths,
    float maximumWidth = toolbarUnlimitedWidth, float extraTop = 0.f)
{
    if (widths.empty()) return {};
    const auto widest = *std::max_element(widths.begin(), widths.end());
    const auto contentLimit = std::max(widest, maximumWidth - 2.f * toolbarBorder);
    float rowWidth = 0.f;
    float measuredWidth = 0.f;
    std::size_t rows = 1;
    for (const auto width : widths) {
        if (rowWidth > 0.f && rowWidth + width > contentLimit) {
            measuredWidth = std::max(measuredWidth, rowWidth);
            rowWidth = 0.f;
            ++rows;
        }
        rowWidth += width;
    }
    measuredWidth = std::max(measuredWidth, rowWidth);
    return {
        measuredWidth + 2.f * toolbarBorder,
        static_cast<float>(rows) * toolbarButtonSize + 2.f * toolbarBorder + extraTop,
        rows
    };
}

// WinBase truncates native window dimensions. Round outward before passing
// logical sizes back to it, including at fractional scale factors such as 125%.
inline float toolbarPixelAligned(float logical, float dpi)
{
    return std::ceil(logical * dpi) / dpi;
}

} // namespace HuhuCapture::UI
