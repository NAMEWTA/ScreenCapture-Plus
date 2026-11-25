#include "pch.h"
#include "editor/shapes/ShapeAnnotation.h"
#include "editor/WinPin.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Core = HuhuCapture::Core;

namespace {
HuhuCapture::Editor::AnnotationModel makeModel(WinPin* window)
{
    const auto size = window->screenImg ? window->screenImg->GetPixelSize() : D2D1::SizeU(0, 0);
    return {window->allocateAnnotationId(), size.width, size.height,
        window->dpi / (std::max)(0.01f, window->scale)};
}
}

ShapeAnnotation::ShapeAnnotation(WinPin* window)
    : ShapeBase(window), model_(makeModel(window))
{
    // Opening the rail may fit a large screenshot into a small viewport. Keep
    // newly created badges readable on screen, then retain that original-image
    // size during later zooming and export rather than changing existing ones.
    const float dpi = model_.creationScale();
    radius_ = 12.0f * dpi;
    const auto palette = model_.color();
    auto* d2d = Ling::D2D::get();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(palette.backgroundRgb), fill_.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(palette.textRgb), text_.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), border_.GetAddressOf());
    const auto label = std::to_wstring(number());
    const auto length = static_cast<UINT32>(label.size());
    HRESULT result = d2d->dwriteFactory->CreateTextLayout(label.data(), length,
        d2d->baseTextFormat.Get(), 1024.0f * dpi, 128.0f * dpi, numberLayout_.GetAddressOf());
    if (FAILED(result)) return;
    numberLayout_->SetFontSize(13.0f * dpi, {0, length});
    numberLayout_->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, length});
    numberLayout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(numberLayout_->GetMetrics(&metrics)))
        radius_ = (std::max)(radius_, std::ceil((std::max)(
            metrics.width + 8.0f * dpi, metrics.height + 2.0f * dpi) / 2.0f));
    // Extremely small screenshots still contain the entire badge. The region
    // remains the user's exact selection; only its numeric label is fitted.
    const auto size = imageSize();
    const float imageRadius = static_cast<float>((std::min)(size.width, size.height)) / 2.0f;
    if (imageRadius > 0.0f && imageRadius < radius_) {
        numberLayout_->SetFontSize(13.0f * dpi * imageRadius / radius_, {0, length});
        radius_ = imageRadius;
    }
    numberLayout_->SetMaxWidth(radius_ * 2.0f);
    numberLayout_->SetMaxHeight(radius_ * 2.0f);
    numberLayout_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    numberLayout_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void ShapeAnnotation::setNote(std::wstring note)
{
    model_.setNote(std::move(note));
}

D2D1_COLOR_F ShapeAnnotation::color() const noexcept
{
    return D2D1::ColorF(model_.color().backgroundRgb);
}

D2D1_SIZE_U ShapeAnnotation::imageSize() const noexcept
{
    return win->screenImg ? win->screenImg->GetPixelSize() : D2D1::SizeU(0, 0);
}

D2D1_RECT_F ShapeAnnotation::bounds() const noexcept
{
    const auto& region = model_.bounds();
    return D2D1::RectF(region.left, region.top, region.right, region.bottom);
}

D2D1_POINT_2F ShapeAnnotation::center() const noexcept
{
    const auto point = model_.badgeCenter(radius_);
    return D2D1::Point2F(point.x, point.y);
}

bool ShapeAnnotation::hasValidRegion() const noexcept
{
    return model_.hasValidRegion();
}

float ShapeAnnotation::handleTolerance() const noexcept
{
    // Hit targets and selection handles follow the current view. The stored
    // border, badge and region keep their original-image dimensions.
    return 4.0f * win->dpi / (std::max)(0.01f, win->scale);
}

void ShapeAnnotation::paint(ID2D1DeviceContext* context)
{
    if (!model_.placed() || !fill_ || !text_ || !border_ || !numberLayout_) return;
    const auto& region = model_.bounds();
    const float width = region.right - region.left, height = region.bottom - region.top;
    if (width <= 0.0f || height <= 0.0f) return;
    // Draw wholly inside the selected bounds, including at image edges. The
    // exact same brush paints the region and its automatic-color badge.
    const float stroke = (std::min)(2.0f * model_.creationScale(), (std::min)(width, height));
    const float inset = stroke / 2.0f;
    context->DrawRectangle(D2D1::RectF(region.left + inset, region.top + inset,
        region.right - inset, region.bottom - inset), fill_.Get(), stroke);
    if (!hasValidRegion()) return;
    const auto badge = center();
    const auto ellipse = D2D1::Ellipse(badge, radius_, radius_);
    // The outline lies inside the badge so an image-edge badge is not clipped.
    context->FillEllipse(ellipse, fill_.Get());
    const float outline = (std::min)(model_.creationScale(), radius_ / 3.0f);
    context->DrawEllipse(D2D1::Ellipse(badge, radius_ - outline / 2.0f,
        radius_ - outline / 2.0f), border_.Get(), outline);
    context->DrawTextLayout({badge.x - radius_, badge.y - radius_}, numberLayout_.Get(), text_.Get());
}

void ShapeAnnotation::paintDragger(ID2D1DeviceContext* context)
{
    if (!hasValidRegion() || model_.creating() || !fill_ || !border_) return;
    const float unit = handleTolerance() / 4.0f;
    const float half = 3.0f * unit;
    for (const auto point : Core::annotationRegionHandles(model_.bounds())) {
        const auto handle = D2D1::RectF(point.x - half, point.y - half, point.x + half, point.y + half);
        context->FillRectangle(handle, border_.Get());
        context->DrawRectangle(handle, fill_.Get(), unit);
    }
}

void ShapeAnnotation::mouseMove(float x, float y)
{
    hoverDraggerIndex = static_cast<int>(model_.hitTest({x, y}, handleTolerance(), radius_));
}

void ShapeAnnotation::mouseDown(float x, float y)
{
    hoverDraggerIndex = static_cast<int>(model_.beginGesture({x, y},
        static_cast<Core::AnnotationRegionHandle>(hoverDraggerIndex)));
    win->selectAnnotation(number(), false);
    win->refresh();
}

void ShapeAnnotation::mouseDrag(float x, float y)
{
    model_.drag({x, y});
}

void ShapeAnnotation::mouseUp(float, float)
{
    model_.finishGesture();
}

void ShapeAnnotation::setCursor()
{
    using Handle = Core::AnnotationRegionHandle;
    switch (static_cast<Handle>(hoverDraggerIndex)) {
    case Handle::TopLeft: case Handle::BottomRight:
        SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); break;
    case Handle::TopRight: case Handle::BottomLeft:
        SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); break;
    case Handle::Top: case Handle::Bottom:
        SetCursor(LoadCursor(nullptr, IDC_SIZENS)); break;
    case Handle::Left: case Handle::Right:
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); break;
    case Handle::Move:
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); break;
    default: break;
    }
}
