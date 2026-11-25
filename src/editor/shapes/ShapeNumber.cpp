#include "pch.h"
#include "app/App.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/History.h"
#include "editor/shapes/ShapeNumber.h"

using Microsoft::WRL::ComPtr;

ShapeNumber::ShapeNumber(WinPin* win) : ShapeBase(win),
    model_(win->history->document().nextOrdinaryNumber().value_or(0), win->toolSub->getSliderVal())
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0XFFFFFF), brushText.GetAddressOf());
	isFill = toolSub->isNumberFill;
}



ShapeNumber::~ShapeNumber()
{

}

// 把序号排到 2r × 2r 的方框里居中，字号取 r（直径的一半），刚好填满圆
void ShapeNumber::makeTextLayout()
{
    auto d2d = Ling::D2D::get();
    const auto text = std::to_wstring(model_.number());
    const float radius = model_.radius();
    d2d->dwriteFactory->CreateTextLayout(text.data(), static_cast<UINT32>(text.length()),
        d2d->baseTextFormat.Get(), radius * 2, radius * 2, layoutText.ReleaseAndGetAddressOf());
    if (!layoutText) return;
    layoutText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layoutText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layoutText->SetFontSize(radius, {0, static_cast<UINT32>(text.length())});
}

void ShapeNumber::paint(ID2D1DeviceContext* ctx)
{
    if (!path || !layoutText) return;
    const auto center = model_.center();
    const float radius = model_.radius();
    if (isFill) {
        ctx->FillGeometry(path.Get(), brush.Get());
        ctx->DrawTextLayout({center.x - radius, center.y - radius}, layoutText.Get(),
            brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    } else {
        ctx->DrawGeometry(path.Get(), brush.Get(), win->dpi);
        ctx->DrawTextLayout({center.x - radius, center.y - radius}, layoutText.Get(),
            brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

void ShapeNumber::paintDragger(ID2D1DeviceContext* ctx)
{
    if (isWheel) return;
    for (const auto& bounds : model_.handles(draggerSize))
        ctx->DrawRectangle(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom),
            brushDragger.Get(), win->dpi);
}

void ShapeNumber::mouseDrag(const float x, const float y)
{
    if (!model_.gestureActive()) return;
    const auto requested = model_.drag({x, y}, 8.f * win->dpi);
    if (requested) {
        model_.setRadius(win->toolSub->setShapeSliderVal(L"number", *requested));
        makeTextLayout();
    }
    makePath();
}

void ShapeNumber::mouseDown(const float x, const float y)
{
    const bool wasPlaced = model_.placed();
    hoverDraggerIndex = static_cast<int>(model_.beginGesture({x, y},
        static_cast<HuhuCapture::Editor::NumberHandle>(hoverDraggerIndex)));
    if (!wasPlaced && model_.placed()) {
        makePath();
        makeTextLayout();
        win->refresh();
    }
}

void ShapeNumber::mouseUp(const float x, const float y)
{
    model_.finishGesture();
}

void ShapeNumber::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = static_cast<int>(model_.hitTest({x, y}, draggerSize));
    if (isWheel) {
        isWheel = false;
        model_.finishGesture();
        win->refresh();
    }
}

void ShapeNumber::mouseWheel(const float x, const float y, const short delta)
{
    isWheel = true;
    const float radius = model_.radius();
    if (delta < 0 && radius <= 6.f * win->dpi) return;
    const float next = radius + (delta < 0 ? -1.f : 1.f);
    model_.setRadius(win->toolSub->setShapeSliderVal(L"number", next));
    makePath();
    makeTextLayout();
    win->refresh();
}

void ShapeNumber::setCursor()
{
	if (hoverDraggerIndex >=0) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}





void ShapeNumber::makePath()
{
    if (!model_.placed()) return;
    auto d2d = Ling::D2D::get();
    d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
    if (!path) return;
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(sink.GetAddressOf());
    if (!sink) { path.Reset(); return; }
    const auto& outline = model_.outline();
    const float radius = model_.radius();
    sink->BeginFigure({outline.start.x, outline.start.y}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment({outline.middle.x, outline.middle.y}, D2D1::SizeF(radius, radius),
        0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment({outline.end.x, outline.end.y}, D2D1::SizeF(radius, radius),
        0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine({outline.tip.x, outline.tip.y});
    sink->AddLine({outline.start.x, outline.start.y});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) path.Reset();
}
