#include "pch.h"
#include <include/Ling.h>
#include "capture/CapLong.h"
#include "capture/WinCap.h"
#include "capture/CutMask.h"
#include "editor/WinPin.h"
#include "ui/toolbars/ToolLong.h"
#include "app/App.h"
#include "services/ImageExport.h"
#include "services/MediaHistory.h"
#include "services/Lang.h"
#include "core/ImagePreview.h"
#include <limits>
#include <new>
using namespace Microsoft::WRL;

namespace {
    constexpr UINT scrollMsgId = 18;
    constexpr UINT scrollEndMsgId = 19;
    using Rules = HuhuCapture::Capture::ScrollCaptureRules;

}

CapLong::CapLong(WinCap* win) : win(win)
{
    startCircleR *= win->dpi;
    auto d2d = Ling::D2D::get();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), textBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.68f), bgBrush.GetAddressOf());
    auto size{ startCircleR * 2 };
    layoutTextStart = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.start"), 16 * win->dpi, size, size);
    if (layoutTextStart) {
        DWRITE_TEXT_METRICS tm{};
        layoutTextStart->GetMetrics(&tm);
        startTextSize = { tm.width, tm.height };
    }
}

CapLong::~CapLong()
{
}

void CapLong::dispose()
{
    isScrolling = false;
    capture.reset();
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    if (tool) tool->close();
}

void CapLong::paint(ID2D1DeviceContext* ctx)
{
    paintImgPreview(ctx);
    if (isFinish) {
        auto borderRadius{ 4.f * win->dpi };
        ctx->FillRoundedRectangle(D2D1::RoundedRect(stopTextRect, borderRadius, borderRadius), bgBrush.Get());
        ctx->DrawTextLayout(stopTextPos, layoutTextEnd.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    else if (isShowStartBtn) {
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)circleCenter.x, (float)circleCenter.y), startCircleR, startCircleR), bgBrush.Get());
        ctx->DrawTextLayout({ circleCenter.x - startTextSize.width / 2, circleCenter.y - startTextSize.height / 2 },
            layoutTextStart.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

void CapLong::setCursor()
{
    if (!isFinish && isShowStartBtn) {
        // 开始按钮跟着光标走，藏掉系统光标免得两个东西叠在一起
        SetCursor(NULL);
    }
    else {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
}

void CapLong::onMove(POINT pos)
{
    if (isFinish) {
        if (isShowStartBtn) {
            isShowStartBtn = false;
            win->refresh();
        }
        return;
    }
    circleCenter = pos;
    auto& r = win->cutMask->maskRect;
    if (pos.x > r.left && pos.x < r.right && pos.y > r.top && pos.y < r.bottom) {
        isShowStartBtn = true;
        win->refresh();
    }
    else {
        if (isShowStartBtn) {
            isShowStartBtn = false;
            win->refresh();
        }
    }
}

void CapLong::onUp(POINT pos)
{
    if (isScrolling || isFinish) return;
    if (isShowStartBtn) { //按下开始按钮
        isScrolling = true;
        win->hollowWin();
        makeTool();
        firstStep(); //首次截图
    }
}

void CapLong::onTimerCB(UINT timerId)
{
    if (!isScrolling || isFinish) return;
    if (timerId == scrollMsgId) {
        POINT pt;
        GetCursorPos(&pt);
        auto tarHwnd = WindowFromPoint(pt);
        if (targetHwnd == nullptr) {
            targetHwnd = tarHwnd;
        }
        if (tarHwnd != targetHwnd) return; //鼠标没在截屏区域直接退出，定时器仍在检查
        win->killTimer(scrollMsgId);
        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = -WHEEL_DELTA;
        SendInput(1, &input, sizeof(INPUT));
        win->setTimer(Rules::scrollSettleMs, scrollEndMsgId); //滚动开始
    }
    else if (scrollEndMsgId == timerId) {
        win->killTimer(scrollEndMsgId); //滚动完成
        capStep();
    }
}

void CapLong::firstStep()
{
    auto& maskRect = win->cutMask->maskRect;
    const int width = int(maskRect.right - maskRect.left);
    const int height = int(maskRect.bottom - maskRect.top);
    capStartPos.x = (int)maskRect.left;
    capStartPos.y = (int)maskRect.top;
    ClientToScreen(win->hwnd, &capStartPos);
    if (!capture.capture(capStartPos.x, capStartPos.y, width, height) ||
        model.begin(capture.pixels(), width, height) != HuhuCapture::Capture::ScrollStartError::None) {
        stopCap();
        return;
    }
    makeImgPreview();
    win->refresh();
    win->setTimer(Rules::firstScrollDelayMs, scrollMsgId);
}

void CapLong::makeImgPreview()
{
    if (!model.hasImage()) return;
    auto ctx = Ling::D2D::get()->deviceContext;
    const auto maximum = static_cast<int>(ctx->GetMaximumBitmapSize());
    POINT bottom{ tool ? tool->x : win->x, tool ? tool->y : win->y };
    bottom.y -= static_cast<LONG>(2 * win->dpi);
    ScreenToClient(win->hwnd, &bottom);
    // The original preview is bottom-aligned above the toolbar. Its invisible
    // top may be hundreds of thousands of rows for a very narrow selection.
    // Rasterize only the visible tail with the identical source coordinates.
    try {
        const auto size = HuhuCapture::Core::makeImagePreview(model.pixels(), model.width(), model.height(),
            tool ? static_cast<float>(tool->w) : static_cast<float>(model.width()),
            static_cast<int>(bottom.y), maximum, previewPixels);
        if (!size) {
            imgPreview.Reset();
            return;
        }
        D2D1_BITMAP_PROPERTIES1 props = {
            .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
            .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
        };
        ComPtr<ID2D1Bitmap1> nextPreview;
        if (SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(size->width, size->height), previewPixels.data(),
                                        size->width * 4, props, nextPreview.GetAddressOf()))) {
            imgPreview = std::move(nextPreview);
        }
    }
    catch (const std::bad_alloc&) {
        // The full result is still valid; keep the previous thumbnail available.
    }
}

void CapLong::capStep()
{
    const auto update = capture.capture(capStartPos.x, capStartPos.y, model.width(), model.frameHeight())
        ? model.accept(capture.pixels()) : model.captureFailed();
    if (update.appended) {
        makeImgPreview();
        win->refresh();
    }
    if (update.next == HuhuCapture::Capture::ScrollNextStep::Stopped) {
        stopCap();
        return;
    }
    if (update.next == HuhuCapture::Capture::ScrollNextStep::Settle)
        win->setTimer(Rules::settleRecheckMs, scrollEndMsgId);
    else
        win->setTimer(Rules::retryDelayMs, scrollMsgId);
}

void CapLong::makeTool()
{
    tool = std::make_unique<ToolLong>(win);
    // 尺寸在 ToolLong 构造里算好了，这里只定位；两者都要在建窗口之前设好
    layoutTool();
    tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

void CapLong::layoutTool()
{
    if (!tool) return;
    const auto& mask = win->cutMask->maskRect;
    const RECT maskOnScreen{
        win->x + static_cast<LONG>(mask.left), win->y + static_cast<LONG>(mask.top),
        win->x + static_cast<LONG>(mask.right), win->y + static_cast<LONG>(mask.bottom)
    };
    MONITORINFO monitor{ sizeof(MONITORINFO) };
    if (!GetMonitorInfo(MonitorFromRect(&maskOnScreen, MONITOR_DEFAULTTONEAREST), &monitor)) {
        monitor.rcWork = RECT{ win->x, win->y,
            win->x + static_cast<LONG>(win->w), win->y + static_cast<LONG>(win->h) };
    }
    tool->fitToWorkArea(monitor.rcWork);
    const auto width = static_cast<LONG>(tool->w + .5f);
    const auto height = static_cast<LONG>(tool->h + .5f);
    const auto gap = static_cast<LONG>(win->cutMask->strokeWidth + 2.f * tool->dpi + .5f);
    // Prefer the right side of the selection, then the left; keep all controls
    // inside this physical monitor when neither side has enough space.
    LONG left = maskOnScreen.right + gap;
    if (left + width > monitor.rcWork.right) left = maskOnScreen.left - gap - width;
    if (left < monitor.rcWork.left) left = maskOnScreen.right - width - gap;
    const auto top = maskOnScreen.bottom - height;
    tool->setPosition(
        std::clamp(left, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width)),
        std::clamp(top, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height)));
}

void CapLong::paintImgPreview(ID2D1DeviceContext* ctx)
{
    if (!imgPreview || !tool) return;
    auto bitmapSize = imgPreview->GetPixelSize();
    float drawW = (float)bitmapSize.width;
    float drawH = (float)bitmapSize.height;
    POINT pos{ tool->x, tool->y - (int)drawH - (int)(2 * win->dpi) };
    ScreenToClient(win->hwnd, &pos);
    D2D1_RECT_F destRect = D2D1::RectF((float)pos.x, (float)pos.y, pos.x + drawW, pos.y + drawH);
    ctx->DrawBitmap(imgPreview.Get(), destRect);
}

void CapLong::stopCap()
{
    capture.reset();
    model.stop();
    isFinish = true;
    makeStopText();
    win->restoreWin();
    isScrolling = false;
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    win->refresh();
}

void CapLong::makeStopText()
{
    if (model.stopReason() == HuhuCapture::Capture::ScrollStopReason::HeightLimit) {
        layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooLong"), 13 * win->dpi);
    }
    else {
        layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.reachedBottom"), 13 * win->dpi);
    }
    if (!layoutTextEnd) return;
    DWRITE_TEXT_METRICS tm = {};
    layoutTextEnd->GetMetrics(&tm);
    auto& maskRect = win->cutMask->maskRect;
    auto halfX = maskRect.left + (maskRect.right - maskRect.left) / 2;
    auto halfW = tm.width / 2;
    float padding{ 8 * win->dpi };
    stopTextRect.left = halfX - halfW - padding;
    stopTextRect.top = maskRect.bottom - 30 * win->dpi - padding;
    stopTextRect.right = halfX + halfW + padding;
    stopTextRect.bottom = maskRect.bottom - padding;
    layoutTextEnd->SetMaxWidth(stopTextRect.right - stopTextRect.left);
    layoutTextEnd->SetMaxHeight(stopTextRect.bottom - stopTextRect.top);
    // 圆角矩形是按文本宽度加 padding 撑出来的，文本本身要摆回它的正中
    stopTextPos = { halfX - halfW, stopTextRect.top + (stopTextRect.bottom - stopTextRect.top - tm.height) / 2 };
}

bool CapLong::copyToClipboard()
{
    if (!model.hasImage()) return false;
    return HuhuCapture::MediaHistory::copyImage(win->hwnd, model.width(), model.height(), model.pixels().data());
}

bool CapLong::saveToFile()
{
    if (!model.hasImage()) return false;
    auto path = ImageExport::getSaveFilePath(win->hwnd);
    if (path.empty()) return false;
    return HuhuCapture::MediaHistory::saveImage(win->hwnd, path, model.width(), model.height(), model.pixels().data());
}

void CapLong::pin()
{
    if (!model.hasImage()) return;
    // 居中放置在主显示器
    auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(monitor, &mi);
    auto& workArea = mi.rcWork;
    int screenW = workArea.right - workArea.left;
    int screenH = workArea.bottom - workArea.top;
    int posX = workArea.left + (screenW - model.width()) / 2;
    int posY = workArea.top + (screenH - std::min(model.height(), screenH)) / 2;
    WinPin::initFromData(posX, posY, model.width(), model.height(), model.pixels());
}
