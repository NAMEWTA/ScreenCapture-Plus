#pragma once
#include <include/Ling.h>
#include "ui/toolbars/ToolbarMetrics.h"

class WinCap;
class Tip;
// 框选工具条：图像标记 / 截长图 / 录屏 / 二维码，之后是关闭 / 保存 / 剪切板。
class ToolCap : public Ling::WinBase
{
public:
	ToolCap(WinCap* win);
	~ToolCap();
	// Reflow fixed-size controls within the current monitor work area.
	void fitToWorkArea(const RECT& workArea);
private:
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onClick(Ling::Button* btn);
	// 按当前 dpi 把窗口尺寸算出来并应用。构造时算一次，DPI 变了再算一次
	void refreshSize();
private:
	WinCap* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	float maximumWidth{ HuhuCapture::UI::toolbarUnlimitedWidth };

	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	// 三张表一一对应。spliter 是竖线分隔符，不是按钮
	std::vector<std::wstring> btnIds = { L"mark",L"long",L"video",L"qrcode",L"history",L"spliter",L"close",L"save",L"clipboard" };
	std::vector<std::wstring> btnCodes = { L"\ue97f",L"\ue73e",L"\ue660",L"\ue71e",L"\ue77f",L"",L"\ue62d",L"\ue608",L"\ue6ad" };
	// 翻译 key。空串是分隔符，没提示可挂
	std::vector<std::wstring> btnTips = { L"cap.mark",L"cap.long",L"cap.video",L"cap.qrcode",L"clipboard.title",L"",L"tool.close",L"tool.save",L"tool.clipboard" };
	// 以下都是逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi
	static constexpr float btnSize{ HuhuCapture::UI::toolbarButtonSize };
	static constexpr float spliterW{ HuhuCapture::UI::toolbarDivider };
};
