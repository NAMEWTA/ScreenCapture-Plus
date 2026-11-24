#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ScreenCapture::Core {

struct AnnotationNote {
    std::uint64_t number;
    std::wstring note;
};

struct AnnotationColor {
    std::uint32_t backgroundRgb;
    std::uint32_t textRgb;
    constexpr bool operator==(const AnnotationColor&) const = default;
};

// The identifier, rather than a row's current position, owns its color. Removing,
// undoing or redoing another annotation therefore never changes an existing pin.
inline constexpr std::array<AnnotationColor, 10> annotationPalette{{
    {0x2563EB, 0xFFFFFF}, {0xFBBF24, 0x172033},
    {0x0F766E, 0xFFFFFF}, {0x7C3AED, 0xFFFFFF},
    {0xC2413A, 0xFFFFFF}, {0x0369A1, 0xFFFFFF},
    {0xBE185D, 0xFFFFFF}, {0x4D7C0F, 0xFFFFFF},
    {0xC05621, 0xFFFFFF}, {0x475569, 0xFFFFFF}
}};

inline AnnotationColor annotationColor(std::uint64_t number) noexcept
{
    if (number <= annotationPalette.size()) return annotationPalette[number == 0 ? 0 : number - 1];
    // Continue with deterministic golden-angle hue steps instead of repeating
    // the first ten colors. Integer modular arithmetic also handles large IDs.
    const auto index = number - annotationPalette.size() - 1;
    const double hue = static_cast<double>((index * 137508u) % 360000u) / 60000.0;
    const double saturation = 0.64 + static_cast<double>((index / 360u) % 3u) * 0.07;
    const double value = 0.72 + static_cast<double>((index / 1080u) % 3u) * 0.07;
    const double chroma = value * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(hue, 2.0) - 1.0));
    const double m = value - chroma;
    double r = 0, g = 0, b = 0;
    if (hue < 1) { r = chroma; g = x; }
    else if (hue < 2) { r = x; g = chroma; }
    else if (hue < 3) { g = chroma; b = x; }
    else if (hue < 4) { g = x; b = chroma; }
    else if (hue < 5) { r = x; b = chroma; }
    else { r = chroma; b = x; }
    const auto channel = [m](double value) { return static_cast<std::uint32_t>(std::lround((value + m) * 255.0)); };
    const auto red = channel(r), green = channel(g), blue = channel(b);
    const auto linear = [](std::uint32_t value) {
        const double s = static_cast<double>(value) / 255.0;
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue);
    return {(red << 16) | (green << 8) | blue, luminance > 0.179 ? 0x000000u : 0xFFFFFFu};
}

// Window-owned high water mark. It is deliberately independent of undo/redo and
// deletion; a new branch of history must not reuse a discarded note's identity.
class AnnotationSequence {
public:
    std::optional<std::uint64_t> allocate() noexcept
    {
        if (next_ == 0) return std::nullopt;
        const auto result = next_;
        next_ = result == (std::numeric_limits<std::uint64_t>::max)() ? 0 : result + 1;
        return result;
    }

    void observe(std::uint64_t number) noexcept
    {
        if (next_ != 0 && number >= next_)
            next_ = number == (std::numeric_limits<std::uint64_t>::max)() ? 0 : number + 1;
    }

private:
    std::uint64_t next_{1};
};

struct AnnotationLayoutOptions {
    std::uint32_t panelWidth{360};
    std::uint32_t minimumCanvasHeight{240};
    std::uint32_t maximumDimension{16384};
    std::size_t maximumBytes{256u * 1024u * 1024u};
    float preferredFontSize{18.0f};
    float minimumFontSize{12.0f};
    float headerHeight{64.0f};
    float padding{20.0f};
    float badgeDiameter{32.0f};
    float badgeGap{12.0f};
    float rowGap{16.0f};
};

struct AnnotationRow {
    std::size_t index;
    std::uint32_t column;
    float x, y, width, height;
};

struct AnnotationLayout {
    std::uint32_t width{}, height{}, imageWidth{}, imageHeight{}, panelWidth{}, columns{};
    float fontSize{}, textWidth{}, headerHeight{}, padding{}, badgeDiameter{}, badgeFontSize{}, badgeGap{}, rowGap{};
    std::vector<AnnotationRow> rows;
};

// Measure returns the complete wrapped ink height for note[index] at the given
// font size and text width, or nullopt on failure. Windows uses real DirectWrite
// metrics; the placement/overflow policy stays independently testable here.
// The result always contains every row in input order, or fails as a whole.
template<class Measure>
std::optional<AnnotationLayout> layoutAnnotations(
    std::uint32_t imageWidth, std::uint32_t imageHeight, std::size_t count,
    const AnnotationLayoutOptions& options, Measure&& measure)
{
    const auto maximum = options.maximumDimension;
    if (imageWidth == 0 || imageHeight == 0 || maximum == 0 ||
        maximum > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
        imageWidth > maximum || imageHeight > maximum || options.maximumBytes == 0)
        return std::nullopt;

    const auto fitsPixels = [&](std::uint32_t width, std::uint32_t height) {
        return width != 0 && height != 0 && width <= maximum && height <= maximum &&
            static_cast<std::uint64_t>(width) * height <= options.maximumBytes / 4;
    };
    if (count == 0) {
        if (!fitsPixels(imageWidth, imageHeight)) return std::nullopt;
        AnnotationLayout empty;
        empty.width = empty.imageWidth = imageWidth;
        empty.height = empty.imageHeight = imageHeight;
        return empty;
    }

    const auto finiteNonnegative = [](float value) { return std::isfinite(value) && value >= 0.0f; };
    if (!finiteNonnegative(options.preferredFontSize) || !finiteNonnegative(options.minimumFontSize) ||
        options.minimumFontSize <= 0.0f || options.preferredFontSize < options.minimumFontSize ||
        !finiteNonnegative(options.headerHeight) || !finiteNonnegative(options.padding) ||
        !finiteNonnegative(options.badgeDiameter) || options.badgeDiameter <= 0.0f ||
        !finiteNonnegative(options.badgeGap) || !finiteNonnegative(options.rowGap) ||
        options.panelWidth == 0 || options.panelWidth > maximum - imageWidth)
        return std::nullopt;

    float textWidth = static_cast<float>(options.panelWidth) -
        options.padding * 2.0f - options.badgeDiameter - options.badgeGap;
    const double top = std::ceil(static_cast<double>(options.headerHeight));
    const double bottom = std::ceil(static_cast<double>(options.padding));
    if (!std::isfinite(textWidth) || textWidth < 32.0f || top + bottom >= maximum)
        return std::nullopt;

    const std::uint32_t initialHeight = (std::min)(maximum,
        (std::max)(imageHeight, options.minimumCanvasHeight));
    const auto singleColumnBudgetHeight = static_cast<std::uint32_t>((std::min)(
        static_cast<std::size_t>(maximum), options.maximumBytes / 4 / (imageWidth + options.panelWidth)));
    if (singleColumnBudgetHeight < imageHeight || singleColumnBudgetHeight <= top + bottom)
        return std::nullopt;
    const auto fittingHeight = (std::min)(initialHeight, singleColumnBudgetHeight);
    std::vector<float> heights(count);
    float selectedSize = options.preferredFontSize;
    float badgeDiameter = options.badgeDiameter;
    float badgeFontSize = 16.0f;
    float rowGap = options.rowGap;
    double fullHeight = 0.0;
    // Candidate sizes are bounded by the caller's small preferred/minimum range.
    // Stop if subtraction cannot change a pathological floating-point input.
    for (;;) {
        const float ratio = selectedSize / options.preferredFontSize;
        // Compact short-note rows as well as long wrapped text. Numerals retain
        // a 12px floor, circles a 24px floor; measured wide identifiers scale by
        // the same factor as their numerals so every digit still fits.
        badgeFontSize = (std::max)(12.0f, 16.0f * ratio);
        badgeDiameter = std::ceil((std::max)(24.0f, options.badgeDiameter * badgeFontSize / 16.0f));
        rowGap = std::ceil((std::min)(options.rowGap, (std::max)(8.0f, options.rowGap * ratio)));
        textWidth = static_cast<float>(options.panelWidth) -
            options.padding * 2.0f - badgeDiameter - options.badgeGap;
        if (!std::isfinite(textWidth) || textWidth < 32.0f) return std::nullopt;
        fullHeight = top + bottom;
        for (std::size_t index = 0; index < count; ++index) {
            const auto measured = measure(index, selectedSize, textWidth);
            if (!measured || !finiteNonnegative(*measured)) return std::nullopt;
            const double rowHeight = std::ceil(static_cast<double>((std::max)(*measured, badgeDiameter)));
            heights[index] = static_cast<float>(rowHeight);
            fullHeight += rowHeight;
            if (index != 0) fullHeight += rowGap;
        }
        if (fullHeight <= fittingHeight || selectedSize <= options.minimumFontSize) break;
        const float nextSize = (std::max)(options.minimumFontSize, selectedSize - 1.0f);
        if (nextSize >= selectedSize) return std::nullopt;
        selectedSize = nextSize;
    }

    AnnotationLayout result;
    result.imageWidth = imageWidth;
    result.imageHeight = imageHeight;
    result.panelWidth = options.panelWidth;
    result.fontSize = selectedSize;
    result.textWidth = textWidth;
    result.headerHeight = static_cast<float>(top);
    result.padding = options.padding;
    result.badgeDiameter = badgeDiameter;
    result.badgeFontSize = badgeFontSize;
    result.badgeGap = options.badgeGap;
    result.rowGap = rowGap;

    // Prefer the original image height, then a taller single rail. If a GPU
    // dimension or the pixel budget prevents that, pack complete rows into more
    // right-side columns. A row is never clipped, split or silently omitted.
    const auto maximumColumns = (maximum - imageWidth) / options.panelWidth;
    for (std::uint32_t columns = 1; columns <= maximumColumns; ++columns) {
        const auto width = imageWidth + columns * options.panelWidth;
        const auto budgetHeight = options.maximumBytes / 4 / width;
        const auto availableHeight = static_cast<std::uint32_t>((std::min)(
            static_cast<std::size_t>(maximum), budgetHeight));
        if (availableHeight < imageHeight || availableHeight <= top + bottom) continue;
        const double requestedHeight = columns == 1
            ? (std::max)(static_cast<double>(fittingHeight), fullHeight)
            : static_cast<double>(availableHeight);
        if (columns == 1 && requestedHeight > availableHeight) continue;
        const double limit = columns == 1 ? requestedHeight : availableHeight;

        result.rows.clear();
        result.rows.reserve(count);
        std::uint32_t column = 0;
        double y = top;
        double usedHeight = top + bottom;
        bool fits = true;
        for (std::size_t index = 0; index < count; ++index) {
            const double height = heights[index];
            if (y + height + bottom > limit) {
                ++column;
                y = top;
            }
            if (column >= columns || y + height + bottom > limit) {
                fits = false;
                break;
            }
            result.rows.push_back({index, column,
                static_cast<float>(imageWidth + column * options.panelWidth) + options.padding,
                static_cast<float>(y), static_cast<float>(options.panelWidth) - options.padding * 2.0f,
                static_cast<float>(height)});
            usedHeight = (std::max)(usedHeight, y + height + bottom);
            y += height + rowGap;
        }
        if (!fits) continue;
        result.columns = column + 1;
        result.width = imageWidth + result.columns * options.panelWidth;
        result.height = static_cast<std::uint32_t>(std::ceil((std::max)(
            static_cast<double>(imageHeight), usedHeight)));
        // Small screenshots retain a comfortable rail when the budget allows it.
        if (initialHeight <= availableHeight) result.height = (std::max)(result.height, initialHeight);
        if (fitsPixels(result.width, result.height)) return result;
    }
    return std::nullopt;
}

} // namespace ScreenCapture::Core
