#include "pch.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/shapes/ShapeRect.h"

ShapeRect::ShapeRect(WinPin* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isRectFill;
}

ShapeRect::~ShapeRect()
{

}

void ShapeRect::paint(ID2D1DeviceContext* ctx)
{
    const auto& bounds = model_.bounds();
    const auto rect = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom);
    if (isFill) ctx->FillRectangle(rect, brush.Get());
    else ctx->DrawRectangle(rect, brush.Get(), strokeWidth);
}

void ShapeRect::paintDragger(ID2D1DeviceContext* ctx)
{
    for (const auto& bounds : model_.handles(draggerSize))
        ctx->DrawRectangle(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom),
            brushDragger.Get(), win->dpi);
}

void ShapeRect::mouseDrag(const float x, const float y)
{
    model_.drag({x, y}, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
}

void ShapeRect::mouseDown(const float x, const float y)
{
    hoverDraggerIndex = static_cast<int>(model_.beginGesture({x, y},
        static_cast<HuhuCapture::Editor::EditorHandle>(hoverDraggerIndex)));
}

void ShapeRect::mouseUp(const float x, const float y)
{
    model_.finishGesture();
}

void ShapeRect::mouseMove(const float x, const float y)
{
    const float half = isFill ? 2.f * win->dpi : strokeWidth / 2.f + win->dpi;
    hoverDraggerIndex = static_cast<int>(model_.hitTest({x, y}, draggerSize, half));
}

// 光标停在边框或八个 dragger 上（此时 WinPin 才把滚轮事件转过来）滚滚轮 = 调线宽，
// 与序号那边用滚轮调大小是一回事。填充矩形没有边框可调，直接不管。
void ShapeRect::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) return;
	// 一格一个逻辑像素。上下限交给 ToolSub 那张滑块值域表夹，用它夹完的返回值 ——
	// 线宽与工具栏滑块因此永远是同一个数，也滚不出滑块能表达的范围
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"rect", next);
	if (applied == strokeWidth) return;   //已经顶到值域的头了，不用重画
	strokeWidth = applied;
	win->refresh();
}

void ShapeRect::setCursor()
{
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
}
