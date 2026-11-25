#include "pch.h"
#include "app/App.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/shapes/ShapeLine.h"

using Microsoft::WRL::ComPtr;

ShapeLine::ShapeLine(WinPin* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto color = toolSub->getSelectedColor();
	isTransparent = toolSub->isLineTransparent;
	if (isTransparent) {
		color.a = 0.5f;
	}
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	// 圆头圆角描边。本项目没有 App::getRoundStrokeStyle 那样的全局缓存，
	// 就每个 shape 自己持一个 —— StrokeStyle 是不可变的轻对象，代价可以忽略
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND,    // 起点线帽：圆角
			D2D1_CAP_STYLE_ROUND,    // 终点线帽：圆角
			D2D1_CAP_STYLE_ROUND,    // 虚线端点（如有）
			D2D1_LINE_JOIN_ROUND,    // 线段连接处：圆角
			8.f,                     // miterLimit
			D2D1_DASH_STYLE_SOLID,
			0.f
		),
		nullptr, 0, roundStyle.GetAddressOf());
}

ShapeLine::~ShapeLine()
{

}

void ShapeLine::paint(ID2D1DeviceContext* ctx)
{
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
}

void ShapeLine::paintDragger(ID2D1DeviceContext* ctx)
{
    for (const auto& bounds : model_.handles(draggerSize))
        ctx->DrawRectangle(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom),
            brushDragger.Get(), win->dpi);
}

void ShapeLine::mouseDrag(const float x, const float y)
{
    if (!model_.gestureActive()) return;
    model_.drag({x, y}, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    makePath();
}

void ShapeLine::mouseDown(const float x, const float y)
{
    const bool wasPlaced = model_.placed();
    hoverDraggerIndex = static_cast<int>(model_.beginGesture({x, y},
        (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        static_cast<HuhuCapture::Editor::PathHandle>(hoverDraggerIndex)));
    if (!wasPlaced && model_.placed()) {
        makePath();
        win->refresh();
    }
}

void ShapeLine::mouseUp(const float x, const float y)
{
    model_.finishGesture();
}

void ShapeLine::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = static_cast<int>(model_.hitTest({x, y}, draggerSize, strokeWidth, win->dpi));
}

void ShapeLine::setCursor()
{
	if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

void ShapeLine::makePath()
{
    const auto points = model_.points();
    if (points.empty()) return;
    auto d2d = Ling::D2D::get();
    d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
    if (!path) return;
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(sink.GetAddressOf());
    if (!sink) { path.Reset(); return; }
    sink->BeginFigure({points.front().x, points.front().y}, D2D1_FIGURE_BEGIN_HOLLOW);
    addPathLines(sink.Get(), points.subspan(1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) path.Reset();
}
