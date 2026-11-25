#include "pch.h"
#include <dwmapi.h>
#include <include/Ling.h>
#include "capture/CutMask.h"

using namespace Microsoft::WRL;

CutMask::CutMask(Ling::WinBase* win) :win{ win }, selection{ win->w, win->h }
{
	strokeWidth = 2 * win->dpi;
	paddingTop *= win->dpi;
	paddingMargin *= win->dpi;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), brushBorder.GetAddressOf());
	initWinRect();
}

bool CutMask::highlight(POINT pos)
{
	if (!selection.highlight({ (float)pos.x, (float)pos.y })) return false;
	selectionChanged();
	return true;
}

void CutMask::initWinRect()
{
	struct Enumeration {
		CutMask* owner;
		std::vector<HuhuCapture::Capture::SelectionBounds> rectangles;
	} enumeration{ this, {} };
	EnumWindows([](HWND hwnd, LPARAM lparam)
		{
			if (!hwnd) return TRUE;
			if (!IsWindowVisible(hwnd)) return TRUE;
			BOOL cloaked = FALSE;
			DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
			if (cloaked) return TRUE;
			RECT rect{};
			if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) return TRUE;
			if (rect.right - rect.left <= 6 || rect.bottom - rect.top <= 6) return TRUE;
			auto& result = *(Enumeration*)lparam;
			auto self = result.owner;
			auto win = self->win;
			if (rect.left < win->x) rect.left = win->x;
			if (rect.top < win->y) rect.top = win->y;
			if (rect.right > win->x + win->w) rect.right = (LONG)(win->x + win->w);
			if (rect.bottom > win->y + win->h) rect.bottom = (LONG)(win->y + win->h);
			auto x = (float)(rect.left - win->x);
			auto y = (float)(rect.top - win->y);
			auto r = (float)(rect.right - win->x);
			auto b = (float)(rect.bottom - win->y);
			result.rectangles.push_back({ x, y, r, b });
			return TRUE;
		}, (LPARAM)&enumeration);
	selection.setWindowCandidates(std::move(enumeration.rectangles));
}

void CutMask::selectionChanged()
{
	const auto& rect = selection.bounds();
	maskRect = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
	makeLayout();
	win->refresh();
}

void CutMask::makeLayout()
{
	layout.Reset();
	auto layoutStr = std::format(L"X:{} Y:{} R:{} B:{} W:{} H:{}",
		maskRect.left, maskRect.top, maskRect.right, maskRect.bottom,
		maskRect.right - maskRect.left, maskRect.bottom - maskRect.top);
	layout = Ling::D2D::get()->makeTextLayout(layoutStr, 10 * win->dpi);
	if (!layout) return;
	DWRITE_TEXT_METRICS tm = {};
	layout->GetMetrics(&tm);
	layoutRect = D2D1::RectF(maskRect.left, maskRect.top - paddingMargin - tm.height - paddingMargin * 2, maskRect.left + tm.width + paddingMargin * 2, maskRect.top - paddingMargin);
	// 标签被顶出窗口上边时折回 maskRect 内部
	if (layoutRect.top < 0) {
		auto h = layoutRect.bottom - layoutRect.top;
		auto w = layoutRect.right - layoutRect.left;
		layoutRect.top = maskRect.top + paddingMargin / 2;
		layoutRect.bottom = layoutRect.top + h;
		layoutRect.left = maskRect.left + paddingMargin;
		layoutRect.right = layoutRect.left + w;
	}
	layout->SetMaxWidth(layoutRect.right - layoutRect.left);
	layout->SetMaxHeight(layoutRect.bottom - layoutRect.top);
}

void CutMask::startMakeRect(POINT pos)
{
	selection.beginCreation({ (float)pos.x, (float)pos.y });
}

void CutMask::makeRect(POINT pos)
{
	if (selection.createTo({ (float)pos.x, (float)pos.y })) selectionChanged();
}

bool CutMask::hasRect() const
{
	return selection.hasSelection();
}

MaskHit CutMask::hitTest(POINT pos) const
{
	return selection.hitTest({ (float)pos.x, (float)pos.y });
}

void CutMask::startAdjust(POINT pos)
{
	if (selection.beginAdjustment({ (float)pos.x, (float)pos.y })) selectionChanged();
}

void CutMask::adjust(POINT pos)
{
	if (selection.adjustTo({ (float)pos.x, (float)pos.y })) selectionChanged();
}

void CutMask::paint(ID2D1DeviceContext* ctx)
{
	if (!layout) return;
	ctx->FillRectangle(D2D1::RectF(0.f, 0.f, win->w, maskRect.top), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, maskRect.bottom, win->w, win->h), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, maskRect.top, maskRect.left, maskRect.bottom), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(maskRect.right, maskRect.top, win->w, maskRect.bottom), brushBg.Get());
	auto halfStrokeWidth{ strokeWidth / 2.f };
	ctx->DrawRectangle(D2D1::RectF(maskRect.left - halfStrokeWidth, maskRect.top - halfStrokeWidth, maskRect.right + halfStrokeWidth, maskRect.bottom + halfStrokeWidth), brushBorder.Get(), strokeWidth);
	if (hideLabel) return;
	ctx->FillRectangle(layoutRect, brushBg.Get());
	ctx->DrawTextLayout({ layoutRect.left+ paddingMargin, layoutRect.top+ paddingMargin }, layout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}
