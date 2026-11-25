#include "pch.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/shapes/ShapeArrow.h"

using Microsoft::WRL::ComPtr;

ShapeArrow::ShapeArrow(WinPin* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	// 滑块值当箭头尺寸用。太小的话箭头画出来只有几个像素，看不出形状
	model_.setSize(toolSub->getSliderVal() * 4.f);
	isFill = toolSub->isArrowFill;
}

ShapeArrow::~ShapeArrow()
{

}

void ShapeArrow::paint(ID2D1DeviceContext* ctx)
{
	// makeArrow 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (isFill) {
		ctx->FillGeometry(path.Get(), brush.Get());
	}
	else {
		ctx->DrawGeometry(path.Get(), brush.Get(), win->dpi);
	}
}

void ShapeArrow::paintDragger(ID2D1DeviceContext* ctx)
{
    for (const auto& bounds : model_.handles(draggerSize))
        ctx->DrawRectangle(D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom),
            brushDragger.Get(), win->dpi);
}

void ShapeArrow::mouseDrag(const float x, const float y)
{
    if (!model_.gestureActive()) return;
    model_.drag({x, y}, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    makeArrow();
}

void ShapeArrow::mouseDown(const float x, const float y)
{
    const bool wasPlaced = model_.placed();
    hoverDraggerIndex = static_cast<int>(model_.beginGesture({x, y},
        static_cast<HuhuCapture::Editor::PathHandle>(hoverDraggerIndex)));
    if (!wasPlaced && model_.placed()) makeArrow();
}

void ShapeArrow::mouseUp(const float x, const float y)
{
    model_.finishGesture();
}

void ShapeArrow::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = static_cast<int>(model_.hitHandles({x, y}, draggerSize));
    if (hoverDraggerIndex < 0 && path) {
        BOOL contains = FALSE;
        path->FillContainsPoint({x, y}, nullptr, &contains);
        if (contains) hoverDraggerIndex = 8;
    }
}

// 光标停在两端的 dragger 或箭头身上（此时 WinPin 才把滚轮事件转过来）滚滚轮 = 调箭头大小，
// 与矩形/椭圆用滚轮调线宽是一回事。
void ShapeArrow::mouseWheel(const float x, const float y, const short delta)
{
    // The tool's physical slider value remains one quarter of the arrow size.
    const float step = 4.f * win->dpi;
    const float next = model_.size() + (delta < 0 ? -step : step);
    const float applied = win->toolSub->setShapeSliderVal(L"arrow", next / 4.f) * 4.f;
    if (applied == model_.size()) return;
    model_.setSize(applied);
    makeArrow();
    win->refresh();
}

void ShapeArrow::setCursor()
{
	SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
}

void ShapeArrow::makeArrow()
{
    const auto vertices = model_.polygon();
    if (vertices.empty()) return;
    auto d2d = Ling::D2D::get();
    d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
    if (!path) return;
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(sink.GetAddressOf());
    if (!sink) { path.Reset(); return; }
    sink->BeginFigure({vertices.front().x, vertices.front().y}, D2D1_FIGURE_BEGIN_FILLED);
    addPathLines(sink.Get(), vertices.subspan(1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) path.Reset();
}
