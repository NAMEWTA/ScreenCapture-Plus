#include "pch.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/shapes/ShapeEllipse.h"

ShapeEllipse::ShapeEllipse(WinPin* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isEllipseFill;
}

ShapeEllipse::~ShapeEllipse()
{

}

void ShapeEllipse::paint(ID2D1DeviceContext* ctx)
{
    const auto center = model_.center(), radii = model_.radii();
    const auto ellipse = D2D1::Ellipse({center.x, center.y}, radii.x, radii.y);
    if (isFill) ctx->FillEllipse(ellipse, brush.Get());
    else ctx->DrawEllipse(ellipse, brush.Get(), strokeWidth);
}

void ShapeEllipse::paintDragger(ID2D1DeviceContext* ctx)
{
    for (const auto& bounds : model_.handles(draggerSize))
        ctx->DrawRectangle(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom),
            brushDragger.Get(), win->dpi);
}

void ShapeEllipse::mouseDrag(const float x, const float y)
{
    model_.drag({x, y}, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
}

void ShapeEllipse::mouseDown(const float x, const float y)
{
    hoverDraggerIndex = static_cast<int>(model_.beginGesture({x, y},
        static_cast<HuhuCapture::Editor::EditorHandle>(hoverDraggerIndex)));
}

void ShapeEllipse::mouseUp(const float x, const float y)
{
    model_.finishGesture();
}

void ShapeEllipse::mouseMove(const float x, const float y)
{
    const float half = isFill ? 2.f * win->dpi : strokeWidth / 2.f + win->dpi;
    hoverDraggerIndex = static_cast<int>(model_.hitTest({x, y}, draggerSize, half));
}

// 同 ShapeRect::mouseWheel：光标停在边框或八个 dragger 上时滚滚轮 = 调线宽
void ShapeEllipse::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) return;
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"ellipse", next);
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	win->refresh();
}

void ShapeEllipse::setCursor()
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
