#pragma once

#include "editor/Geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace HuhuCapture::Editor {

// Endpoint, Shift constraint and six-vertex arrow geometry extracted from the
// production ShapeArrow. Filled/outline painting and polygon containment use
// the renderer's mature path implementation rather than a second polygon engine.
class ArrowModel {
public:
    explicit ArrowModel(float size = 12) noexcept { setSize(size); }

    EditorPoint start() const noexcept { return start_; }
    EditorPoint end() const noexcept { return end_; }
    float size() const noexcept { return size_; }
    bool placed() const noexcept { return placed_; }
    bool gestureActive() const noexcept { return active_ != PathHandle::None; }
    std::span<const EditorPoint> polygon() const noexcept { return {polygon_.data(), vertexCount_}; }

    std::array<EditorBounds, 2> handles(float handleSize) const noexcept
    {
        return {editorHandleBounds(start_, handleSize), editorHandleBounds(end_, handleSize)};
    }

    PathHandle hitHandles(EditorPoint point, float handleSize) const noexcept
    {
        if (!placed_ || !finite(point) || !std::isfinite(handleSize) || handleSize < 0)
            return PathHandle::None;
        const auto boxes = handles(handleSize);
        if (containsEditorPoint(boxes[0], point)) return PathHandle::Start;
        if (containsEditorPoint(boxes[1], point)) return PathHandle::End;
        return PathHandle::None;
    }

    bool setSize(float size) noexcept
    {
        if (!std::isfinite(size) || size <= 0) return false;
        size_ = size;
        rebuild();
        return true;
    }

    PathHandle beginGesture(EditorPoint point, PathHandle handle = PathHandle::None) noexcept
    {
        if (!finite(point)) return PathHandle::None;
        if (!placed_) {
            start_ = end_ = point;
            placed_ = true;
            active_ = PathHandle::End;
            rebuild();
        } else {
            active_ = handle;
            if (handle == PathHandle::Move) press_ = point;
        }
        return active_;
    }

    void drag(EditorPoint point, bool eightDirections) noexcept
    {
        if (!placed_ || !finite(point)) return;
        if (active_ == PathHandle::Start)
            start_ = eightDirections ? constrained(end_, point) : point;
        else if (active_ == PathHandle::End)
            end_ = eightDirections ? constrained(start_, point) : point;
        else if (active_ == PathHandle::Move) {
            const float dx = point.x - press_.x, dy = point.y - press_.y;
            start_.x += dx;
            start_.y += dy;
            end_.x += dx;
            end_.y += dy;
            press_ = point;
        } else return;
        rebuild();
    }

    void finishGesture() noexcept { active_ = PathHandle::None; }

private:
    static bool finite(EditorPoint point) noexcept
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    static EditorPoint constrained(EditorPoint anchor, EditorPoint pointer) noexcept
    {
        const float dx = pointer.x - anchor.x, dy = pointer.y - anchor.y;
        const float x = std::abs(dx), y = std::abs(dy);
        constexpr float tangent22Degrees = 0.41421356237f;
        if (y <= x * tangent22Degrees) return {anchor.x + dx, anchor.y};
        if (x <= y * tangent22Degrees) return {anchor.x, anchor.y + dy};
        const float span = (std::max)(x, y);
        return {anchor.x + (dx >= 0 ? span : -span), anchor.y + (dy >= 0 ? span : -span)};
    }

    void rebuild() noexcept
    {
        if (!placed_) { vertexCount_ = 0; return; }
        polygon_[0] = start_;
        const float dx = end_.x - start_.x, dy = end_.y - start_.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1) { vertexCount_ = 1; return; }
        const float ux = dx / length, uy = dy / length, vx = -uy, vy = ux;
        const float shaft = size_ / 4, head = size_ * 2 / 3;
        polygon_[1] = {end_.x - size_ * ux - shaft * vx, end_.y - size_ * uy - shaft * vy};
        polygon_[2] = {end_.x - (size_ + shaft) * ux - head * vx, end_.y - (size_ + shaft) * uy - head * vy};
        polygon_[3] = end_;
        polygon_[4] = {end_.x - (size_ + shaft) * ux + head * vx, end_.y - (size_ + shaft) * uy + head * vy};
        polygon_[5] = {end_.x - size_ * ux + shaft * vx, end_.y - size_ * uy + shaft * vy};
        vertexCount_ = polygon_.size();
    }

    EditorPoint start_{}, end_{}, press_{};
    std::array<EditorPoint, 6> polygon_{};
    std::size_t vertexCount_{};
    float size_{12};
    PathHandle active_{PathHandle::None};
    bool placed_{};
};

} // namespace HuhuCapture::Editor
