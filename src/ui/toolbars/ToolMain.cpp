#include "pch.h"
#include "editor/WinPin.h"
#include "editor/History.h"
#include "services/Lang.h"
#include "ui/Tip.h"
#include "ui/toolbars/ToolMain.h"
#include "ui/toolbars/ToolbarLayout.h"
#include "ui/toolbars/ToolSub.h"

ToolMain::ToolMain(WinPin* win) : Ling::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，WinPin 可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 初始位置由 WinPin::layoutTools() 统一决定，这里只算尺寸
	x = win->x;
	y = win->y + win->h + 5.f * win->dpi;
	refreshSize();
	// 点按钮会把 ToolMain 激活，此后键盘消息进的是它而不是 WinPin。
	// 直接把按键转触给 WinPin 的同名事件，快捷键在两个窗口上表现一致。
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	// DPI 变了（工具条被挪到缩放比例不同的显示器上，或者用户改了系统缩放）：
	// Ling 只会把窗口按系统给的建议矩形整体缩放一遍，我们自己定的那套摆放规则不会重跑，
	// 工具条就歪在别处了。位置也不能在 onDpiChanged 里直接改 —— 那个事件在 Ling 应用建议矩形
	// 之前触发，改了马上被覆盖，所以这里只记个标记，等建议矩形应用后紧随而来的 WM_SIZE 再动手
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();                    //宿主的摆放规则要用宽高，先按新 dpi 把尺寸定下来
		this->win->layoutTools();
	});
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

void ToolMain::refreshSize()
{
	std::vector<float> widths;
	widths.reserve(btnIds.size());
	for (const auto& id : btnIds) widths.push_back(id == L"|" ? spliterW : btnSize);
	ScreenCapture::UI::applyToolbarExtent(this, ScreenCapture::UI::measureToolbar(widths, maximumWidth));
}

void ToolMain::fitToWorkArea(const RECT& workArea)
{
	const auto availableWidth = ScreenCapture::UI::toolbarWorkWidth(workArea, dpi);
	if (availableWidth == maximumWidth) return;
	maximumWidth = availableWidth;
	refreshSize();
}

ToolMain::~ToolMain()
{
}

void ToolMain::init()
{
}

// Buttons can wrap onto another row; their laid-out positions are authoritative.
float ToolMain::getBtnCenterX()
{
	Ling::WinBase::layout();
	for (const auto btn : btns) {
		if (btn->id == curId) return btn->x + btn->w / 2.f;
	}
	return w / 2.f;
}

void ToolMain::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0xFFFFFFFF);
	body->setBorder(ScreenCapture::UI::toolbarBorder, 0xA8A8A8ff);
	ScreenCapture::UI::configureToolbarRow(body);
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		auto& id = btnIds[i];
		if (id == L"|") {
			ScreenCapture::UI::makeToolbarDivider(body);
		}
		else {
			auto btn = body->makeChild<Ling::Button>();
			btn->setId(id);
			btn->setText(btnCodes[i]);
			ScreenCapture::UI::sizeToolbarControl(btn);
			btn->setHoverBg(0xF2F2F2ff);
			btn->setFontFamily(L"icon");
			btn->setFontSize(ScreenCapture::UI::toolbarIconSize);
			if (id == L"annotation") {
				annotationIcon = btn->makeChild<Ling::Canvas>();
				annotationIcon->setPositionType(Ling::Position::Absolute);
				annotationIcon->setPosition(Ling::Edge::Left, (btnSize - ScreenCapture::UI::toolbarIconSize) / 2.f);
				annotationIcon->setPosition(Ling::Edge::Top, (btnSize - ScreenCapture::UI::toolbarIconSize) / 2.f);
				annotationIcon->setSize(ScreenCapture::UI::toolbarIconSize, ScreenCapture::UI::toolbarIconSize);
			}
			btn->onClick.add([this](Ling::Button* btn) {onClick(btn);});
			tip->bind(btn, Lang::get(std::format(L"tool.{}", id)));
			btns.push_back(btn);
		}
	}
	show();
}

void ToolMain::layout()
{
	Ling::WinBase::layout();
	paintAnnotationIcon();
}

void ToolMain::paintAnnotationIcon()
{
	if (!annotationIcon) return;
	auto d2d = Ling::D2D::get();
	if (!annotationIconBrush && FAILED(d2d->deviceContext->CreateSolidColorBrush(
		D2D1::ColorF(0x333333), annotationIconBrush.GetAddressOf()))) return;
	// A numbered speech bubble, separate from the existing plain number circle.
	// Rebuild its vector path only on DPI changes.
	if (!annotationIconPath || annotationIconDpi != dpi) {
		Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
		Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(d2d->d2dFactory->CreatePathGeometry(path.GetAddressOf())) ||
			FAILED(path->Open(sink.GetAddressOf()))) return;
		const auto point = [this](float x, float y) { return D2D1::Point2F(x * dpi, y * dpi); };
		sink->BeginFigure(point(5.f, 3.f), D2D1_FIGURE_BEGIN_HOLLOW);
		sink->AddLine(point(19.f, 3.f));
		sink->AddBezier({ point(22.f, 3.f), point(22.f, 5.f), point(22.f, 6.f) });
		sink->AddLine(point(22.f, 14.f));
		sink->AddBezier({ point(22.f, 17.f), point(20.f, 17.f), point(19.f, 17.f) });
		sink->AddLine(point(11.f, 17.f));
		sink->AddLine(point(7.f, 21.f));
		sink->AddLine(point(7.f, 17.f));
		sink->AddLine(point(5.f, 17.f));
		sink->AddBezier({ point(2.f, 17.f), point(2.f, 15.f), point(2.f, 14.f) });
		sink->AddLine(point(2.f, 6.f));
		sink->AddBezier({ point(2.f, 3.f), point(4.f, 3.f), point(5.f, 3.f) });
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		if (FAILED(sink->Close())) return;
		annotationIconPath = std::move(path);
		annotationIconDpi = dpi;
	}
	auto ctx = annotationIcon->startPaint();
	if (!ctx) return;
	ctx->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
	const auto brush = annotationIconBrush.Get();
	const auto stroke = 1.6f * dpi;
	const auto point = [this](float x, float y) { return D2D1::Point2F(x * dpi, y * dpi); };
	ctx->DrawGeometry(annotationIconPath.Get(), brush, stroke);
	ctx->DrawLine(point(7.f, 8.f), point(9.f, 6.f), brush, stroke);
	ctx->DrawLine(point(9.f, 6.f), point(9.f, 13.f), brush, stroke);
	ctx->DrawLine(point(7.f, 13.f), point(11.f, 13.f), brush, stroke);
	ctx->DrawLine(point(14.f, 7.f), point(18.f, 7.f), brush, stroke);
	ctx->DrawLine(point(14.f, 11.f), point(18.f, 11.f), brush, stroke);
	annotationIcon->finishPaint();
}

void ToolMain::applyNormalStyle(Ling::Button* btn)
{
	btn->setBg(0);
	btn->setHoverBg(0xF2F2F2ff);
}

// 取消选中：与 onClick 选中某个按钮是对称操作，只是没有新的选中项。
// ToolSub 由 curId 是否为空驱动，所以清空 curId 后 layoutTools() 会自动把它收起来。
void ToolMain::cancelSelect()
{
	if (curId.empty()) return;
	for (auto b : btns)
	{
		if (b->id == curId) {
			applyNormalStyle(b);
		}
	}
	curId.clear();
	win->toolSub->hideTools();
	win->onToolSelectionChanged();
	// curId 空了 ToolMain 要下移收回 ToolSub 让出的空间，交给 WinPin 重排整组
	win->layoutTools();
}

void ToolMain::onClick(Ling::Button* btn)
{
	tip->hide();
	// 关闭整个贴图窗口。WinPin 的 onDestroy 里会连带关掉 ToolMain / ToolSub，
	// 但 C++ 对象的释放被推迟到下一轮消息循环，所以这里 return 之后栈上访问 this 仍是安全的。
	if (btn->id == L"close") {
		win->close();
		return;
	}
	// 下面这几个都是"执行一次动作"而不是"切换绘图工具"，做完就返回，不动 curId 和选中态。
	// undo/redo 由 History 内部负责 refresh；save/clipboard 成功后会关窗，同样不能往下走。
	else if (btn->id == L"undo") {
		win->history->undo();
		return;
	}
	else if (btn->id == L"redo") {
		win->history->redo();
		return;
	}
	else if (btn->id == L"save") {
		win->saveToFile();
		return;
	}
	else if (btn->id == L"clipboard") {
		win->copyToClipboard();
		return;
	}
	// 再次点击已选中的按钮 = 取消选中（开关式）。cancelSelect 里已经做了配色复位、
	// 隐藏 ToolSub 和重排，这里直接返回，不要再往下走选中流程。
	if (btn->id == curId) {
		cancelSelect();
		return;
	}
	for (auto b:btns)
	{
		if (b->id == curId)
		{
			applyNormalStyle(b);
		}
		if (b->id == btn->id)
		{
			b->setBg(0xe6f4ffff);
			b->setHoverBg(0xe6f4ffff);
		}
	}
	curId = btn->id;
	if (curId == L"rect") {
		win->toolSub->showRectTools();
	}
	else if (curId == L"ellipse") {
		win->toolSub->showEllipseTools();
	}
	else if (curId == L"arrow") {
		win->toolSub->showArrowTools();
	}
	else if (curId == L"number") {
		win->toolSub->showNumberTools();
	}
	else if (curId == L"line") {
		win->toolSub->showLineTools();
	}
	else if (curId == L"text") {
		win->toolSub->showTextTools();
	}
	else if (curId == L"mosaic") {
		win->toolSub->showMosaicTools();
	}
	else if (curId == L"eraser") {
		win->toolSub->showEraserTools();
	}
	else {
		win->toolSub->hideTools();
	}
	win->onToolSelectionChanged();
	// curId 变化后 ToolMain 可能要上移给 ToolSub 腾位置，交给 WinPin 重新排布整组
	win->layoutTools();
}

void ToolMain::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
