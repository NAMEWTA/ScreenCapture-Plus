#pragma once

#include "editor/Geometry.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace HuhuCapture::Editor {

enum class BoxKind { Rectangle, Ellipse };

// Shared original-image geometry for rectangle/ellipse and rectangular masks.
// Unlike annotation regions, these tools historically allow crossing the fixed
// edge and drawing outside the image. The rasterizer clips to the source canvas;
// reusing annotation resize clamping here would change the existing interaction.
class BoxModel {
public:
    explicit BoxModel(BoxKind kind = BoxKind::Rectangle) noexcept : kind_(kind) {}

    const EditorBounds& bounds() const noexcept { return bounds_; }
    BoxKind kind() const noexcept { return kind_; }
    bool placed() const noexcept { return placed_; }
    bool gestureActive() const noexcept { return active_ != EditorHandle::None; }
    EditorPoint center() const noexcept
    {
        return {(bounds_.left + bounds_.right) / 2, (bounds_.top + bounds_.bottom) / 2};
    }
    EditorPoint radii() const noexcept
    {
        return {(bounds_.right - bounds_.left) / 2, (bounds_.bottom - bounds_.top) / 2};
    }

    std::array<EditorBounds, 8> handles(float size) const noexcept
    {
        const auto centers = Core::annotationRegionHandles(bounds_);
        std::array<EditorBounds, 8> result;
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = editorHandleBounds(centers[index], size);
        return result;
    }

    EditorHandle hitTest(EditorPoint point, float handleSize, float edgeHalfWidth) const noexcept
    {
        if (!placed_ || !std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(handleSize) || handleSize < 0 ||
            !std::isfinite(edgeHalfWidth) || edgeHalfWidth <= 0)
            return EditorHandle::None;
        // Preserve the old 0..7 handle priority. Annotation's corner-first rule
        // is intentionally different on very small overlapping handle boxes.
        const auto boxes = handles(handleSize);
        for (std::size_t index = 0; index < boxes.size(); ++index)
            if (containsEditorPoint(boxes[index], point)) return static_cast<EditorHandle>(index);
        if (kind_ == BoxKind::Rectangle) {
            if (point.x >= bounds_.left - edgeHalfWidth && point.x <= bounds_.right + edgeHalfWidth &&
                point.y >= bounds_.top - edgeHalfWidth && point.y <= bounds_.bottom + edgeHalfWidth &&
                (point.x <= bounds_.left + edgeHalfWidth || point.x >= bounds_.right - edgeHalfWidth ||
                 point.y <= bounds_.top + edgeHalfWidth || point.y >= bounds_.bottom - edgeHalfWidth))
                return EditorHandle::Move;
        } else {
            const auto c = center(), r = radii();
            const float dx = point.x - c.x, dy = point.y - c.y;
            const float outerX = r.x + edgeHalfWidth, outerY = r.y + edgeHalfWidth;
            const float innerX = (std::max)(0.0f, r.x - edgeHalfWidth);
            const float innerY = (std::max)(0.0f, r.y - edgeHalfWidth);
            const bool outer = (dx / outerX) * (dx / outerX) + (dy / outerY) * (dy / outerY) <= 1;
            const bool inner = innerX > 0 && innerY > 0 &&
                (dx / innerX) * (dx / innerX) + (dy / innerY) * (dy / innerY) <= 1;
            if (outer && !inner) return EditorHandle::Move;
        }
        return EditorHandle::None;
    }

    EditorHandle beginGesture(EditorPoint point, EditorHandle handle = EditorHandle::None) noexcept
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return EditorHandle::None;
        if (!placed_) {
            // A zero-size press is still discarded by the owning document. Give
            // interim paints its actual location instead of the image origin.
            bounds_ = {point.x, point.y, point.x, point.y};
            placed_ = true;
            press_ = point;
            active_ = EditorHandle::BottomRight;
            return active_;
        }
        active_ = handle;
        using H = EditorHandle;
        switch (handle) {
        case H::TopLeft: press_ = {bounds_.right, bounds_.bottom}; break;
        case H::Top: press_.y = bounds_.bottom; break;
        case H::TopRight: press_ = {bounds_.left, bounds_.bottom}; break;
        case H::Right: press_.x = bounds_.left; break;
        case H::BottomRight: press_ = {bounds_.left, bounds_.top}; break;
        case H::Bottom: press_.y = bounds_.top; break;
        case H::BottomLeft: press_ = {bounds_.right, bounds_.top}; break;
        case H::Left: press_.x = bounds_.right; break;
        case H::Move: press_ = {point.x - bounds_.left, point.y - bounds_.top}; break;
        default: active_ = H::None; break;
        }
        return active_;
    }

    void drag(EditorPoint point, bool square) noexcept
    {
        if (!placed_ || !gestureActive() || !std::isfinite(point.x) || !std::isfinite(point.y)) return;
        auto next = bounds_;
        using H = EditorHandle;
        if (active_ == H::TopLeft || active_ == H::TopRight || active_ == H::BottomRight || active_ == H::BottomLeft) {
            next = {(std::min)(press_.x, point.x), (std::min)(press_.y, point.y),
                (std::max)(press_.x, point.x), (std::max)(press_.y, point.y)};
        } else if (active_ == H::Top || active_ == H::Bottom) {
            next.top = (std::min)(press_.y, point.y);
            next.bottom = (std::max)(press_.y, point.y);
        } else if (active_ == H::Right || active_ == H::Left) {
            next.left = (std::min)(press_.x, point.x);
            next.right = (std::max)(press_.x, point.x);
        } else if (active_ == H::Move) {
            next.left = point.x - press_.x;
            next.top = point.y - press_.y;
            next.right = next.left + (bounds_.right - bounds_.left);
            next.bottom = next.top + (bounds_.bottom - bounds_.top);
        }
        // This deliberately expands from the normalized top-left, matching the
        // mature tools even when dragging left/up or crossing the opposite side.
        if (active_ != H::Move && square) {
            const float extent = (std::max)(next.right - next.left, next.bottom - next.top);
            next.right = next.left + extent;
            next.bottom = next.top + extent;
        }
        if (std::isfinite(next.left) && std::isfinite(next.top) &&
            std::isfinite(next.right) && std::isfinite(next.bottom))
            bounds_ = next;
    }

    void finishGesture() noexcept { active_ = EditorHandle::None; }

private:
    BoxKind kind_;
    EditorBounds bounds_{};
    EditorPoint press_{};
    EditorHandle active_{EditorHandle::None};
    bool placed_{};
};

} // namespace HuhuCapture::Editor
