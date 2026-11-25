#pragma once

#include "core/AnnotationRegion.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace HuhuCapture::Capture {

using SelectionPoint = Core::AnnotationRegionPoint;
using SelectionBounds = Core::AnnotationRegionBounds;

enum class SelectionHit { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomRight, BottomLeft };

// Original-image pixels, with half-open image extents [0, width] x [0, height].
// This is the mature CutMask selection interaction, shared by the retained
// Windows capture and Qt. Editor boxes/annotations deliberately have different
// hit and resize rules and must not replace this model.
class SelectionModel final {
public:
    SelectionModel() noexcept = default;
    SelectionModel(float width, float height) noexcept { setExtent(width, height); }

    bool setExtent(float width, float height) noexcept
    {
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return false;
        width_ = width;
        height_ = height;
        bounds_ = {};
        candidates_.clear();
        finishGesture();
        return true;
    }

    float width() const noexcept { return width_; }
    float height() const noexcept { return height_; }
    const SelectionBounds& bounds() const noexcept { return bounds_; }
    bool hasSelection() const noexcept { return bounds_.right > bounds_.left && bounds_.bottom > bounds_.top; }
    bool gestureActive() const noexcept { return gesture_ != Gesture::None; }
    SelectionHit adjustment() const noexcept { return adjustHit_; }

    // The platform supplies front-to-back native window bounds in source
    // coordinates. Clip once, preserving order and avoiding allocations in hover.
    void setWindowCandidates(std::vector<SelectionBounds> candidates)
    {
        for (auto& candidate : candidates)
            candidate = finite(candidate) ? clipped(candidate) : SelectionBounds{};
        std::erase_if(candidates, [](const SelectionBounds& candidate) {
            return candidate.right <= candidate.left || candidate.bottom <= candidate.top;
        });
        candidates_ = std::move(candidates);
    }

    bool highlight(SelectionPoint point) noexcept
    {
        if (!finite(point)) return false;
        for (const auto& candidate : candidates_) {
            // Window boundaries themselves were never strict-interior hits.
            if (point.x > candidate.left && point.y > candidate.top &&
                point.x < candidate.right && point.y < candidate.bottom)
                return replace(candidate);
        }
        return false; // Preserve the last highlighted rectangle when none matches.
    }

    bool setSelection(SelectionBounds bounds) noexcept
    {
        if (!finite(bounds)) return false;
        bounds = clipped(bounds);
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return false;
        return replace(bounds);
    }

    bool beginCreation(SelectionPoint point) noexcept
    {
        if (!validExtent() || !finite(point)) return false;
        press_ = clamped(point);
        gesture_ = Gesture::Create;
        adjustHit_ = SelectionHit::None;
        // A click without movement accepts the existing window highlight.
        return true;
    }

    bool createTo(SelectionPoint point) noexcept
    {
        if (gesture_ != Gesture::Create || !finite(point)) return false;
        point = clamped(point);
        return replace({(std::min)(press_.x, point.x), (std::min)(press_.y, point.y),
            (std::max)(press_.x, point.x), (std::max)(press_.y, point.y)});
    }

    SelectionHit hitTest(SelectionPoint point) const noexcept
    {
        if (!hasSelection() || !finite(point)) return SelectionHit::None;
        // The four extended edges divide the whole canvas into nine regions.
        // An inclusive interior/border moves; anywhere outside resizes its side.
        const int column = point.x < bounds_.left ? 0 : (point.x > bounds_.right ? 2 : 1);
        const int row = point.y < bounds_.top ? 0 : (point.y > bounds_.bottom ? 2 : 1);
        constexpr SelectionHit grid[3][3]{
            {SelectionHit::TopLeft, SelectionHit::Top, SelectionHit::TopRight},
            {SelectionHit::Left, SelectionHit::Inside, SelectionHit::Right},
            {SelectionHit::BottomLeft, SelectionHit::Bottom, SelectionHit::BottomRight}
        };
        return grid[row][column];
    }

    bool beginAdjustment(SelectionPoint point) noexcept
    {
        adjustHit_ = hitTest(point);
        gesture_ = adjustHit_ == SelectionHit::None ? Gesture::None : Gesture::Adjust;
        if (gesture_ == Gesture::None) return false;
        start_ = bounds_;
        press_ = point;
        // Outside presses immediately snap the corresponding edge/corner.
        return adjustHit_ != SelectionHit::Inside && adjustTo(point);
    }

    bool adjustTo(SelectionPoint point) noexcept
    {
        if (gesture_ != Gesture::Adjust || !finite(point)) return false;
        auto next = start_;
        if (adjustHit_ == SelectionHit::Inside) {
            const float width = next.right - next.left, height = next.bottom - next.top;
            next.left = std::clamp(start_.left + point.x - press_.x, 0.f, width_ - width);
            next.top = std::clamp(start_.top + point.y - press_.y, 0.f, height_ - height);
            next.right = next.left + width;
            next.bottom = next.top + height;
        } else {
            point = clamped(point);
            using H = SelectionHit;
            const bool left = adjustHit_ == H::Left || adjustHit_ == H::TopLeft || adjustHit_ == H::BottomLeft;
            const bool right = adjustHit_ == H::Right || adjustHit_ == H::TopRight || adjustHit_ == H::BottomRight;
            const bool top = adjustHit_ == H::Top || adjustHit_ == H::TopLeft || adjustHit_ == H::TopRight;
            const bool bottom = adjustHit_ == H::Bottom || adjustHit_ == H::BottomLeft || adjustHit_ == H::BottomRight;
            if (left) next.left = point.x;
            if (right) next.right = point.x;
            if (top) next.top = point.y;
            if (bottom) next.bottom = point.y;
            if (next.left > next.right) std::swap(next.left, next.right);
            if (next.top > next.bottom) std::swap(next.top, next.bottom);
            minimumAxis(next.left, next.right, width_, left);
            minimumAxis(next.top, next.bottom, height_, top);
        }
        return replace(next);
    }

    void finishGesture() noexcept { gesture_ = Gesture::None; adjustHit_ = SelectionHit::None; }

private:
    enum class Gesture { None, Create, Adjust };
    static bool finite(SelectionPoint p) noexcept { return std::isfinite(p.x) && std::isfinite(p.y); }
    static bool finite(SelectionBounds r) noexcept
    { return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom); }
    bool validExtent() const noexcept { return width_ > 0 && height_ > 0; }
    SelectionPoint clamped(SelectionPoint p) const noexcept
    { return {std::clamp(p.x, 0.f, width_), std::clamp(p.y, 0.f, height_)}; }
    SelectionBounds clipped(SelectionBounds r) const noexcept
    { return {std::clamp(r.left, 0.f, width_), std::clamp(r.top, 0.f, height_),
        std::clamp(r.right, 0.f, width_), std::clamp(r.bottom, 0.f, height_)}; }
    bool replace(SelectionBounds next) noexcept
    {
        if (next.left == bounds_.left && next.top == bounds_.top &&
            next.right == bounds_.right && next.bottom == bounds_.bottom) return false;
        bounds_ = next;
        return true;
    }
    static void minimumAxis(float& begin, float& end, float extent, bool movingBegin) noexcept
    {
        const float minimum = (std::min)(4.f, extent);
        if (end - begin < minimum) {
            if (movingBegin) begin = end - minimum;
            else end = begin + minimum;
        }
        // CutMask's original minimum-size step could push an edge outside the
        // image. Fit the same extent back inside instead of passing a negative
        // or overshooting source rectangle to capture/export.
        if (begin < 0) { end -= begin; begin = 0; }
        if (end > extent) { begin -= end - extent; end = extent; }
    }

    float width_{}, height_{};
    SelectionBounds bounds_{}, start_{};
    SelectionPoint press_{};
    std::vector<SelectionBounds> candidates_;
    SelectionHit adjustHit_{SelectionHit::None};
    Gesture gesture_{Gesture::None};
};

} // namespace HuhuCapture::Capture
