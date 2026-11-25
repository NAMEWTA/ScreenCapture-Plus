#pragma once

#include "core/Annotation.h"
#include "core/AnnotationRegion.h"

#include <cstdint>
#include <string>
#include <utility>

namespace HuhuCapture::Editor {

// The annotation's data and pointer gesture, in original-image pixels. This is
// the model used by the production shape; rendering and input focus belong to
// its presentation adapter. It owns no window, font layout or drawing resource.
//
// The document allocates the identifier from AnnotationSequence. Deleting an
// unfinished creation must not return that identifier to the sequence. On lost
// capture the owner removes a newly created model, or calls finishGesture() on
// an existing model to retain its last valid geometry.
class AnnotationModel {
public:
    using Point = Core::AnnotationRegionPoint;
    using Bounds = Core::AnnotationRegionBounds;
    using Handle = Core::AnnotationRegionHandle;

    AnnotationModel(std::uint64_t number, std::uint32_t imageWidth,
        std::uint32_t imageHeight, float creationScale) noexcept
        : number_(number), imageWidth_(imageWidth), imageHeight_(imageHeight),
          creationScale_(creationScale) {}

    std::uint64_t number() const noexcept { return number_; }
    const std::wstring& note() const noexcept { return note_; }
    void setNote(std::wstring note) { note_ = std::move(note); }
    Core::AnnotationColor color() const noexcept { return Core::annotationColor(number_); }
    const Bounds& bounds() const noexcept { return region_; }
    float creationScale() const noexcept { return creationScale_; }
    bool placed() const noexcept { return placed_; }
    bool creating() const noexcept { return creating_; }
    bool gestureActive() const noexcept { return gestureActive_; }

    bool hasValidRegion() const noexcept
    {
        return placed_ && Core::validAnnotationRegion(region_,
            Core::annotationRegionMinimumExtent(creationScale_));
    }

    Point badgeCenter(float radius) const noexcept
    {
        return Core::annotationBadgeCenter(region_, radius,
            static_cast<float>(imageWidth_), static_cast<float>(imageHeight_));
    }

    // Font measurement remains in the renderer. Its actual badge radius is
    // supplied here so hit testing never guesses a numeral's rendered width.
    Handle hitTest(Point point, float tolerance, float badgeRadius) const noexcept
    {
        if (!hasValidRegion()) return Handle::None;
        const auto hit = Core::hitAnnotationRegion(region_, point, tolerance);
        const auto badge = badgeCenter(badgeRadius);
        const float dx = point.x - badge.x, dy = point.y - badge.y;
        const float hitRadius = badgeRadius + tolerance / 2.0f;
        const bool corner = hit == Handle::TopLeft || hit == Handle::TopRight
            || hit == Handle::BottomLeft || hit == Handle::BottomRight;
        if (!corner && dx * dx + dy * dy <= hitRadius * hitRadius)
            return Handle::Move;
        return hit;
    }

    // The returned handle is also the presentation adapter's active handle.
    // Creation is four-directional; resizing fixes the opposite side instead.
    Handle beginGesture(Point point, Handle handle = Handle::None) noexcept
    {
        if (!placed_) {
            region_ = Core::annotationRegionFromDrag(point, point,
                static_cast<float>(imageWidth_), static_cast<float>(imageHeight_));
            placed_ = true;
            creating_ = true;
            handle = Handle::BottomRight;
        }
        press_ = point;
        dragOrigin_ = region_;
        dragHandle_ = handle;
        gestureActive_ = handle != Handle::None;
        return handle;
    }

    void drag(Point point) noexcept
    {
        if (!placed_ || !gestureActive_) return;
        const float width = static_cast<float>(imageWidth_);
        const float height = static_cast<float>(imageHeight_);
        if (creating_)
            region_ = Core::annotationRegionFromDrag(press_, point, width, height);
        else if (dragHandle_ == Handle::Move)
            region_ = Core::moveAnnotationRegion(dragOrigin_,
                {point.x - press_.x, point.y - press_.y}, width, height);
        else
            region_ = Core::resizeAnnotationRegion(dragOrigin_, dragHandle_, point,
                width, height, Core::annotationRegionMinimumExtent(creationScale_));
    }

    void finishGesture() noexcept
    {
        creating_ = false;
        gestureActive_ = false;
        dragHandle_ = Handle::None;
    }

private:
    const std::uint64_t number_;
    const std::uint32_t imageWidth_, imageHeight_;
    const float creationScale_;
    std::wstring note_;
    Bounds region_{}, dragOrigin_{};
    Point press_{};
    Handle dragHandle_{Handle::None};
    bool placed_{false}, creating_{false}, gestureActive_{false};
};

} // namespace HuhuCapture::Editor
