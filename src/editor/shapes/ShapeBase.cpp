#include "pch.h"
#include "app/App.h"
#include "editor/WinPin.h"
#include "editor/shapes/ShapeBase.h"
#include <array>

ShapeBase::ShapeBase(WinPin* win):win{win}, draggerSize{6*win->dpi}
{
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000), brushDragger.GetAddressOf());
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y)
{
	return (x > rect.left && x<rect.right && y>rect.top && y < rect.bottom);
}

void ShapeBase::addPathLines(ID2D1GeometrySink* sink,
    std::span<const HuhuCapture::Editor::EditorPoint> points)
{
    std::array<D2D1_POINT_2F, 128> batch;
    while (!points.empty()) {
        const auto count = (std::min)(points.size(), batch.size());
        for (std::size_t index = 0; index < count; ++index)
            batch[index] = {points[index].x, points[index].y};
        sink->AddLines(batch.data(), static_cast<UINT32>(count));
        points = points.subspan(count);
    }
}
