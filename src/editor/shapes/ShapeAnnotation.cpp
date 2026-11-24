#include "pch.h"
#include "editor/shapes/ShapeAnnotation.h"
#include "editor/WinPin.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Core = HuhuCapture::Core;

ShapeAnnotation::ShapeAnnotation(WinPin* window)
    : ShapeBase(window), number_(window->allocateAnnotationId())
{
    // Opening the rail may fit a large screenshot into a small viewport. Keep
    // newly created badges readable on screen, then retain that original-image
    // size during later zooming and export rather than changing existing ones.
    markerScale_ = win->dpi / (std::max)(0.01f, win->scale);
    const float dpi = markerScale_;
    radius_ = 12.0f * dpi;
    const auto palette = Core::annotationColor(number_);
    auto* d2d = Ling::D2D::get();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(palette.backgroundRgb), fill_.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(palette.textRgb), text_.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), border_.GetAddressOf());
    const auto label = std::to_wstring(number_);
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
    note_ = std::move(note);
}

D2D1_COLOR_F ShapeAnnotation::color() const noexcept
{
    return D2D1::ColorF(Core::annotationColor(number_).backgroundRgb);
}

D2D1_SIZE_U ShapeAnnotation::imageSize() const noexcept
{
    return win->screenImg ? win->screenImg->GetPixelSize() : D2D1::SizeU(0, 0);
}

D2D1_RECT_F ShapeAnnotation::bounds() const noexcept
{
    return D2D1::RectF(region_.left, region_.top, region_.right, region_.bottom);
}

D2D1_POINT_2F ShapeAnnotation::center() const noexcept
{
    const auto size = imageSize();
    const auto point = Core::annotationBadgeCenter(region_, radius_,
        static_cast<float>(size.width), static_cast<float>(size.height));
    return D2D1::Point2F(point.x, point.y);
}

bool ShapeAnnotation::hasValidRegion() const noexcept
{
    return placed_ && Core::validAnnotationRegion(region_,
        Core::annotationRegionMinimumExtent(markerScale_));
}

float ShapeAnnotation::handleTolerance() const noexcept
{
    // Hit targets and selection handles follow the current view. The stored
    // border, badge and region keep their original-image dimensions.
    return 4.0f * win->dpi / (std::max)(0.01f, win->scale);
}

void ShapeAnnotation::paint(ID2D1DeviceContext* context)
{
    if (!placed_ || !fill_ || !text_ || !border_ || !numberLayout_) return;
    const float width = region_.right - region_.left, height = region_.bottom - region_.top;
    if (width <= 0.0f || height <= 0.0f) return;
    // Draw wholly inside the selected bounds, including at image edges. The
    // exact same brush paints the region and its automatic-color badge.
    const float stroke = (std::min)(2.0f * markerScale_, (std::min)(width, height));
    const float inset = stroke / 2.0f;
    context->DrawRectangle(D2D1::RectF(region_.left + inset, region_.top + inset,
        region_.right - inset, region_.bottom - inset), fill_.Get(), stroke);
    if (!hasValidRegion()) return;
    const auto badge = center();
    const auto ellipse = D2D1::Ellipse(badge, radius_, radius_);
    // The outline lies inside the badge so an image-edge badge is not clipped.
    context->FillEllipse(ellipse, fill_.Get());
    const float outline = (std::min)(markerScale_, radius_ / 3.0f);
    context->DrawEllipse(D2D1::Ellipse(badge, radius_ - outline / 2.0f,
        radius_ - outline / 2.0f), border_.Get(), outline);
    context->DrawTextLayout({badge.x - radius_, badge.y - radius_}, numberLayout_.Get(), text_.Get());
}

void ShapeAnnotation::paintDragger(ID2D1DeviceContext* context)
{
    if (!hasValidRegion() || creating_ || !fill_ || !border_) return;
    const float unit = handleTolerance() / 4.0f;
    const float half = 3.0f * unit;
    for (const auto point : Core::annotationRegionHandles(region_)) {
        const auto handle = D2D1::RectF(point.x - half, point.y - half, point.x + half, point.y + half);
        context->FillRectangle(handle, border_.Get());
        context->DrawRectangle(handle, fill_.Get(), unit);
    }
}

void ShapeAnnotation::mouseMove(float x, float y)
{
    hoverDraggerIndex = -1;
    if (!hasValidRegion()) return;
    const auto hit = Core::hitAnnotationRegion(region_, {x, y}, handleTolerance());
    hoverDraggerIndex = static_cast<int>(hit);
    const auto badge = center();
    const float dx = x - badge.x, dy = y - badge.y;
    const float hitRadius = radius_ + handleTolerance() / 2.0f;
    const bool corner = hit == Core::AnnotationRegionHandle::TopLeft
        || hit == Core::AnnotationRegionHandle::TopRight
        || hit == Core::AnnotationRegionHandle::BottomLeft
        || hit == Core::AnnotationRegionHandle::BottomRight;
    if (!corner && dx * dx + dy * dy <= hitRadius * hitRadius)
        hoverDraggerIndex = static_cast<int>(Core::AnnotationRegionHandle::Move);
}

void ShapeAnnotation::mouseDown(float x, float y)
{
    if (!placed_) {
        const auto size = imageSize();
        region_ = Core::annotationRegionFromDrag({x, y}, {x, y},
            static_cast<float>(size.width), static_cast<float>(size.height));
        placed_ = true;
        creating_ = true;
        hoverDraggerIndex = static_cast<int>(Core::AnnotationRegionHandle::BottomRight);
    }
    press_ = {x, y};
    dragOrigin_ = region_;
    win->selectAnnotation(number_, false);
    win->refresh();
}

void ShapeAnnotation::mouseDrag(float x, float y)
{
    if (!placed_ || hoverDraggerIndex < 0) return;
    const auto size = imageSize();
    const float width = static_cast<float>(size.width), height = static_cast<float>(size.height);
    const auto handle = static_cast<Core::AnnotationRegionHandle>(hoverDraggerIndex);
    if (creating_)
        region_ = Core::annotationRegionFromDrag(press_, {x, y}, width, height);
    else if (handle == Core::AnnotationRegionHandle::Move)
        region_ = Core::moveAnnotationRegion(dragOrigin_, {x - press_.x, y - press_.y}, width, height);
    else
        region_ = Core::resizeAnnotationRegion(dragOrigin_, handle, {x, y},
            width, height, Core::annotationRegionMinimumExtent(markerScale_));
}

void ShapeAnnotation::mouseUp(float, float)
{
    creating_ = false;
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
