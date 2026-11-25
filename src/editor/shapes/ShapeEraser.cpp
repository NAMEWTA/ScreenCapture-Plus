#include "pch.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/History.h"
#include "editor/shapes/ShapeEraser.h"

using Microsoft::WRL::ComPtr;

ShapeEraser::ShapeEraser(WinPin* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	// 橡皮没有颜色可选，这个半透明品红只在拖拽过程中当占位提示 ——
	// 松开鼠标后 isErasing 置位，改用底图画刷把这块盖回原样
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isRect = toolSub->isEraserRect;
	// 圆头圆角描边。本项目没有 App::getRoundStrokeStyle 那样的全局缓存，
	// 就每个 shape 自己持一个 —— StrokeStyle 是不可变的轻对象，代价可以忽略
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_ROUND, 8.f, D2D1_DASH_STYLE_SOLID, 0.f
		),
		nullptr, 0, roundStyle.GetAddressOf());
}

ShapeEraser::~ShapeEraser()
{

}

void ShapeEraser::paint(ID2D1DeviceContext* ctx)
{
    const auto& bounds = box_.bounds();
    const auto rect = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom);
	if (isRect) {
		if (isErasing) {
			initBackgroundBrush();
			if (bgBrush) {
				ctx->FillRectangle(rect, bgBrush.Get());
			}
		}
		else {
			ctx->FillRectangle(rect, brush.Get());
		}
		return;
	}
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (isErasing) {
		initBackgroundBrush();
		if (bgBrush) {
			ctx->DrawGeometry(path.Get(), bgBrush.Get(), strokeWidth, roundStyle.Get());
		}
	}
	else {
		ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
	}
}

void ShapeEraser::paintDragger(ID2D1DeviceContext* ctx)
{
    const auto draw = [&](const HuhuCapture::Editor::EditorBounds& bounds) {
        ctx->DrawRectangle(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom),
            brushDragger.Get(), win->dpi);
    };
    if (isRect) {
        for (const auto& bounds : box_.handles(draggerSize)) draw(bounds);
    } else {
        for (const auto& bounds : stroke_.handles(draggerSize)) draw(bounds);
    }
}

void ShapeEraser::mouseDrag(const float x, const float y)
{
    const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (isRect) {
        resetEraser();
        box_.drag({x, y}, shiftDown);
    } else if (stroke_.gestureActive()) {
        stroke_.drag({x, y}, shiftDown);
        makePath();
    }
}

void ShapeEraser::mouseDown(const float x, const float y)
{
    if (isRect) {
        hoverDraggerIndex = static_cast<int>(box_.beginGesture({x, y},
            static_cast<HuhuCapture::Editor::EditorHandle>(hoverDraggerIndex)));
    } else {
        const bool wasPlaced = stroke_.placed();
        hoverDraggerIndex = static_cast<int>(stroke_.beginGesture({x, y},
            (GetKeyState(VK_SHIFT) & 0x8000) != 0,
            static_cast<HuhuCapture::Editor::PathHandle>(hoverDraggerIndex)));
        if (!wasPlaced && stroke_.placed()) {
            makePath();
            win->refresh();
        }
    }
}

void ShapeEraser::mouseUp(const float x, const float y)
{
    box_.finishGesture();
    stroke_.finishGesture();
    const auto& rect = box_.bounds();
    isErasing = isRect ? rect.right > rect.left && rect.bottom > rect.top
        : path && stroke_.points().size() > 1;
}

void ShapeEraser::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = isRect
        ? static_cast<int>(box_.hitTest({x, y}, draggerSize, 2.f * win->dpi))
        : static_cast<int>(stroke_.hitTest({x, y}, draggerSize, strokeWidth, win->dpi));
}

void ShapeEraser::setCursor()
{
	if (isRect) {
		if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4) {
			SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
		}
		else if (hoverDraggerIndex == 1 || hoverDraggerIndex == 5) {
			SetCursor(LoadCursor(nullptr, IDC_SIZENS));
		}
		else if (hoverDraggerIndex == 2 || hoverDraggerIndex == 6) {
			SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
		}
		else if (hoverDraggerIndex == 3 || hoverDraggerIndex == 7) {
			SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
		}
		else if (hoverDraggerIndex == 8) {
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		}
		return;
	}
	if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

void ShapeEraser::makePath()
{
    const auto points = stroke_.points();
    if (points.empty()) return;
    resetEraser();
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

void ShapeEraser::resetEraser()
{
	isErasing = false;
	bgBrush.Reset();
}

// 底图画刷懒创建。底图与窗口同尺寸同坐标，所以不需要像 ShapeMosaic 那样平移画刷
void ShapeEraser::initBackgroundBrush()
{
	if (bgBrush || !win->screenImg) return;
	auto bitmapBrushProps = D2D1::BitmapBrushProperties(
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	auto brushProps = D2D1::BrushProperties();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateBitmapBrush(win->screenImg.Get(), &bitmapBrushProps, &brushProps, bgBrush.GetAddressOf());
}
