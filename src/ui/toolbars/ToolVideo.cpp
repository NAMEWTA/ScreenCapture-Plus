#include "pch.h"
#include "capture/WinCap.h"
#include "services/ImageExport.h"
#include "services/Lang.h"
#include "ui/Tip.h"
#include "ui/toolbars/ToolVideo.h"
#include "ui/toolbars/ToolbarLayout.h"

ToolVideo::ToolVideo(WinCap* win) : Ling::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，宿主可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 位置由 CapVideo::makeTool() 在 createNativeWindow 之前设好，这里只算尺寸
	refreshSize();
	// 点按钮会把 ToolVideo 激活，键盘消息进的是它，转发给 WinCap 让 ESC 一致生效
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onTimer.add([this](UINT id) { this->onTimerCB(id); });
	// DPI 变了（工具条被挪到缩放比例不同的显示器上，或者用户改了系统缩放）：
	// Ling 只会把窗口按系统给的建议矩形整体缩放一遍，我们自己定的那套摆放规则不会重跑，
	// 工具条就歪在别处了。位置也不能在 onDpiChanged 里直接改 —— 那个事件在 Ling 应用建议矩形
	// 之前触发，改了马上被覆盖，所以这里只记个标记，等建议矩形应用后紧随而来的 WM_SIZE 再动手
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();                    //宿主的摆放规则要用宽高，先按新 dpi 把尺寸定下来
		this->win->layoutTool(this);
	});
}

void ToolVideo::refreshSize()
{
	const std::vector<float> widths = isRecording
		? std::vector<float>{ timerW, spliterW, btnSize, btnSize, btnSize }
		: std::vector<float>{ formatW, formatW, spliterW, btnSize, btnSize, spliterW, btnSize, btnSize };
	HuhuCapture::UI::applyToolbarExtent(this, HuhuCapture::UI::measureToolbar(widths, maximumWidth));
}

void ToolVideo::fitToWorkArea(const RECT& workArea)
{
	const auto availableWidth = HuhuCapture::UI::toolbarWorkWidth(workArea, dpi);
	if (availableWidth == maximumWidth) return;
	maximumWidth = availableWidth;
	refreshSize();
}

ToolVideo::~ToolVideo()
{
}

void ToolVideo::onCreated()
{
	tip = std::make_unique<Tip>(this);
	// 提示气泡是独立顶层窗口，本窗口被摘出屏幕捕获它不跟着走，得单独说一声
	tip->excludeFromCapture();
	body->setBg(0xFFFFFFFF);
	body->setBorder(HuhuCapture::UI::toolbarBorder, 0xA8A8A8ff);
	HuhuCapture::UI::configureToolbarRow(body.get());
	showSetting();
	show();
}

void ToolVideo::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

Ling::Node* ToolVideo::makeSpliter()
{
	return HuhuCapture::UI::makeToolbarDivider(body.get());
}

Ling::Button* ToolVideo::makeIconBtn(const std::wstring& code)
{
	auto btn = body->makeChild<Ling::Button>();
	btn->setText(code);
	HuhuCapture::UI::sizeToolbarControl(btn);
	btn->setHoverBg(0xF2F2F2ff);
	btn->setFontFamily(L"icon");
	btn->setFontSize(HuhuCapture::UI::toolbarIconSize);
	return btn;
}

void ToolVideo::applyToggleStyle(Ling::Button* btn, bool selected)
{
	if (selected) {
		btn->setBg(0xe6f4ffff);
		btn->setHoverBg(0xe6f4ffff);
		btn->setColor(0x1677ffff);
		btn->setHoverColor(0x1677ffff);
	}
	else {
		btn->setBg(0);
		btn->setHoverBg(0xF2F2F2ff);
		btn->setColor(0x333333ff);
		btn->setHoverColor(0x333333ff);
	}
}

void ToolVideo::showSetting()
{
	// 重建前先把旧指针作废：removeAllChildren 会连带销毁所有子节点
	btnMp4 = nullptr;
	btnGif = nullptr;
	btnSpeaker = nullptr;
	btnMic = nullptr;
	timerLabel = nullptr;
	// 按钮被销毁时 onLeave 不会触发，提示得手动收掉，否则它会一直挂在屏幕上
	tip->hide();
	body->removeAllChildren();
	refreshSize();

	// MP4 / GIF 二选一。与图标按钮使用同样高度，文字保留充足空间。
	btnMp4 = body->makeChild<Ling::Button>();
	btnMp4->setText(L"MP4");
	HuhuCapture::UI::sizeToolbarControl(btnMp4, formatW);
	btnMp4->setFontSize(16.f);
	btnMp4->onClick.add([this](Ling::Button*) { onFormatClick(0); });
	tip->bind(btnMp4, Lang::get(L"video.outputMp4"));

	btnGif = body->makeChild<Ling::Button>();
	btnGif->setText(L"GIF");
	HuhuCapture::UI::sizeToolbarControl(btnGif, formatW);
	btnGif->setFontSize(16.f);
	btnGif->onClick.add([this](Ling::Button*) { onFormatClick(1); });
	tip->bind(btnGif, Lang::get(L"video.outputGif"));

	makeSpliter();

	btnSpeaker = makeIconBtn(L"\ue654");
	btnSpeaker->onClick.add([this](Ling::Button* btn) {
		// GIF 不带声音，此时两个音源按钮不可切换
		if (selectIndex != 0) return;
		selectSpeaker = !selectSpeaker;
		applyToggleStyle(btn, selectSpeaker);
	});
	tip->bind(btnSpeaker, Lang::get(L"video.recordSystem"));
	btnMic = makeIconBtn(L"\ue73b");
	btnMic->onClick.add([this](Ling::Button* btn) {
		if (selectIndex != 0) return;
		selectMic = !selectMic;
		applyToggleStyle(btn, selectMic);
	});
	tip->bind(btnMic, Lang::get(L"video.recordMic"));

	makeSpliter();

	auto btnStart = makeIconBtn(L"\ue660");
	btnStart->onClick.add([this](Ling::Button*) { startRecord(); });
	tip->bind(btnStart, Lang::get(L"video.startRecord"));
	auto btnClose = makeIconBtn(L"\ue62d");
	btnClose->onClick.add([this](Ling::Button*) { this->win->close(); });
	tip->bind(btnClose, Lang::get(L"video.exit"));

	applyFormatStyle();
}

void ToolVideo::showRecording()
{
	btnMp4 = nullptr;
	btnGif = nullptr;
	btnSpeaker = nullptr;
	btnMic = nullptr;
	timerLabel = nullptr;
	tip->hide();
	body->removeAllChildren();
	refreshSize();

	timerLabel = body->makeChild<Ling::Label>();
	HuhuCapture::UI::sizeToolbarControl(timerLabel, timerW);
	timerLabel->setFontSize(14.f);
	timerLabel->setAlignItems(Ling::Align::Center);
	timerLabel->setJustifyContent(Ling::Justify::Center);
	updateTimerText();

	makeSpliter();

	// 丢弃 / 存文件 / 存剪切板，三条路都会停掉录制并结束整个流程
	auto btnDiscard = makeIconBtn(L"\ue62d");
	btnDiscard->onClick.add([this](Ling::Button*) { finishRecord(false); });
	tip->bind(btnDiscard, Lang::get(L"video.stopExit"));
	auto btnSave = makeIconBtn(L"\ue608");
	btnSave->onClick.add([this](Ling::Button*) { saveFile(); });
	tip->bind(btnSave, Lang::get(L"video.stopFile"));
	auto btnClipboard = makeIconBtn(L"\ue6ad");
	btnClipboard->onClick.add([this](Ling::Button*) { finishRecord(true); });
	tip->bind(btnClipboard, Lang::get(L"video.stopClipboard"));
}

void ToolVideo::onFormatClick(int index)
{
	if (selectIndex == index) return;
	selectIndex = index;
	// GIF 不录声音，切过去时把两个音源都关掉
	if (selectIndex == 1) {
		selectSpeaker = false;
		selectMic = false;
	}
	applyFormatStyle();
}

void ToolVideo::applyFormatStyle()
{
	if (!btnMp4 || !btnGif) return;
	applyToggleStyle(btnMp4, selectIndex == 0);
	applyToggleStyle(btnGif, selectIndex == 1);
	btnMp4->setColor(selectIndex == 0 ? 0x1677ffff : 0x333333ff);
	btnMp4->setHoverColor(0x1677ffff);
	btnGif->setColor(selectIndex == 1 ? 0x1677ffff : 0x333333ff);
	btnGif->setHoverColor(0x1677ffff);
	applyToggleStyle(btnSpeaker, selectSpeaker);
	applyToggleStyle(btnMic, selectMic);
}

void ToolVideo::startRecord()
{
	isRecording = true;
	totalSeconds = 0;
	showRecording();
	win->layoutTool(this);
	setTimer(1000, tickTimerId);
	if (selectIndex == 0) {
		win->startMp4(selectSpeaker, selectMic);
	}
	else {
		win->startGif();
	}
}

void ToolVideo::updateTimerText()
{
	if (!timerLabel) return;
	// GIF 上限 6 分钟，MP4 上限 120 分钟
	const int maxMinutes = (selectIndex == 1) ? 6 : 120;
	timerLabel->setText(std::format(L"{:02d}:{:02d} / {:02d}:00", totalSeconds / 60, totalSeconds % 60, maxMinutes));
}

void ToolVideo::onTimerCB(UINT id)
{
	if (id != tickTimerId) return;
	totalSeconds += 1;
	updateTimerText();
	const int maxSeconds = ((selectIndex == 1) ? 6 : 120) * 60;
	if (totalSeconds >= maxSeconds) {
		// 到上限就自动存盘收工
		saveFile();
	}
}

void ToolVideo::saveFile()
{
	hide();
	killTimer(tickTimerId);
	auto srcPath = win->stopRecord();
	// 空路径 = 一帧都没录到（刚开录就停了），没什么可存的，别弹保存框去打扰用户
	if (srcPath.empty()) {
		win->close();
		return;
	}
	auto tarPath = ImageExport::getSaveFilePath(nullptr, selectIndex == 1 ? L"gif" : L"mp4");
	if (!tarPath.empty()) {
		CopyFile(srcPath.data(), tarPath.data(), false);
	}
	DeleteFile(srcPath.data());
	win->close();
}

bool ToolVideo::onSaveKey(bool toClipboard)
{
	if (!isRecording) return false;
	// 停录、存盘、关掉整个流程，这两条都在下面两个函数里一条龙做完
	if (toClipboard) finishRecord(true);
	else saveFile();
	return true;
}

void ToolVideo::finishRecord(bool toClipboard)
{
	hide();
	killTimer(tickTimerId);
	auto srcPath = win->stopRecord();
	if (toClipboard) {
		// 文件留在临时目录里，剪切板持有的是它的路径，不能删。
		// 空路径 = 一帧都没录到，没东西可放进剪切板
		if (!srcPath.empty())
			ImageExport::addFileToClipboard(srcPath);
	}
	else {
		DeleteFile(srcPath.data());
	}
	win->close();
}
