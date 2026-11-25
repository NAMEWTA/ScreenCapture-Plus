#pragma once

#include "editor/AnnotationModel.h"
#include "editor/shapes/ShapeBase.h"
#include <cstdint>
#include <string>

// A rectangle, numbered badge and note are one history-owned object. Moving or
// resizing the region does not change its identifier, automatic color or note;
// undo/redo restores that same object. Note editing is coordinated by WinPin's
// shared right rail.
class ShapeAnnotation final : public ShapeBase {
public:
    explicit ShapeAnnotation(WinPin* win);

    std::uint64_t number() const noexcept { return model_.number(); }
    const std::wstring& note() const noexcept { return model_.note(); }
    void setNote(std::wstring note);
    D2D1_COLOR_F color() const noexcept;
    D2D1_POINT_2F center() const noexcept;
    D2D1_RECT_F bounds() const noexcept;
    float badgeRadius() const noexcept { return radius_; }
    bool hasValidRegion() const noexcept;

    void paint(ID2D1DeviceContext* context) override;
    void paintDragger(ID2D1DeviceContext* context) override;
    void mouseMove(float x, float y) override;
    void mouseDrag(float x, float y) override;
    void mouseDown(float x, float y) override;
    void mouseUp(float x, float y) override;
    void setCursor() override;
    bool isValidWithoutDrag() override { return false; }

private:
    D2D1_SIZE_U imageSize() const noexcept;
    float handleTolerance() const noexcept;
    HuhuCapture::Editor::AnnotationModel model_;
    float radius_{12.0f};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> numberLayout_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_, text_, border_;
};
