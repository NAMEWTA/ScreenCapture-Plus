#pragma once

#include "editor/Geometry.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace HuhuCapture::Editor {

enum class NumberHandle { None = -1, Move = 0, Rotate = 1, Resize = 2 };

struct NumberOutline {
    EditorPoint start, middle, end, tip;
};

// The ordinary number tool is a directional round marker with a pointed tip.
// The document chooses active maximum + 1; this ID is unrelated to the stable
// annotation sequence. The native/Qt adapters only build arcs and lay out text.
class NumberModel {
public:
    NumberModel(std::uint64_t number, float radius) noexcept : number_(number) { setRadius(radius); }

    std::uint64_t number() const noexcept { return number_; }
    EditorPoint center() const noexcept { return center_; }
    float radius() const noexcept { return radius_; }
    float angle() const noexcept { return angle_; }
    const NumberOutline& outline() const noexcept { return outline_; }
    bool placed() const noexcept { return placed_; }
    bool gestureActive() const noexcept { return active_ != NumberHandle::None; }

    bool setRadius(float radius) noexcept
    {
        if (!std::isfinite(radius) || radius <= 0) return false;
        radius_ = radius;
        rebuild();
        return true;
    }

    std::array<EditorBounds, 3> handles(float size) const noexcept
    {
        return {editorHandleBounds(center_, size), editorHandleBounds(outline_.tip, size),
            editorHandleBounds(outline_.middle, size)};
    }

    NumberHandle hitTest(EditorPoint point, float handleSize) const noexcept
    {
        if (!placed_ || !finite(point) || !std::isfinite(handleSize) || handleSize < 0)
            return NumberHandle::None;
        const auto boxes = handles(handleSize);
        for (std::size_t index = 0; index < boxes.size(); ++index)
            if (containsEditorPoint(boxes[index], point)) return static_cast<NumberHandle>(index);
        return NumberHandle::None;
    }

    NumberHandle beginGesture(EditorPoint point, NumberHandle handle = NumberHandle::None) noexcept
    {
        if (!finite(point)) return NumberHandle::None;
        if (!placed_) {
            center_ = point;
            placed_ = true;
            active_ = NumberHandle::Move;
            rebuild();
        } else active_ = handle;
        press_ = point;
        return active_;
    }

    // Resize is a requested physical radius. The owner passes it through the
    // existing shared slider/configuration clamp, then commits with setRadius().
    // This keeps the visible toolbar value and the model's actual radius equal
    // without coupling model geometry to configuration or a UI callback.
    std::optional<float> drag(EditorPoint point, float minimumResizeRadius) noexcept
    {
        if (!placed_ || !finite(point)) return std::nullopt;
        if (active_ == NumberHandle::Move) {
            center_.x += point.x - press_.x;
            center_.y += point.y - press_.y;
            press_ = point;
            rebuild();
        } else if (active_ == NumberHandle::Rotate) {
            angle_ = -std::atan2(point.y - center_.y, point.x - center_.x) * 180.f / pi;
            rebuild();
        } else if (active_ == NumberHandle::Resize) {
            if (!std::isfinite(minimumResizeRadius) || minimumResizeRadius < 0) return std::nullopt;
            const float dx = point.x - center_.x, dy = point.y - center_.y;
            const float requested = (std::max)(minimumResizeRadius, std::sqrt(dx * dx + dy * dy));
            if (std::isfinite(requested)) return requested;
        }
        return std::nullopt;
    }

    void finishGesture() noexcept { active_ = NumberHandle::None; }

private:
    static constexpr float pi = 3.14159265358979323846f;

    static bool finite(EditorPoint point) noexcept
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    EditorPoint localPoint(float degrees) const noexcept
    {
        const float radians = degrees * pi / 180;
        return {radius_ * std::cos(radians), -radius_ * std::sin(radians)};
    }

    EditorPoint transform(EditorPoint point) const noexcept
    {
        const float radians = -angle_ * pi / 180;
        const float c = std::cos(radians), s = std::sin(radians);
        return {center_.x + point.x * c - point.y * s, center_.y + point.x * s + point.y * c};
    }

    void rebuild() noexcept
    {
        outline_ = {transform(localPoint(10)), transform(localPoint(180)),
            transform(localPoint(350)), transform({radius_ + radius_ / 3, 0})};
    }

    const std::uint64_t number_;
    EditorPoint center_{}, press_{};
    NumberOutline outline_{};
    float radius_{16}, angle_{270};
    NumberHandle active_{NumberHandle::None};
    bool placed_{};
};

} // namespace HuhuCapture::Editor
