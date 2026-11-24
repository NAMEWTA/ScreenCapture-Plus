#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace HuhuCapture::Core {

// Geometry is expressed in original-image pixels. The image boundary is the
// half-open pixel extent [0, width] x [0, height], not the last pixel's center.
struct AnnotationRegionPoint { float x{}, y{}; };
struct AnnotationRegionBounds { float left{}, top{}, right{}, bottom{}; };

enum class AnnotationRegionHandle {
    None = -1, TopLeft, Top, TopRight, Right, BottomRight, Bottom,
    BottomLeft, Left, Move
};

inline AnnotationRegionPoint clampAnnotationPoint(AnnotationRegionPoint point,
    float width, float height) noexcept
{
    return {std::clamp(point.x, 0.f, (std::max)(0.f, width)),
        std::clamp(point.y, 0.f, (std::max)(0.f, height))};
}

inline AnnotationRegionBounds annotationRegionFromDrag(AnnotationRegionPoint start,
    AnnotationRegionPoint end, float width, float height) noexcept
{
    start = clampAnnotationPoint(start, width, height);
    end = clampAnnotationPoint(end, width, height);
    return {(std::min)(start.x, end.x), (std::min)(start.y, end.y),
        (std::max)(start.x, end.x), (std::max)(start.y, end.y)};
}

inline bool validAnnotationRegion(const AnnotationRegionBounds& region,
    float minimumExtent) noexcept
{
    return std::isfinite(region.left) && std::isfinite(region.top)
        && std::isfinite(region.right) && std::isfinite(region.bottom)
        && region.right > region.left && region.bottom > region.top
        && region.right - region.left >= minimumExtent
        && region.bottom - region.top >= minimumExtent;
}

inline float annotationRegionMinimumExtent(float markerScale) noexcept
{
    // Pointer positions arrive as integer source pixels. Rounding the minimum
    // outward keeps creation, resize and translation on that same grid, so a
    // smallest valid region stays valid when moved anywhere in the image.
    return (std::max)(1.f, std::ceil(3.f * markerScale));
}

// Clamp the translation as a whole. Clamping individual corners would shrink
// a region each time the pointer crosses an image boundary.
inline AnnotationRegionBounds moveAnnotationRegion(const AnnotationRegionBounds& origin,
    AnnotationRegionPoint delta, float width, float height) noexcept
{
    const float dx = std::clamp(delta.x, -origin.left, (std::max)(-origin.left, width - origin.right));
    const float dy = std::clamp(delta.y, -origin.top, (std::max)(-origin.top, height - origin.bottom));
    return {origin.left + dx, origin.top + dy, origin.right + dx, origin.bottom + dy};
}

// A resize keeps the opposite side fixed and stops at the minimum extent.
// Creation can run in any direction; resizing an existing object never turns
// it into an invalid zero-area shape or changes its identity.
inline AnnotationRegionBounds resizeAnnotationRegion(const AnnotationRegionBounds& origin,
    AnnotationRegionHandle handle, AnnotationRegionPoint point,
    float width, float height, float minimumExtent) noexcept
{
    auto region = origin;
    point = clampAnnotationPoint(point, width, height);
    const float minWidth = (std::min)(minimumExtent, origin.right - origin.left);
    const float minHeight = (std::min)(minimumExtent, origin.bottom - origin.top);
    using Handle = AnnotationRegionHandle;
    if (handle == Handle::TopLeft || handle == Handle::Left || handle == Handle::BottomLeft)
        region.left = (std::min)(point.x, origin.right - minWidth);
    if (handle == Handle::TopRight || handle == Handle::Right || handle == Handle::BottomRight)
        region.right = (std::max)(point.x, origin.left + minWidth);
    if (handle == Handle::TopLeft || handle == Handle::Top || handle == Handle::TopRight)
        region.top = (std::min)(point.y, origin.bottom - minHeight);
    if (handle == Handle::BottomLeft || handle == Handle::Bottom || handle == Handle::BottomRight)
        region.bottom = (std::max)(point.y, origin.top + minHeight);
    return region;
}

inline std::array<AnnotationRegionPoint, 8> annotationRegionHandles(
    const AnnotationRegionBounds& region) noexcept
{
    const float midX = (region.left + region.right) / 2.f;
    const float midY = (region.top + region.bottom) / 2.f;
    return {{{region.left, region.top}, {midX, region.top}, {region.right, region.top},
        {region.right, midY}, {region.right, region.bottom}, {midX, region.bottom},
        {region.left, region.bottom}, {region.left, midY}}};
}

inline AnnotationRegionHandle hitAnnotationRegion(const AnnotationRegionBounds& region,
    AnnotationRegionPoint point, float tolerance) noexcept
{
    const auto handles = annotationRegionHandles(region);
    // Corners take precedence over side midpoints on very small rectangles.
    constexpr std::array<std::size_t, 8> order{0, 2, 4, 6, 1, 3, 5, 7};
    for (const auto index : order) {
        if (std::abs(point.x - handles[index].x) <= tolerance
            && std::abs(point.y - handles[index].y) <= tolerance)
            return static_cast<AnnotationRegionHandle>(index);
    }
    if (point.x >= region.left - tolerance && point.x <= region.right + tolerance
        && point.y >= region.top - tolerance && point.y <= region.bottom + tolerance
        && (std::abs(point.x - region.left) <= tolerance
            || std::abs(point.x - region.right) <= tolerance
            || std::abs(point.y - region.top) <= tolerance
            || std::abs(point.y - region.bottom) <= tolerance))
        return AnnotationRegionHandle::Move;
    // The transparent interior is intentionally available for nested regions
    // and other tools. Only the border, handles and badge select this object.
    return AnnotationRegionHandle::None;
}

inline AnnotationRegionPoint annotationBadgeCenter(const AnnotationRegionBounds& region,
    float radius, float width, float height) noexcept
{
    const float radiusX = (std::min)(radius, width / 2.f);
    const float radiusY = (std::min)(radius, height / 2.f);
    return {std::clamp(region.left + radius, radiusX, width - radiusX),
        std::clamp(region.top + radius, radiusY, height - radiusY)};
}

} // namespace HuhuCapture::Core
