#include "pch.h"
#include "editor/shapes/ShapeAnnotation.h"
#include "editor/WinPin.h"

#include <algorithm>
#include <cmath>
#include <utility>

ShapeAnnotation::ShapeAnnotation(WinPin* window)
    : ShapeBase(window), number_(window->allocateAnnotationId())
{
    // Opening the rail may fit a large screenshot into a small viewport. Keep
    // newly placed pins readable on screen, then retain that original-image
    // size during later zooming and export rather than changing existing pins.
    markerScale_ = win->dpi / (std::max)(0.01f, win->scale);
    const float dpi = markerScale_;
    radius_ = 17.0f * dpi;
    const auto palette = ScreenCapture::Core::annotationColor(number_);
    auto* d2d = Ling::D2D::get();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(palette.backgroundRgb), fill_.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(palette.textRgb), text_.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), border_.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x172033, 0.35f), shadow_.GetAddressOf());
    const auto label = std::to_wstring(number_);
    const auto length = static_cast<UINT32>(label.size());
    HRESULT result = d2d->dwriteFactory->CreateTextLayout(label.data(), length,
        d2d->baseTextFormat.Get(), 1024.0f * dpi, 128.0f * dpi, numberLayout_.GetAddressOf());
    if (FAILED(result)) return;
    numberLayout_->SetFontSize(17.0f * dpi, {0, length});
    numberLayout_->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, length});
    numberLayout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(numberLayout_->GetMetrics(&metrics)))
        radius_ = (std::max)(radius_, std::ceil((std::max)(metrics.width, metrics.height) / 2.0f + 6.0f * dpi));
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
    return D2D1::ColorF(ScreenCapture::Core::annotationColor(number_).backgroundRgb);
}

void ShapeAnnotation::paint(ID2D1DeviceContext* context)
{
    if (!placed_ || !fill_ || !text_ || !border_ || !shadow_ || !numberLayout_) return;
    const auto ellipse = D2D1::Ellipse(center_, radius_, radius_);
    context->DrawEllipse(D2D1::Ellipse(center_, radius_ + markerScale_, radius_ + markerScale_),
        shadow_.Get(), 2.0f * markerScale_);
    context->FillEllipse(ellipse, fill_.Get());
    context->DrawEllipse(ellipse, border_.Get(), 2.0f * markerScale_);
    context->DrawTextLayout({center_.x - radius_, center_.y - radius_}, numberLayout_.Get(), text_.Get());
}

void ShapeAnnotation::paintDragger(ID2D1DeviceContext* context)
{
    if (!placed_) return;
    const float radius = radius_ + 4.0f * markerScale_;
    context->DrawEllipse(D2D1::Ellipse(center_, radius, radius), brushDragger.Get(), markerScale_);
}

void ShapeAnnotation::mouseMove(float x, float y)
{
    const float dx = x - center_.x, dy = y - center_.y;
    const float hitRadius = radius_ + 3.0f * markerScale_;
    hoverDraggerIndex = placed_ && dx * dx + dy * dy <= hitRadius * hitRadius ? 0 : -1;
}

void ShapeAnnotation::mouseDown(float x, float y)
{
    if (!placed_) {
        center_ = {x, y};
        clampCenter();
        placed_ = true;
    }
    press_ = {x, y};
    dragOrigin_ = center_;
    hoverDraggerIndex = 0;
    win->selectAnnotation(number_, false);
    win->refresh();
}

void ShapeAnnotation::mouseDrag(float x, float y)
{
    if (!placed_ || hoverDraggerIndex < 0) return;
    center_ = {dragOrigin_.x + x - press_.x, dragOrigin_.y + y - press_.y};
    clampCenter();
}

void ShapeAnnotation::clampCenter() noexcept
{
    if (!win->screenImg) return;
    const auto size = win->screenImg->GetPixelSize();
    if (size.width == 0 || size.height == 0) return;
    center_.x = std::clamp(center_.x, 0.0f, static_cast<float>(size.width - 1));
    center_.y = std::clamp(center_.y, 0.0f, static_cast<float>(size.height - 1));
}

void ShapeAnnotation::setCursor()
{
    if (hoverDraggerIndex >= 0) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
}
