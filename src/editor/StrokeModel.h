#pragma once

#include "editor/Geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace HuhuCapture::Editor {

// One geometry implementation for freehand, mosaic and eraser strokes. Style,
// sampling/composition and raster resources stay in the respective renderer.
class StrokeModel {
public:
    std::span<const EditorPoint> points() const noexcept { return points_; }
    bool placed() const noexcept { return !points_.empty(); }
    bool gestureActive() const noexcept { return active_ != PathHandle::None; }

    std::array<EditorBounds, 2> handles(float size) const noexcept
    {
        if (points_.empty()) return {};
        return {editorHandleBounds(points_.front(), size), editorHandleBounds(points_.back(), size)};
    }

    PathHandle hitTest(EditorPoint point, float handleSize, float strokeWidth, float interactionScale) const noexcept
    {
        if (points_.empty() || !finite(point) || !std::isfinite(handleSize) || handleSize < 0 ||
            !std::isfinite(strokeWidth) || strokeWidth < 0 ||
            !std::isfinite(interactionScale) || interactionScale <= 0)
            return PathHandle::None;
        const auto boxes = handles(handleSize);
        if (containsEditorPoint(boxes[0], point)) return PathHandle::Start;
        if (containsEditorPoint(boxes[1], point)) return PathHandle::End;
        const float outer = strokeWidth * 0.5f + interactionScale;
        const float inner = outer - handleSize / 2;
        float nearest = (std::numeric_limits<float>::max)();
        for (std::size_t index = 0; index + 1 < points_.size(); ++index)
            nearest = (std::min)(nearest, distanceToSegment(point, points_[index], points_[index + 1]));
        // Test the nearest segment only. At self intersections, another
        // segment's interior must not be mistaken for this segment's edge.
        return nearest <= outer && nearest >= inner ? PathHandle::Move : PathHandle::None;
    }

    PathHandle beginGesture(EditorPoint point, bool straight, PathHandle handle = PathHandle::None)
    {
        if (!finite(point)) return PathHandle::None;
        if (points_.empty()) {
            std::vector<EditorPoint> initial(straight ? 2u : 1u, point);
            points_ = std::move(initial);
            active_ = PathHandle::End;
        } else {
            active_ = handle;
            if (handle == PathHandle::Move) press_ = point;
        }
        return active_;
    }

    void drag(EditorPoint point, bool straight)
    {
        if (points_.empty() || !finite(point)) return;
        if (active_ == PathHandle::Start) {
            if (straight) points_.front() = point;
            else points_.insert(points_.begin(), point);
        } else if (active_ == PathHandle::End) {
            if (straight) points_.back() = point;
            else points_.push_back(point);
        } else if (active_ == PathHandle::Move) {
            const float dx = point.x - press_.x, dy = point.y - press_.y;
            if (!std::isfinite(dx) || !std::isfinite(dy)) return;
            for (auto& current : points_) {
                current.x += dx;
                current.y += dy;
            }
            press_ = point;
        }
    }

    void finishGesture() noexcept { active_ = PathHandle::None; }

private:
    static bool finite(EditorPoint point) noexcept
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    static float distanceToSegment(EditorPoint point, EditorPoint start, EditorPoint end) noexcept
    {
        const float dx = end.x - start.x, dy = end.y - start.y;
        const float px = point.x - start.x, py = point.y - start.y;
        const float lengthSquared = dx * dx + dy * dy;
        const float t = std::clamp(lengthSquared == 0 ? 0 : (px * dx + py * dy) / lengthSquared, 0.0f, 1.0f);
        const float offsetX = point.x - (start.x + t * dx);
        const float offsetY = point.y - (start.y + t * dy);
        return std::sqrt(offsetX * offsetX + offsetY * offsetY);
    }

    std::vector<EditorPoint> points_;
    EditorPoint press_{};
    PathHandle active_{PathHandle::None};
};

} // namespace HuhuCapture::Editor
