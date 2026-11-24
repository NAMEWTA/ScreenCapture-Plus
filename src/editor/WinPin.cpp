#include "pch.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolSub.h"
#include "ui/toolbars/ToolbarLayout.h"
#include "editor/shapes/ShapeBase.h"
#include "editor/shapes/ShapeText.h"
#include "editor/shapes/ShapeAnnotation.h"
#include "ui/AnnotationPanel.h"
#include "editor/WinPin.h"
#include "capture/WinCap.h"
#include "editor/History.h"
#include "app/App.h"
#include "services/ImageExport.h"
#include "services/Update.h"
#include "services/AnnotationRenderer.h"
#include "services/Lang.h"
#include "core/PixelBuffer.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <new>
#include <stdexcept>

using namespace Microsoft::WRL;
namespace {
	std::vector<std::unique_ptr<WinPin>> winPins;

	int clampPos(float val, float size, int min, int max)
	{
		auto upper = max - static_cast<int>(size);
		if (upper < min) upper = min;
		auto result = static_cast<int>(val);
		if (result < min) result = min;
		if (result > upper) result = upper;
		return result;
	}
}

WinPin::WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data) : Ling::WinBase(), history{ std::make_unique<History>(this) }
{
	this->x = x;
	this->y = y;
	this->w = w;
	this->h = h;
	if (data) {
		// 外部像素建底图。ShapeMosaic / ShapeEraser 会把它当画刷源，属性与 getCutImg() 出来的保持一致
		D2D1_BITMAP_PROPERTIES1 props{};
		props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
		props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
		props.dpiX = 96.0f;
		props.dpiY = 96.0f;
		Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data->data(), w * 4, &props, screenImg.GetAddressOf());
	}
	else {
		screenImg = WinCap::get()->getCutImg();
	}
	toolMain = std::make_unique<ToolMain>(this);
    toolSub = std::make_unique<ToolSub>(this);
	layoutTools();
	onMoved.add([this]() { layoutTools(); });
	// DPI 变了（用户改了缩放比例，或者窗口被拖到缩放比例不同的显示器上）：系统会按新旧缩放比
	// 把窗口整体放大一圈，但贴图窗口的尺寸是钉死在底图像素上的 —— 底图按原始像素画，
	// shape 的坐标也都是相对底图的物理像素，跟着缩放只会让窗口比图大一圈：
	// 右边、下边多出一条空白，边框看着比左上两边粗（描边居中，左上那半截被窗口边裁掉了），
	// 工具条也会按虚高的宽高往右下偏。所以等系统把建议矩形应用完（紧随而来的 WM_SIZE）
	// 再把尺寸掰回底图大小，并重排工具条。位置不能在 onDpiChanged 里改 ——
	// 那个事件在系统建议矩形生效之前触发，改了马上被覆盖
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		applyWinSize();
		layoutTools();
	});
	onMouseDown.add([this](POINT pos, BOOL isRight) {this->onDown(pos, isRight);});
	onMouseMove.add([this](POINT pos) {this->onMove(pos);});
	onMouseUp.add([this](POINT pos, BOOL isRight) {this->onUp(pos, isRight);});
	// Ling 传进来的是已经换算成滚动距离的 space（一格 = 60 逻辑像素 × dpi），
	// shape 只关心方向，这里按符号还原成 ±WHEEL_DELTA
	onMouseWheel.add([this](POINT pos, float space) {
		if (annotationPanelVisible && annotationPanel && annotationPanel->contains(pos)) {
			annotationPanel->wheel(pos, space);
			return; // The shared note TextBox receives its own wheel event.
		}
		// Ling 的滚轮事件不带修饰键状态，自己查：按住 Ctrl 是缩放窗口，不是调 shape
		if (GetKeyState(VK_CONTROL) & 0x8000) {
			// 一格 10%，按当前倍数等比走，放大和缩小的手感才对称
			applyScale(scale * (space > 0 ? 1.1f : 1.f / 1.1f), pos);
			return;
		}
		if (!isImagePosition(pos)) return;
		if (!shapeHover) return;
		auto imgPos = toImgPos(pos);
		shapeHover->mouseWheel((float)imgPos.x, (float)imgPos.y, space > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
	});
	onTimer.add([this](UINT id) {this->onTimerCB(id);});
	onKeyDown.add([this](UINT key) {this->onKey(key);});
	onDestroy.add([this]() { this->onClosed(); });
}

// WinBase::close() 里 DestroyWindow 之后同步触发 onDestroy，所以这个函数很可能是从
// ToolMain 的按钮回调里一路调进来的（点了 close 按钮）。此时 ToolMain::onClick 还在栈上，
// 而 toolMain 是 WinPin 的成员 —— 在这里直接把自己从 winPins 里擦掉就是 use-after-free。
// 因此：窗口句柄立即销毁（用户马上看到界面消失），C++ 对象的释放推迟到下一轮消息循环。
void WinPin::onClosed()
{
	// 防止 close() 被走两遍（比如按钮和快捷键先后触发）时排两次销毁
	if (isClosed) return;
	isClosed = true;
	if (editingText) editingText->finishEdit();
	if (annotationPanel) annotationPanel->detachOwner();
	// 先收起附属窗口，再让出 hover 指针 —— shapeHover 指向 history 里的元素，
	// history 随 WinPin 一起析构，留着悬空指针没意义
	if (toolSub) toolSub->close();
	if (toolMain) toolMain->close();
	shapeHover = nullptr;
	newShape = nullptr;
	editingText = nullptr;
	// screenImg / canvas / history 都是成员（canvas 挂在 body 的子节点上），随下面这次 erase 一并释放
	Ling::App::get()->dq.TryEnqueue([this]() {
		std::erase_if(winPins, [this](const std::unique_ptr<WinPin>& p) { return p.get() == this; });
		// 用完即走模式下，最后一个贴图窗口关掉就退出进程，不驻留在系统里。
		// 贴图可以同时开好几个（标注、长截图各来一张），所以得等它们都没了才退
		if (winPins.empty()) {
			if (Ling::App::get()->args[L"--auto-quit"] == L"true") {
				Ling::App::get()->quit(0);
			}
			else {
				Update::checkLater(); //只剩托盘图标了，顺便查一下更新
			}
		}
	});
}

bool WinPin::hasWindow()
{
	return !winPins.empty();
}

void WinPin::dispose()
{
	winPins.clear();
}

D2D1_SIZE_U WinPin::getImgSize() const
{
	if (!screenImg) return D2D1::SizeU(0, 0);
	// 要的是像素数，所以问 GetPixelSize 而不是 GetSize（后者返回的是按位图自身 dpi 折算的 DIP）
	return screenImg->GetPixelSize();
}

RECT WinPin::workArea() const
{
	auto image = getImgSize();
	RECT bounds{x, y, x + std::max(1, static_cast<int>(image.width * scale)),
		y + std::max(1, static_cast<int>(image.height * scale))};
	MONITORINFO info{.cbSize = sizeof(MONITORINFO)};
	if (GetMonitorInfo(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &info)) return info.rcWork;
	auto [left, top, width, height] = App::get()->getScreenArea();
	return RECT{left, top, left + width, top + height};
}

bool WinPin::isImagePosition(POINT point) const
{
	const auto image = getImgSize();
	return point.x >= 0 && point.y >= 0 && point.x < image.width * scale && point.y < image.height * scale;
}

std::uint64_t WinPin::allocateAnnotationId()
{
	return annotationSequence.allocate().value_or(0);
}

ShapeAnnotation* WinPin::findAnnotation(std::uint64_t number) const
{
	if (!number) return nullptr;
	for (const auto& shape : history->shapes()) {
		auto* note = dynamic_cast<ShapeAnnotation*>(shape.get());
		if (note && note->number() == number) return note;
	}
	return nullptr;
}

std::vector<ScreenCapture::Core::AnnotationNote> WinPin::annotationNotes() const
{
	std::vector<ScreenCapture::Core::AnnotationNote> result;
	for (const auto& shape : history->shapes()) {
		if (auto* note = dynamic_cast<ShapeAnnotation*>(shape.get())) result.push_back({note->number(), note->note()});
	}
	std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.number < b.number; });
	return result;
}

void WinPin::onAnnotationsChanged()
{
	auto notes = annotationNotes();
	if (!findAnnotation(selectedAnnotation)) selectedAnnotation = notes.empty() ? 0 : notes.back().number;
	if (annotationPanel) annotationPanel->sync(std::move(notes), selectedAnnotation);
	refresh();
}

void WinPin::selectAnnotation(std::uint64_t number, bool focusEditor)
{
	if (!findAnnotation(number)) return;
	if (editingText) editingText->finishEdit();
	selectedAnnotation = number;
	showAnnotationPanel(!annotationPanelVisible);
	if (annotationPanel) {
		annotationPanel->sync(annotationNotes(), selectedAnnotation);
		annotationPanel->select(number, focusEditor);
	}
	refresh();
}

void WinPin::updateAnnotationNote(std::uint64_t number, const std::wstring& text)
{
	if (auto* note = findAnnotation(number)) {
		note->setNote(text);
		if (annotationPanel) annotationPanel->updateNote(number, text);
		refresh();
	}
}

void WinPin::removeAnnotation(std::uint64_t number)
{
	if (auto* note = findAnnotation(number)) history->removeShape(note);
}

void WinPin::showAnnotationPanel(bool fitWorkspace)
{
	if (!annotationPanel || !hwnd) return;
	const bool wasVisible = annotationPanelVisible;
	annotationPanelVisible = true;
	annotationPanel->setPanelVisible(true);
	if ((fitWorkspace || !wasVisible) && isMouseDown) {
		// Keep drag coordinates in one image transform. A previously collapsed
		// rail can open now, but fitting/moving the image waits for mouse-up.
		fitAnnotationsAfterPointer = true;
		applyWinSize();
	} else if (fitWorkspace || !wasVisible) {
		fitAnnotationWorkspace();
	} else applyWinSize();
	layoutAnnotationPanel();
	layoutTools();
	refresh();
}

void WinPin::hideAnnotationPanel()
{
	if (!annotationPanelVisible) return;
	annotationPanelVisible = false;
	fitAnnotationsAfterPointer = false;
	annotationPanel->setPanelVisible(false);
	if (toolMain->curId == L"annotation") toolMain->cancelSelect();
	applyWinSize();
	layoutTools();
	refresh();
}

void WinPin::onToolSelectionChanged()
{
	lastDownTime = 0;
	prevPressCreatedShape = false;
	if (editingText) editingText->finishEdit();
	if (annotationPanel) annotationPanel->finishEditing();
	shapeHover = nullptr;
	newShape = nullptr;
	if (toolMain->curId == L"annotation") {
		showAnnotationPanel(!annotationPanelVisible);
		onAnnotationsChanged();
	}
	refresh();
}

void WinPin::fitAnnotationWorkspace()
{
	const auto area = workArea();
	const auto size = getImgSize();
	if (!size.width || !size.height) return;
	if (editingText) editingText->finishEdit();
	const float availableW = static_cast<float>(area.right - area.left) - 16.f * dpi;
	const float availableH = static_cast<float>(area.bottom - area.top) - 100.f * dpi;
	annotationPanelWidth = std::clamp(availableW / dpi * .36f, 240.f, 340.f);
	annotationPanelWidth = std::min(annotationPanelWidth, std::max(160.f, availableW / dpi - 80.f));
	const float imageW = std::max(20.f, availableW - (annotationPanelWidth + 10.f) * dpi);
	const float fitScale = std::min(imageW / size.width, std::max(60.f, availableH) / size.height);
	scale = std::min(scale, std::max(.01f, fitScale));
	applyWinSize();
	setPosition(clampPos(static_cast<float>(x), w, area.left + 8, area.right - 8),
		clampPos(static_cast<float>(y), h + 60.f * dpi, area.top + 8, area.bottom - 8));
}

void WinPin::layoutAnnotationPanel()
{
	if (!annotationPanel || !annotationPanelVisible) return;
	const auto size = getImgSize();
	annotationPanel->setBounds(size.width * scale + 10.f * dpi, annotationPanelWidth * dpi, h);
}

void WinPin::applyWinSize()
{
	auto sz = getImgSize();
	if (!hwnd || sz.width == 0 || sz.height == 0) return;
	auto newW = std::max(1, static_cast<int>(std::lround(sz.width * scale)));
	auto newH = std::max(1, static_cast<int>(std::lround(sz.height * scale)));
	if (annotationPanelVisible) {
		const auto area = workArea();
		const auto minimumHeight = std::min(420.f * dpi,
			std::max(240.f * dpi, static_cast<float>(area.bottom - area.top) - 100.f * dpi));
		newW += static_cast<int>(std::ceil((annotationPanelWidth + 10.f) * dpi));
		newH = std::max(newH, static_cast<int>(std::ceil(minimumHeight)));
	}
	w = static_cast<float>(newW);
	h = static_cast<float>(newH);
	// 不走 setSize：它收的是逻辑像素、内部还要乘一遍 dpi，而这里的宽高本来就是物理像素
	SetWindowPos(hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
	layoutAnnotationPanel();
}

POINT WinPin::toImgPos(const POINT& pos) const
{
	if (scale == 1.f) return pos;
	return POINT{ static_cast<LONG>(std::lround(pos.x / scale)), static_cast<LONG>(std::lround(pos.y / scale)) };
}

void WinPin::applyScale(float newScale, POINT anchor)
{
	auto sz = getImgSize();
	if (sz.width == 0 || sz.height == 0) return;
	// 上限跟着底图大小走：窗口边长再大，swap chain 那块显存也吃不消，人也看不过来；
	// 但至少要能回到 1 倍，所以下面用 max 兜一下
	auto maxScale = std::max(1.f, std::min(8.f, 16000.f / std::max(sz.width, sz.height)));
	newScale = std::clamp(newScale, annotationPanelVisible ? .01f : .1f, maxScale);
	if (std::abs(newScale - scale) < 0.0001f) return;
	// 编辑中的文字是 TextBox（真控件）画的，缩放期间它的位置、字号都得跟着重算，
	// 与其在缩放过程里一路同步，不如先收尾把文字交回 ShapeText 自己画 —— 之后它就跟着一起缩了
	if (editingText) editingText->finishEdit();
	if (annotationPanel) annotationPanel->finishEditing();
	// anchor 底下那个底图上的点，缩放前后都要停在光标下：屏幕坐标 = 窗口原点 + 底图点 × 倍数
	auto imgX = anchor.x / scale;
	auto imgY = anchor.y / scale;
	scale = newScale;
	applyWinSize();
	// setPosition 内部会 SetWindowPos，随后的 WM_MOVE 会带出 onMoved -> layoutTools
	setPosition(x + anchor.x - static_cast<int>(std::lround(imgX * scale)),
		y + anchor.y - static_cast<int>(std::lround(imgY * scale)));
	scaleTip = Ling::D2D::get()->makeTextLayout(std::format(L"{}%", static_cast<int>(std::lround(scale * 100.f))), 11.f * dpi);
	// 停手 800 毫秒后由定时器把倍数提示收掉。同一个 id 再调一次 SetTimer 就是重新计时，
	// 所以连续滚动期间它一直不会触发
	setTimer(800, 101);
	layoutTools();
	refresh();
}

// 画在窗口右上角，半透明底 + 白字，与 CutMask 上那个坐标标签一个路子。
// 调用方要先把缩放变换收回去：这是窗口装饰，不跟着图一起放大
void WinPin::paintScaleTip(ID2D1DeviceContext* ctx)
{
	if (!scaleTip || !brushTipBg) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(scaleTip->GetMetrics(&tm))) return;
	auto pad = 3.f * dpi;
	auto margin = 5.f * dpi;
	D2D1_RECT_F bgRect{ w - margin - tm.width - pad * 2, margin, w - margin, margin + tm.height + pad * 2 };
	// 图小到装不下提示时，贴着左边画，别画到窗口外面去
	if (bgRect.left < margin) bgRect.left = margin;
	ctx->FillRectangle(bgRect, brushTipBg.Get());
	ctx->DrawTextLayout({ bgRect.left + pad, bgRect.top + pad }, scaleTip.Get(), brushTipText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

// 把 ToolMain / ToolSub 摆到 WinPin 周围，始终靠 WinPin 右对齐，并尽量留在屏幕可视区内。
// 三种模式，按 ToolMain+ToolSub 的总高度决定（与 curId 是否为空无关，避免选中按钮时整组跳动）：
//   bottom : WinPin 下方，自上而下 ToolMain -> ToolSub
//   top    : 下方空间不足时改到 WinPin 上方，自上而下 ToolMain -> ToolSub -> WinPin
//   overlay: 上下都不足时覆盖在 WinPin 右下角，整组贴 WinPin 底边
// ToolSub 只在 curId 非空时显示，此时 ToolMain 上移为它腾出空间；ToolSub 永远紧贴 ToolMain 下方，
// 所以它那个朝上的小箭头在三种模式下都不需要翻转。
void WinPin::layoutTools()
{
	if (!toolMain || !toolSub) return;
	// WinPin 的 hwnd 此时可能还没创建（本函数会在构造期调用），所以用矩形而不是窗口句柄找显示器。
	RECT winRect{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
	MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
	auto monitor = MonitorFromRect(&winRect, MONITOR_DEFAULTTONEAREST);
	if (!monitor || !GetMonitorInfo(monitor, &mi)) {
		auto [sx, sy, sw, sh] = App::get()->getScreenArea();
		mi.rcWork = RECT{ sx, sy, sx + sw, sy + sh };
	}
	auto& wa = mi.rcWork;
	toolMain->fitToWorkArea(wa);
	toolSub->fitToWorkArea(wa);

	const auto gap = 5.f * dpi;              // 工具栏与 WinPin 之间的间距
	const auto subGap = ToolSub::mainGap;    // ToolMain 与 ToolSub 之间的间距
	const auto mainH = toolMain->h;
	const auto subH = toolSub->getDesiredHeight();
	const auto groupH = mainH + subGap + subH;
	const bool showSub = !toolMain->curId.empty() && toolSub->hasContent();
	// ToolSub 实际显示时 ToolMain 才上移让出它的位置（有些绘图按钮暂时还没有子工具栏）
	const auto usedH = showSub ? groupH : mainH;

	float mainY;
	if (winRect.bottom + gap + groupH <= wa.bottom) {         // bottom
		mainY = winRect.bottom + gap;
	}
	else if (winRect.top - gap - groupH >= wa.top) {          // top
		mainY = winRect.top - gap - usedH;
	}
	else {                                                    // overlay
		mainY = winRect.bottom - gap - usedH;
	}
	auto mainX = static_cast<float>(winRect.right) - toolMain->w;
	// 垂直方向按整组当前高度裁剪，避免 WinPin 超出工作区时把 ToolSub 挤到屏幕外
	ScreenCapture::UI::positionToolbar(toolMain.get(),
		clampPos(mainX, toolMain->w, wa.left, wa.right), clampPos(mainY, usedH, wa.top, wa.bottom));
	if (showSub) {
		toolSub->updatePosition(wa);
	}
	else {
		toolSub->hideTools();
	}
}

WinPin::~WinPin()
{
	// App shutdown can dispose windows without taking the normal close path.
	// Disconnect the shared TextBox callbacks while every member is still alive.
	if (editingText) editingText->finishEdit();
	if (annotationPanel) annotationPanel->detachOwner();
	shapeHover = nullptr;
	newShape = nullptr;
}

void WinPin::init(int x, int y, int w, int h)
{
	auto ptr = new WinPin(x,y,w,h);
	std::unique_ptr<WinPin> winPin{ ptr };
	ptr->createNativeWindow(WS_EX_TOPMOST| WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
	winPins.push_back(std::move(winPin));
}

void WinPin::initFromData(int x, int y, int w, int h, std::vector<BYTE>& data)
{
	auto ptr = new WinPin(x, y, w, h, &data);
	std::unique_ptr<WinPin> winPin{ ptr };
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
	winPins.push_back(std::move(winPin));
}

void WinPin::onCreated()
{
    disableBorderRadius();
    auto d2d = Ling::D2D::get();
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免拖动 shape 时整帧闪烁
    canvas = body->makeChild<Ling::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    imageOverlay = body->makeChild<Ling::Node>();
    imageOverlay->setPositionType(Ling::Position::Absolute);
    imageClip = compositor.CreateInsetClip();
    annotationPanel = body->makeChild<AnnotationPanel>();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), borderBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushTipBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushTipText.GetAddressOf());
    show();
}

void WinPin::layout()
{
    layoutAnnotationPanel();
    if (imageOverlay) {
        const auto sourceSize = getImgSize();
        imageOverlay->setSize(sourceSize.width * scale / dpi, sourceSize.height * scale / dpi);
    }
    Ling::WinBase::layout();
    if (imageOverlay) imageOverlay->visual.Clip(imageClip);
    if (!screenImg || !canvas) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(annotationPanelVisible ? D2D1::ColorF(0xEEF2F6) : D2D1::ColorF(0, 0.f));
    auto sz = screenImg->GetSize();
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, sz.width, sz.height);
    // 底图和 shape 都是按底图像素画的，放大缩小整个交给这个变换，
    // 笔宽、夹点跟着一起缩 —— 鼠标坐标进来时也除掉了倍数，所以命中判定天然对得上
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    ctx->PushAxisAlignedClip(destRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ctx->DrawBitmap(screenImg.Get(), destRect);
	for (auto& shape : history->shapes())
	{
		shape->paint(ctx);
	}
	if (annotationPanelVisible) {
		if (auto* selected = findAnnotation(selectedAnnotation); selected && selected != shapeHover)
			selected->paintDragger(ctx);
	}
	if (!isMouseDown && shapeHover) {
		shapeHover->paintDragger(ctx);
	}
	ctx->PopAxisAlignedClip();
	// 蓝边框和倍数提示属于窗口装饰，不跟着图缩放：变换收回来，按窗口坐标画。
	// 边框也因此从"底图矩形"改成"窗口矩形"，任何倍数下都是 2*dpi 粗
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->DrawRectangle(D2D1::RectF(0.f, 0.f, w, h), borderBrush.Get(), 2*dpi);
	paintScaleTip(ctx);
    canvas->finishPaint();
}

void WinPin::onMinMaxInfo(MINMAXINFO* mmi)
{
	auto [x, y, w, h] = App::get()->getScreenArea();
	mmi->ptMaxPosition.x = x;
	mmi->ptMaxPosition.y = y;
	mmi->ptMaxSize.x = w;
	mmi->ptMaxSize.y = h;
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
	// Ctrl+滚轮放大后窗口可以比屏幕大（超出的部分自然被裁掉）。默认的最大跟踪尺寸只有
	// 主显示器那么大，不放开的话 SetWindowPos 会被系统按住，放大就到此为止了
	mmi->ptMaxTrackSize.x = 20000;
	mmi->ptMaxTrackSize.y = 20000;
}

void WinPin::onDown(POINT pos, BOOL isRight)
{
	if (annotationPanelVisible && annotationPanel && annotationPanel->contains(pos)) {
		if (editingText) editingText->finishEdit();
		shapeHover = nullptr;
		newShape = nullptr;
		lastDownTime = 0;
		prevPressCreatedShape = false;
		annotationPanel->pointerDown(pos, isRight != FALSE);
		return;
	}
	// 编辑文本时，落在文本框里的点击整个交给 TextBox（它自己订阅了窗口的鼠标事件）。
	// 这里不能抢先 SetCapture / 置 isMouseDown，否则拖选文本会被当成拖 shape。
	if (editingText && textBox && isImagePosition(pos) && textBox->isPosIn(pos)) return;
	if (editingText && !isImagePosition(pos)) editingText->finishEdit();
	if (annotationPanel) annotationPanel->finishEditing();
	if (isRight) {
		// 右键在"有工具条"和"只剩图"这两个状态之间来回切。
		// 藏着的时候（上一次右键收起来的）就把它请回来。位置先重排一遍：
		// 藏着的这段时间里窗口可能被 Ctrl+滚轮缩放过，工具条的落点跟着变了
		if (!IsWindowVisible(toolMain->hwnd)) {
			if (!annotationNotes().empty()) showAnnotationPanel(false);
			layoutTools();
			toolMain->show();
			return;
		}
		// 显示着：清掉画笔选中态，把两条工具条一起收起来，只剩图本身。
		// cancelSelect 里已经顺手隐藏了 ToolSub 并重排整组，但它在 curId 本来就空时会提前返回，
		// 所以 ToolSub 这一下自己再收一次，右键的效果与当时选没选画笔无关。
		// 左键点一下（抬手时，见 onUp）也能把 ToolMain 请回来
		toolMain->cancelSelect();
		toolMain->hide();
		toolSub->hideTools();
		hideAnnotationPanel();
		return;
	}
	// The expanded window contains editor chrome and possibly empty space below
	// a short image. Those areas must never create or move image annotations.
	if (!isImagePosition(pos) && !toolMain->curId.empty()) return;
	// 双击判定得自己做，做法同 WinCap::onDown：Ling 的窗口类没带 CS_DBLCLKS，
	// WM_LBUTTONDBLCLK 根本不会来，只能拿系统的双击间隔和双击判定框自己认。
	// 与 WinCap 唯一的不同是这里比屏幕坐标而不是客户区坐标：拖动贴图窗口时窗口跟着光标走，
	// 抓住的那一点始终停在光标下，光标的客户区坐标几乎不变 —— 用客户区坐标会把
	// "拖一下松手再拖一下"当成双击，图和标注就这么被复制走关掉了。
	// 而拖动必然意味着光标在屏幕上真的移动过，屏幕坐标能把这种情况分开
	POINT screenPos{};
	GetCursorPos(&screenPos);
	auto now = GetTickCount64();
	bool isDblClick = (now - lastDownTime <= GetDoubleClickTime())
		&& std::abs(screenPos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
		&& std::abs(screenPos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
	lastDownTime = now;
	lastDownPos = screenPos;
	// 双击 = Ctrl+C：把图连标注一起送进剪切板并关窗，选着画笔也一样（等价于按 Ctrl+C，
	// 手里拿着什么工具都不该影响这个手势）。要在下面所有分支之前处理：
	// 这一下既不是画画也不是拖窗，不该留下 capture、更不该新建 shape。
	// 编辑文字时不算：双击归文本框（选中单词），点在框外才会走到这里
	if (isDblClick && !editingText && toolMain->curId != L"annotation"
		&& dynamic_cast<ShapeAnnotation*>(shapeHover) == nullptr) {
		// 前半段那一下点击是这个手势的一部分，它顺手放下的元素（只有序号是按一下就成形的，
		// 别的都在抬手时按"没画出东西"清掉了）不该被带进剪切板
		if (prevPressCreatedShape) history->undo();
		copyToClipboard();
		return;
	}
	// 记的是按下点在窗口内的偏移（客户区坐标），拖动时用它把抓住的那一点保持在光标下
	pressPos.x = pos.x;
	pressPos.y = pos.y;
	isMouseDown = true;
	hasDragged = false;
	SetCapture(hwnd);
	if (toolMain->curId == L"") { //没选画笔，左键是拖窗口，拖的时候把工具条收起来
		toolMain->hide();
		return;
	}
	// 以下都是交给 shape 的坐标，一律换算成底图像素（拖窗口那条路仍用窗口坐标）
	auto imgPos = toImgPos(pos);
	if (!editingText) {
		// Input in the docked editor can retain an old hover. Resolve the actual
		// click position so the next image click creates/selects the right pin.
		shapeHover = nullptr;
		const auto shapes = history->shapes();
		for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) {
			(*it)->mouseMove(static_cast<float>(imgPos.x), static_cast<float>(imgPos.y));
			if ((*it)->hoverDraggerIndex >= 0) { shapeHover = it->get(); break; }
		}
	}
	if (shapeHover) {
		newShape = nullptr; //改的是已有元素，不参与空元素判定
		shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
		return;
	}
	shapeHover = history->createShape(toolMain->curId, imgPos.x, imgPos.y);
	newShape = shapeHover;
}

void WinPin::onMove(POINT pos)
{
	if (annotationPanel) annotationPanel->pointerMove(pos);
	// Ling uses a sentinel for WM_MOUSELEAVE. It is not an image coordinate:
	// treating it as a captured move would drag a pin to the corner and suppress
	// the click-to-edit focus handoff on mouse-up.
	if (pos.x == INT_MAX && pos.y == INT_MAX) {
		if (!isMouseDown && shapeHover) { shapeHover = nullptr; refresh(); }
		return;
	}
	if (!isMouseDown && annotationPanelVisible && annotationPanel && annotationPanel->contains(pos)) {
		if (shapeHover) { shapeHover = nullptr; refresh(); }
		return;
	}
	// 同 onDown：文本框里的移动归 TextBox（拖选、滚动条 hover），不参与 shape 的 hover 判定
	if (editingText && textBox && textBox->isPosIn(pos)) return;
	// 拖窗口用的是窗口坐标（pressPos 也是），只有交给 shape 的才换算成底图像素
	auto imgPos = toImgPos(pos);
	if (isMouseDown) {
		if (toolMain->curId == L"") {
			setPosition(x + pos.x - pressPos.x, y + pos.y - pressPos.y);
			return;
		}
		else if(shapeHover) {
			// 光标一步没挪也会来 WM_MOUSEMOVE，所以跟按下点比一下再算拖动
			if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
			shapeHover->mouseDrag((float)imgPos.x, (float)imgPos.y);
			refresh();
		}
	}
	else
	{
		if (toolMain->curId == L"") return;
		if (!isImagePosition(pos)) {
			if (shapeHover) { shapeHover = nullptr; refresh(); }
			return;
		}
		const auto activeShapes = history->shapes();
		for (auto it = activeShapes.rbegin(); it != activeShapes.rend(); ++it)
		{
			auto cur = it->get();
			cur->mouseMove((float)imgPos.x, (float)imgPos.y);
			if (cur->hoverDraggerIndex >= 0) {
				if (shapeHover != cur) {
					shapeHover = cur;
					setTimer(800, 100);
					refresh();
				}
				return;
			}
		}
		if (shapeHover) {
			shapeHover = nullptr;
		}
	}
}

void WinPin::onUp(POINT pos, BOOL isRight)
{
	// 右键按下时什么都没抓（既没置 isMouseDown 也没 SetCapture，见 onDown），抬手也就没什么要收的。
	// 更要紧的是不能往下走：下面那条"拖窗结束"的路会把 ToolMain 显示出来，
	// 而右键刚刚才把它收起来 —— 一按一放就等于什么都没做
	if (isRight) return;
	if (!isMouseDown) {
		if (annotationPanel) annotationPanel->pointerUp(pos, false);
		return; // TextBox owns selection/caret mouse capture in this path.
	}
	isMouseDown = false;
	ReleaseCapture();
	auto justCreated = newShape;
	newShape = nullptr;
	// 这一下按下有没有新建出一个留得住的元素：紧接着来第二下凑成双击时要把它撤掉（见 onDown）
	prevPressCreatedShape = false;
	if (toolMain->curId == L"") {
		// 没选画笔，这一下要么是拖完窗口（按新位置重排工具条），要么只是点了一下 ——
		// 两种情况都把 ToolMain 显示出来：拖动期间它是藏着的，右键之后它也是藏着的，
		// 左键点一下就是"我还要用工具条"。ToolSub 由 curId 驱动，这会儿仍然不该出来，
		// layoutTools 里已经管了
		layoutTools();
		toolMain->show();
	}
	else if (shapeHover) {
		// 新建的这一笔按下马上弹起，什么也没画出来：直接丢掉，
		// 也省了 mouseUp 里的收尾开销（马赛克那边要把 GPU 像素读回内存，不该为一个要删的元素白做）
		if (shapeHover == justCreated && !hasDragged && !shapeHover->isValidWithoutDrag()) {
			history->removeShape(shapeHover); //它会顺手清掉 shapeHover 并刷新
			return;
		}
		prevPressCreatedShape = (shapeHover == justCreated);
		auto imgPos = toImgPos(pos);
		shapeHover->mouseUp((float)imgPos.x, (float)imgPos.y);
		if (fitAnnotationsAfterPointer) {
			fitAnnotationsAfterPointer = false;
			fitAnnotationWorkspace();
			layoutTools();
		}
		if (auto* note = dynamic_cast<ShapeAnnotation*>(shapeHover)) {
			// Focus after the mouse-up broadcast begins; focusing on mouse-down
			// would immediately be undone by TextBox's outside-click handler.
			selectAnnotation(note->number(), !hasDragged);
		}
		refresh();
		setTimer(800, 100);
	}

}

void WinPin::onTimerCB(UINT id)
{
	if (id == 101) { //缩放停手了，收掉右上角的倍数提示
		killTimer(101);
		scaleTip = nullptr;
		refresh();
		return;
	}
	if (id != 100) return;
	if (!shapeHover) {
		refresh();
		killTimer(100);
	}
}

Ling::TextBox* WinPin::getTextBox()
{
	if (textBox) return textBox;
	// 建在 canvas 之后：Composition 的子 visual 按插入顺序叠放，文本框要盖在截图上面。
	// 绝对定位，位置由 ShapeText 按自己的矩形指定，不参与 body 的 flex 排布。
	textBox = imageOverlay->makeChild<Ling::TextBox>();
	textBox->setPositionType(Ling::Position::Absolute);
	// 不折行、尺寸跟着文字长，与 2.4.25 的文本窗口一致
	textBox->setAutoSize(true);
	// 背景、边框都不画：编辑中看到的就是最终效果，那圈虚线框由 ShapeText 自己画
	textBox->hide();
	return textBox;
}

void WinPin::setEditingText(ShapeText* shape)
{
	editingText = shape;
}

void WinPin::onToolStyleChanged()
{
	if (editingText) editingText->applyStyle();
}

void WinPin::onKey(UINT key)
{
	// 编辑文本时所有按键都归 TextBox：否则 Ctrl+C 复制的是截图、回车会保存并关窗、
	// Delete 删掉的是整个 shape、ESC 直接把窗口关了。ESC 结束编辑由 TextBox 自己处理。
	if (editingText) return;
	bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	if (annotationPanel && annotationPanel->isEditing()) {
		// Enter inserts a line break; copy/delete/selection belong to the input.
		// Explicit Save is still available while writing a note.
		if (ctrl && key == 'S') saveToFile();
		return;
	}
	if (ctrl && key == 'Z') {
		history->undo();
	}
	else if (ctrl && key == 'Y') {
		history->redo();
	}
	else if (ctrl && key == 'C') {
		copyToClipboard();
	}
	else if (ctrl && key == 'S') {
		saveToFile();
	}
	else if (key == VK_RETURN) {
		copyToClipboard();
	}
	else if (key == VK_DELETE) {
		if (shapeHover) history->removeHoverShape();
		else if (annotationPanelVisible) removeAnnotation(selectedAnnotation);
	}
	else if (key == VK_ESCAPE) {
		close();
	}
}

void WinPin::copyToClipboard()
{
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getImagePixels(pixels, size)) return;
	ImageExport::saveToClipboard((int)size.width, (int)size.height, pixels.data());
	close();
}

void WinPin::saveToFile()
{
	auto foregroundBeforeDialog = GetForegroundWindow();
	auto path = ImageExport::getSaveFilePath(hwnd);
	if (path.empty()) {   // 用户取消
		restoreWindowState(foregroundBeforeDialog);
		return;
	}
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getImagePixels(pixels, size)) {
		restoreWindowState(foregroundBeforeDialog);
		return;
	}
	if (ImageExport::saveToFile(path, (int)size.width, (int)size.height, pixels.data())) {
		close();
	}
	else {
		restoreWindowState(foregroundBeforeDialog);
	}
}

// 另存为对话框关掉后会把 owner(hwnd) 变成活动窗口，WinPin 一被激活就会盖住 ToolMain。
// 这里把三个窗口重新压到 topmost，并把前台还给开对话框之前的那个窗口。
void WinPin::restoreWindowState(HWND foregroundBeforeDialog)
{
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (toolMain && toolMain->hwnd && IsWindowVisible(toolMain->hwnd)) {
		SetWindowPos(toolMain->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (toolSub && toolSub->hwnd && IsWindowVisible(toolSub->hwnd)) {
		SetWindowPos(toolSub->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (foregroundBeforeDialog && foregroundBeforeDialog != hwnd && IsWindowVisible(foregroundBeforeDialog)) {
		SetForegroundWindow(foregroundBeforeDialog);
	}
}

// 离屏把底图和 shape 合成到一张新位图上再读回像素。
// 不直接画到 screenImg 上：它是 ShapeEraser 的"原样"来源，也是 ShapeMosaic 的取样来源，
// 一旦被 shape 覆写，之后再擦除/打码就会拿到已经画过的画面。
// 批注服务再把完整表格附在右侧；所有状态、绘制会话和映射都在本函数内成对恢复。
bool WinPin::getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size)
{
    if (editingText) editingText->finishEdit();
    if (annotationPanel) annotationPanel->finishEditing();
    bool hasAnnotations = false;
    for (const auto& shape : history->shapes())
        if (dynamic_cast<ShapeAnnotation*>(shape.get())) { hasAnnotations = true; break; }

    // Keep every device/mapping guard inside this operation. An error dialog is
    // shown only after restoring the shared context, because it pumps UI events.
    const bool success = [&]() -> bool {
        try {
            const auto imgSize = getImgSize();
            if (!imgSize.width || !imgSize.height) return false;
            auto d2d = Ling::D2D::get();
            auto* ctx = d2d->deviceContext.Get();
            AnnotationRenderer::Plan plan;
            if (hasAnnotations) {
                AnnotationRenderer::Options options;
                options.title = Lang::get(L"annotation.title");
                options.emptyNoteText = Lang::get(L"annotation.empty");
                options.maximumDimension = ctx->GetMaximumBitmapSize();
                auto notes = annotationNotes();
                if (FAILED(AnnotationRenderer::makePlan(d2d->dwriteFactory.Get(),
                    d2d->baseTextFormat.Get(), imgSize, notes, options, plan))) return false;
            }

            struct ContextRestore {
                ID2D1DeviceContext* context;
                ComPtr<ID2D1Image> target;
                D2D1_MATRIX_3X2_F transform{};
                D2D1_UNIT_MODE units;
                float dpiX{}, dpiY{};
                explicit ContextRestore(ID2D1DeviceContext* value) : context(value), units(value->GetUnitMode()) {
                    context->GetTarget(&target);
                    context->GetTransform(&transform);
                    context->GetDpi(&dpiX, &dpiY);
                }
                ~ContextRestore() {
                    context->SetTarget(target.Get());
                    context->SetTransform(transform);
                    context->SetDpi(dpiX, dpiY);
                    context->SetUnitMode(units);
                }
            } restore(ctx);
            ctx->SetDpi(96.f, 96.f);
            ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            D2D1_BITMAP_PROPERTIES1 targetProps{
                .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
                .dpiX{96.f}, .dpiY{96.f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_TARGET}
            };
            ComPtr<ID2D1Bitmap1> targetBmp;
            auto hr = ctx->CreateBitmap(imgSize, nullptr, 0, &targetProps, &targetBmp);
            if (FAILED(hr)) return false;
            ctx->SetTarget(targetBmp.Get());
            {
                struct Drawing {
                    ID2D1DeviceContext* context;
                    bool active{true};
                    explicit Drawing(ID2D1DeviceContext* value) : context(value) { context->BeginDraw(); }
                    ~Drawing() { if (active) context->EndDraw(); }
                    HRESULT finish() { active = false; return context->EndDraw(); }
                } drawing(ctx);
                ctx->Clear(D2D1::ColorF(0, 0.f));
                ctx->DrawBitmap(screenImg.Get(), D2D1::RectF(0, 0, static_cast<float>(imgSize.width), static_cast<float>(imgSize.height)));
                for (const auto& shape : history->shapes()) shape->paint(ctx);
                hr = drawing.finish();
            }
            ctx->SetTarget(nullptr);
            if (FAILED(hr)) return false;

            D2D1_SIZE_U outputSize = imgSize;
            if (hasAnnotations) {
                ComPtr<ID2D1Bitmap1> composite;
                if (FAILED(AnnotationRenderer::render(ctx, targetBmp.Get(), plan, composite))) return false;
                targetBmp = std::move(composite);
                outputSize = D2D1::SizeU(plan.layout.width, plan.layout.height);
            }
            const auto layout = ScreenCapture::Core::pixelLayout(
                static_cast<int>(outputSize.width), static_cast<int>(outputSize.height), 4);
            if (!layout || layout->stride > (std::numeric_limits<UINT32>::max)()) return false;
            // Allocate before Map: allocation failure must not leave a CPU bitmap mapped.
            std::vector<BYTE> result(layout->bytes);
            D2D1_BITMAP_PROPERTIES1 cpuProps{
                .pixelFormat{targetBmp->GetPixelFormat()}, .dpiX{96.f}, .dpiY{96.f},
                .bitmapOptions{D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW}
            };
            ComPtr<ID2D1Bitmap1> cpuBmp;
            if (FAILED(ctx->CreateBitmap(outputSize, nullptr, 0, &cpuProps, &cpuBmp))) return false;
            if (FAILED(cpuBmp->CopyFromBitmap(nullptr, targetBmp.Get(), nullptr))) return false;
            D2D1_MAPPED_RECT mapped{};
            if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
            if (mapped.pitch < layout->stride) { cpuBmp->Unmap(); return false; }
            for (UINT32 row = 0; row < outputSize.height; ++row)
                CopyMemory(result.data() + static_cast<std::size_t>(row) * layout->stride,
                    mapped.bits + static_cast<std::size_t>(row) * mapped.pitch, layout->stride);
            cpuBmp->Unmap();
            pixels.swap(result);
            size = outputSize;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
    }();
    if (!success && hasAnnotations)
        MessageBox(hwnd, Lang::get(L"annotation.exportFailed").c_str(), L"ScreenCapture-Plus", MB_OK | MB_ICONWARNING);
    return success;
}

BOOL WinPin::setCursor()
{
	if (annotationPanel && annotationPanel->cursor()) return TRUE;
	// 编辑文本时光标形状交给 TextBox 决定（文本区 I 形、滚动条箭头）。
	// 本函数覆写了基类且不调用它，TextBox 挂在 onCursor 上的那个订阅不会自己被触发，得手动发一次。
	if (editingText || (annotationPanel && annotationPanel->isEditing())) {
		bool handled{ false };
		onCursor(&handled);
		if (handled) return TRUE;
	}
	if (toolMain->curId == L"") {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		return TRUE;
	}
	if (shapeHover) {
		shapeHover->setCursor();
		return TRUE;
	}
	if (toolMain->curId == L"text") {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	}
	else {
		SetCursor(LoadCursor(nullptr, IDC_CROSS));
	}
	return TRUE;
}
