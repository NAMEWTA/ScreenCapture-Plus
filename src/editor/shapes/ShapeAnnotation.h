#pragma once

#include "core/Annotation.h"
#include "editor/shapes/ShapeBase.h"
#include <cstdint>
#include <string>

// A numbered anchor and its note are one history-owned object. Moving the pin
// does not change its identifier, automatic color or note; undo/redo restores
// that same object. Note editing is coordinated by WinPin's shared right rail.
class ShapeAnnotation final : public ShapeBase {
public:
    explicit ShapeAnnotation(WinPin* win);

    std::uint64_t number() const noexcept { return number_; }
    const std::wstring& note() const noexcept { return note_; }
    void setNote(std::wstring note);
    D2D1_COLOR_F color() const noexcept;
    D2D1_POINT_2F center() const noexcept { return center_; }

    void paint(ID2D1DeviceContext* context) override;
    void paintDragger(ID2D1DeviceContext* context) override;
    void mouseMove(float x, float y) override;
    void mouseDrag(float x, float y) override;
    void mouseDown(float x, float y) override;
    void setCursor() override;
    bool isValidWithoutDrag() override { return true; }

private:
    void clampCenter() noexcept;
    std::uint64_t number_;
    std::wstring note_;
    D2D1_POINT_2F center_{0, 0}, press_{0, 0}, dragOrigin_{0, 0};
    float radius_{17.0f};
    float markerScale_{1.0f}; // creation-time logical pixels -> original image pixels
    bool placed_{false};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> numberLayout_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_, text_, border_, shadow_;
};
