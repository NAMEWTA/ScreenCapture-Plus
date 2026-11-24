#pragma once
#include <include/Ling.h>

class WinSetting :public Ling::WinBase
{
public:
	~WinSetting();
	static void init();
	static WinSetting* get();
	void layout() override;
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
private:
	WinSetting();
	void initMenuItems(Ling::Node* menuBox);
	void onCreated() override;
	void onMenuItemClick(Ling::Button* menu);
	void styleMenuItem(Ling::Button* menu, bool selected);
	LRESULT onHitTest(const POINT pos) override;
private:
	std::vector<Ling::Button*> menus;
	int menuIndex{ 0 };
	Ling::Node* contentHost{nullptr};
	Ling::Node* content{nullptr};
	Ling::Node* styledContent{nullptr};
	float contentWidth{-1.f};
	bool compactLayout{};
};

