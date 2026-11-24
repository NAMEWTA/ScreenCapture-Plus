#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "ui/settings/SettingsTheme.h"
#include "ui/settings/WinSetting.h"
#include "ui/settings/WinSettingCommon.h"
#include "ui/settings/WinSettingShortcut.h"
#include "ui/settings/WinSettingAbout.h"

#include <iterator>
#include <algorithm>

namespace {
std::unique_ptr<WinSetting> winSetting;
namespace Theme = HuhuCapture::UI::SettingsTheme;
}

WinSetting::WinSetting() : Ling::WinBase()
{
    // A close button is owned by this window: defer destruction until its event
    // callback has returned. Language changes enqueue reopening after this reset.
    onDestroy.add([]() {
        Ling::App::get()->dq.TryEnqueue([]() { winSetting.reset(); });
    });
    setTitle(Lang::get(L"setting.title"));
    RECT workArea{};
    float availableWidth = 880.f, availableHeight = 640.f;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        availableWidth = (std::max)(320.f, (workArea.right - workArea.left - 32.f) / dpi);
        availableHeight = (std::max)(320.f, (workArea.bottom - workArea.top - 32.f) / dpi);
    }
    setSize((std::min)(880.f, availableWidth), (std::min)(640.f, availableHeight));
    setMinSize((std::min)(800.f, availableWidth), (std::min)(520.f, availableHeight));
    setCenter();
    createNativeWindow();
}

WinSetting::~WinSetting() = default;

void WinSetting::init()
{
    if (winSetting) {
        if (IsIconic(winSetting->hwnd)) ShowWindow(winSetting->hwnd, SW_RESTORE);
        SetForegroundWindow(winSetting->hwnd);
        return;
    }
    winSetting.reset(new WinSetting());
}

WinSetting* WinSetting::get()
{
    return winSetting.get();
}

void WinSetting::dispose()
{
    winSetting.reset();
}

void WinSetting::layout()
{
    const bool compact = h / dpi < 560.f;
    if (content && (styledContent != content || compactLayout != compact)) {
        styledContent = content;
        compactLayout = compact;
        if (compact) content->setPadding(24.f, 48.f, 24.f, 16.f);
        else content->setPadding(32.f, 56.f, 32.f, 24.f);
        if (menuIndex == 0) static_cast<WinSettingCommon*>(content)->setCompact(compact);
    }
    Ling::WinBase::layout();
}

void WinSetting::onCreated()
{
    enableShadow();
    enableBorderResize();
    body->setBg(Theme::canvas);
    body->setFlexDirection(Ling::FlexDirection::Row);

    auto* menuBox = body->makeChild<Ling::Node>();
    menuBox->setBg(Theme::surface);
    menuBox->setWidth(184.f);
    menuBox->setFlexShrink(0.f);
    menuBox->setHeightPercent(100.f);
    menuBox->setPadding(16.f, 56.f, 16.f, 24.f);
    auto* brand = Theme::label(menuBox, std::wstring{ HuhuCapture::Brand::name }, 18.f, 28.f);
    brand->setFontFamily(L"Segoe UI Semibold");
    brand->setMarginLeft(12.f);
    auto* title = Theme::label(menuBox, Lang::get(L"setting.title"), 14.f, 24.f, Theme::secondary);
    title->setMargin(12.f, 0.f, 0.f, 24.f);
    initMenuItems(menuBox);

    content = body->makeChild<WinSettingCommon>();
    Theme::stylePage(content);

    auto* closeButton = body->makeChild<Ling::Button>();
    closeButton->setId(L"settings-close");
    closeButton->setSize(48.f, Theme::controlHeight);
    closeButton->setPositionType(Ling::Position::Absolute);
    closeButton->setPosition(Ling::Edge::Right, 8.f);
    closeButton->setPosition(Ling::Edge::Top, 6.f);
    closeButton->setBorderRadius(11.f);
    closeButton->setColor(Theme::secondary);
    closeButton->setHoverColor(Theme::ink);
    closeButton->setHoverBg(Theme::hairline);
    closeButton->setText(L"\ue62d");
    closeButton->setFontFamily(L"icon");
    closeButton->setFontSize(20.f);
    closeButton->onClick.add([](Ling::Button* button) { button->win->close(); });
    show();
}

void WinSetting::styleMenuItem(Ling::Button* menu, bool selected)
{
    menu->setColor(selected ? Theme::blue : Theme::ink);
    menu->setBg(selected ? Theme::blueTint : Theme::surface);
    menu->setHoverColor(Theme::blue);
    menu->setHoverBg(selected ? Theme::blueTint : Theme::canvas);
}

void WinSetting::initMenuItems(Ling::Node* menuBox)
{
    const std::wstring keys[]{ L"common", L"shortcut", L"about" };
    for (size_t index = 0; index < std::size(keys); ++index) {
        auto* menu = menuBox->makeChild<Ling::Button>();
        menu->setId(L"settings-menu-" + keys[index]);
        menu->setFontFamily(L"Segoe UI");
        menu->setFontSize(17.f);
        menu->setHeight(Theme::controlHeight);
        menu->setFlexShrink(0.f);
        menu->setMarginBottom(8.f);
        menu->setPaddingLeft(14.f);
        menu->setAlignItems(Ling::Align::FlexStart);
        menu->setBorderRadius(11.f);
        menu->setText(Lang::get(L"setting." + keys[index]));
        styleMenuItem(menu, index == 0);
        menu->onClick.add([this](Ling::Button* selected) { onMenuItemClick(selected); });
        menus.push_back(menu);
    }
}

void WinSetting::onMenuItemClick(Ling::Button* menu)
{
    const auto index = Ling::Util::getIndex(menus, menu);
    if (index < 0 || index == menuIndex) return;
    if (menuIndex == 0) static_cast<WinSettingCommon*>(content)->hideSelectBox();
    styleMenuItem(menus[menuIndex], false);
    menuIndex = index;
    styleMenuItem(menu, true);

    body->removeChild(content);
    styledContent = nullptr;
    if (menuIndex == 0) content = body->makeChild<WinSettingCommon>();
    else if (menuIndex == 1) content = body->makeChild<WinSettingShortcut>();
    else content = body->makeChild<WinSettingAbout>();
    Theme::stylePage(content);
}

LRESULT WinSetting::onHitTest(const POINT position)
{
    POINT point = position;
    ScreenToClient(hwnd, &point);
    if (!isMaximized) {
        const auto result = borderHitTest(point);
        if (result != HTCLIENT) return result;
    }
    // Restrict dragging to the title strip; the sidebar's lower area contains
    // real controls and must not unexpectedly turn clicks into window movement.
    if (point.x > 0 && point.y > 0 && point.x < w - 64.f * dpi && point.y < 50.f * dpi)
        return HTCAPTION;
    return HTCLIENT;
}
