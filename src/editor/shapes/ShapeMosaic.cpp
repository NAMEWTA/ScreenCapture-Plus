#include "pch.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "editor/History.h"
#include "editor/shapes/ShapeMosaic.h"
#include "core/MosaicPixels.h"

using Microsoft::WRL::ComPtr;

ShapeMosaic::ShapeMosaic(WinPin* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	// 马赛克没有颜色可选，这个半透明品红只在拖拽过程中当占位提示 ——
	// 松开鼠标后 buildMosaicBitmap 会算出真正的马赛克画刷把它替换掉
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isRect = toolSub->isMosaicRect;
	// 圆头圆角描边。本项目没有 App::getRoundStrokeStyle 那样的全局缓存，
	// 就每个 shape 自己持一个 —— StrokeStyle 是不可变的轻对象，代价可以忽略
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_ROUND, 8.f, D2D1_DASH_STYLE_SOLID, 0.f
		),
		nullptr, 0, roundStyle.GetAddressOf());
}

ShapeMosaic::~ShapeMosaic()
{

}

void ShapeMosaic::paint(ID2D1DeviceContext* ctx)
{
    const auto& bounds = box_.bounds();
    const auto rect = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom);
	if (isRect) {
		if (mosaicBrush) {
			ctx->FillRectangle(rect, mosaicBrush.Get());
		}
		else {
			ctx->FillRectangle(rect, brush.Get());
		}
		return;
	}
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (mosaicBrush) {
		ctx->DrawGeometry(path.Get(), mosaicBrush.Get(), strokeWidth, roundStyle.Get());
	}
	else {
		ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
	}
}

void ShapeMosaic::paintDragger(ID2D1DeviceContext* ctx)
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

void ShapeMosaic::mouseDrag(const float x, const float y)
{
    const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (isRect) {
        resetMosaic();
        box_.drag({x, y}, shiftDown);
    } else if (stroke_.gestureActive()) {
        stroke_.drag({x, y}, shiftDown);
        makePath();
    }
}

void ShapeMosaic::mouseDown(const float x, const float y)
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

void ShapeMosaic::mouseUp(const float x, const float y)
{
    box_.finishGesture();
    stroke_.finishGesture();
    // Read back only after the gesture; never rebuild sampled pixels per move.
    buildMosaicBitmap();
}

void ShapeMosaic::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = isRect
        ? static_cast<int>(box_.hitTest({x, y}, draggerSize, 2.f * win->dpi))
        : static_cast<int>(stroke_.hitTest({x, y}, draggerSize, strokeWidth, win->dpi));
}

void ShapeMosaic::setCursor()
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

void ShapeMosaic::makePath()
{
    const auto points = stroke_.points();
    if (points.empty()) return;
    resetMosaic();
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

void ShapeMosaic::resetMosaic()
{
	mosaicBitmap.Reset();
	mosaicBrush.Reset();
	mosaicOrigin = { 0.f, 0.f };
}



void ShapeMosaic::buildMosaicBitmap()
{
    const auto& rect = box_.bounds();
	if (!isRect && !path) return;
	if (isRect && (rect.right <= rect.left || rect.bottom <= rect.top)) return;
	// 块越大越糊。滑块调的是笔画粗细，顺带让粗笔画对应大色块，下限 6px 保证肉眼可见
	int blockSize = std::max(6, (int)std::round(strokeWidth / 3.0f));
	mosaicBitmap = createMosaicBitmap(blockSize);
	mosaicBrush.Reset();
	if (!mosaicBitmap) return;
	// NEAREST_NEIGHBOR 是关键：线性插值会把色块边界糊成渐变，就不像马赛克了
	auto bitmapBrushProps = D2D1::BitmapBrushProperties(
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	auto brushProps = D2D1::BrushProperties();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateBitmapBrush(mosaicBitmap.Get(), &bitmapBrushProps, &brushProps, mosaicBrush.GetAddressOf());
	if (mosaicBrush) {
		// 位图只覆盖包围盒那一小块，画刷默认从原点铺，要平移到包围盒左上角才对得上
		mosaicBrush->SetTransform(D2D1::Matrix3x2F::Translation(mosaicOrigin.x, mosaicOrigin.y));
	}
}

// 把"这个 shape 之前"的画面在包围盒范围内重现一遍，读回像素打成马赛克，再包成画刷。
// 用 d2d->deviceContext 做离屏绘制是安全的：它是全项目共享的资源工厂，
// 没有任何地方给它 SetTarget / BeginDraw（窗口绘制走的是各自 surface 或 swap chain 的 context）。
// 这里 SetTarget → BeginDraw → EndDraw → SetTarget(nullptr) 在函数内闭环，不跨帧持有。
ComPtr<ID2D1Bitmap> ShapeMosaic::createMosaicBitmap(int blockSize)
{
	ComPtr<ID2D1Bitmap> result;
	if ((!isRect && !path) || win->w <= 0 || win->h <= 0) return result;
	if (!win->screenImg) return result;

	auto d2d = Ling::D2D::get();
	auto ctx = d2d->deviceContext.Get();

	D2D1_RECT_F bounds{};
	HRESULT hr{ S_OK };
	if (isRect) {
		const auto& rect = box_.bounds();
        bounds = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
	}
	else {
		// 描边后的实际覆盖范围，比 path 本身宽 strokeWidth
		hr = path->GetWidenedBounds(strokeWidth, roundStyle.Get(), nullptr, &bounds);
		if (FAILED(hr)) return result;
	}

	// 往外扩一个 blockSize，避免边缘那一列色块取样不足；再夹到窗口范围内
	const int winW = (int)win->w, winH = (int)win->h;
	int left = std::max(0, std::min((int)std::floor(bounds.left) - blockSize, winW));
	int top = std::max(0, std::min((int)std::floor(bounds.top) - blockSize, winH));
	int right = std::max(0, std::min((int)std::ceil(bounds.right) + blockSize + 1, winW));
	int bottom = std::max(0, std::min((int)std::ceil(bounds.bottom) + blockSize + 1, winH));
	if (left >= right || top >= bottom) return result;

	mosaicOrigin = { (float)left, (float)top };
	auto localSize = D2D1::SizeU((UINT32)(right - left), (UINT32)(bottom - top));

	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> targetBitmap;
	hr = ctx->CreateBitmap(localSize, nullptr, 0, &targetProps, targetBitmap.GetAddressOf());
	if (FAILED(hr)) return result;

	// 底图 + 排在自己前面且没被撤销的 shape。平移变换让窗口坐标直接落到这块小位图里，
	// 各 shape 的 paint 不用知道自己被画到了别处
	ctx->SetTarget(targetBitmap.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Translation(-mosaicOrigin.x, -mosaicOrigin.y));
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0, 0.0f));
	ctx->DrawBitmap(win->screenImg.Get(), D2D1::RectF(0, 0, win->w, win->h));
	for (auto& shape : win->history->shapes())
	{
		auto cur = shape.get();
		if (cur == this) break;
		cur->paint(ctx);
	}
	hr = ctx->EndDraw();
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	// 解绑，下面 CopyFromBitmap 才能把它当源读
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return result;

	// GPU 上的 target 位图不能直接 Map，得先拷到一块带 CPU_READ 的位图上
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ targetBitmap->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBitmap;
	hr = ctx->CreateBitmap(localSize, nullptr, 0, &cpuProps, cpuBitmap.GetAddressOf());
	if (FAILED(hr)) return result;

	hr = cpuBitmap->CopyFromBitmap(nullptr, targetBitmap.Get(), nullptr);
	if (FAILED(hr)) return result;

	D2D1_MAPPED_RECT mapped{};
	hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr)) return result;

	std::vector<BYTE> pixels((size_t)mapped.pitch * localSize.height);
	CopyMemory(pixels.data(), mapped.bits, pixels.size());
	cpuBitmap->Unmap();

	if (!HuhuCapture::Core::mosaicPixels(pixels, mapped.pitch, static_cast<int>(localSize.width),
		static_cast<int>(localSize.height), blockSize)) return result;

	auto bitmapProps = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	hr = ctx->CreateBitmap(localSize, pixels.data(), mapped.pitch, &bitmapProps, result.GetAddressOf());
	return result;
}
