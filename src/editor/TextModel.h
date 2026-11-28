#pragma once

#include "editor/Geometry.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace HuhuCapture::Editor {

enum class TextHandle { None = -1, FinishEditing = 0, Edit = 1, Move = 8 };

struct TextStyle {
    float fontSize{20}; // Original-image physical pixels, not view DIPs.
    std::uint32_t rgba{0x000000FF}; // Existing configuration uses 0xRRGGBBAA.
    bool bold{}, italic{};
    bool operator==(const TextStyle&) const = default;
};

// Owns complete text and source-space geometry. Font measurement, the shared
// input widget and IME remain in the presentation adapter; no layout snapshot
// or input widget is permitted to become the authoritative copy of the note.
class TextModel {
public:
    explicit TextModel(float padding) noexcept : padding_(padding) {}

    const std::wstring& text() const noexcept { return text_; }
    void setText(const std::wstring& text) { text_ = text; }
    void setText(std::wstring&& text) noexcept { text_ = std::move(text); }
    const TextStyle& style() const noexcept { return style_; }
    const EditorBounds& bounds() const noexcept { return bounds_; }
    float padding() const noexcept { return padding_; }
    bool placed() const noexcept { return placed_; }
    bool isEditing() const noexcept { return editing_; }
    bool gestureActive() const noexcept { return moving_; }

    bool setStyle(TextStyle style) noexcept
    {
        if (!std::isfinite(style.fontSize) || style.fontSize <= 0) return false;
        style_ = style;
        return true;
    }

    bool place(EditorPoint point) noexcept
    {
        if (placed_ || !finite(point) || !std::isfinite(padding_) || padding_ < 0) return false;
        bounds_ = {point.x - padding_, point.y - padding_, point.x + padding_, point.y + padding_};
        placed_ = true;
        return true;
    }

    bool setLayoutBounds(EditorBounds bounds) noexcept
    {
        if (!placed_ || !finite({bounds.left, bounds.top}) || !finite({bounds.right, bounds.bottom}) ||
            bounds.right < bounds.left || bounds.bottom < bounds.top)
            return false;
        bounds_ = bounds;
        return true;
    }

    bool startEditing() noexcept
    {
        if (!placed_ || editing_) return false;
        moving_ = false;
        editing_ = true;
        return true;
    }

    // The owner disconnects the shared input subscriptions before blurring it,
    // copies its final full text, then deletes an empty model via a history ticket.
    bool finishEditing() noexcept
    {
        if (!editing_) return false;
        editing_ = false;
        return true;
    }

    TextHandle hitTest(EditorPoint point, float interactionScale) const noexcept
    {
        if (!placed_ || !finite(point) || !std::isfinite(interactionScale) || interactionScale <= 0)
            return TextHandle::None;
        const float half = padding_ / 2 + interactionScale;
        if (point.x >= bounds_.left - half && point.x <= bounds_.right + half &&
            point.y >= bounds_.top - half && point.y <= bounds_.bottom + half) {
            if (point.x <= bounds_.left + half || point.x >= bounds_.right - half ||
                point.y <= bounds_.top + half || point.y >= bounds_.bottom - half)
                return TextHandle::Move;
            return TextHandle::Edit;
        }
        // An outside press finishes the current input session instead of
        // immediately creating another invisible/empty text shape.
        return editing_ ? TextHandle::FinishEditing : TextHandle::None;
    }

    void beginMove(EditorPoint point) noexcept
    {
        if (!placed_ || !finite(point)) return;
        press_ = point;
        moving_ = true;
    }

    void drag(EditorPoint point) noexcept
    {
        if (!moving_ || !finite(point)) return;
        const float dx = point.x - press_.x, dy = point.y - press_.y;
        bounds_.left += dx;
        bounds_.right += dx;
        bounds_.top += dy;
        bounds_.bottom += dy;
        press_ = point;
    }

    void finishGesture() noexcept { moving_ = false; }

private:
    static bool finite(EditorPoint point) noexcept
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    std::wstring text_;
    TextStyle style_{};
    EditorBounds bounds_{};
    EditorPoint press_{};
    const float padding_;
    bool placed_{}, editing_{}, moving_{};
};

} // namespace HuhuCapture::Editor
