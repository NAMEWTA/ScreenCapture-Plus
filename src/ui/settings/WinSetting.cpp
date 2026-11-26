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
    float availableWidth = Theme::windowWidth, availableHeight = Theme::windowHeight;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        availableWidth = (std::max)(320.f, (workArea.right - workArea.left - 32.f) / dpi);
        availableHeight = (std::max)(320.f, (workArea.bottom - workArea.top - 32.f) / dpi);
    }
    setSize((std::min)(Theme::windowWidth, availableWidth), (std::min)(Theme::windowHeight, availableHeight));
    setMinSize((std::min)(640.f, availableWidth), (std::min)(400.f, availableHeight));
    onDpiChanged.add([this]() {
        MONITORINFO monitor{ sizeof(MONITORINFO) };
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& area = monitor.rcWork;
            setMinSize((std::min)(640.f, (area.right - area.left - 32.f) / dpi),
                (std::min)(400.f, (area.bottom - area.top - 32.f) / dpi));
        }
    });
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
    const bool compact = h / dpi < 460.f;
    const float width = (std::max)(0.f, (std::min)(Theme::pageMaximumWidth,
        w / dpi - Theme::sidebarWidth - Theme::pageInset * 2.f));
    if (content && (styledContent != content || compactLayout != compact)) {
        styledContent = content;
        compactLayout = compact;
        contentHost->setPadding(Theme::pageInset, compact ? 32.f : 40.f,
            Theme::pageInset, compact ? 16.f : 24.f);
        if (menuIndex == 0) static_cast<WinSettingCommon*>(content)->setCompact(compact);
    }
    if (content && contentWidth != width) {
        contentWidth = width;
        content->setWidth(width);
    }
    if (content && menuIndex == 2) static_cast<WinSettingAbout*>(content)->setContentMetrics(width, compact);
    Ling::WinBase::layout();
}

void WinSetting::onCreated()
{
    enableShadow();
    body->setBg(Theme::canvas);
    body->setFlexDirection(Ling::FlexDirection::Row);

    auto* menuBox = body->makeChild<Ling::Node>();
    menuBox->setBg(Theme::surface);
    menuBox->setWidth(Theme::sidebarWidth);
    menuBox->setFlexShrink(0.f);
    menuBox->setHeightPercent(100.f);
    menuBox->setPadding(12.f, 40.f, 12.f, 20.f);
    auto* brand = Theme::label(menuBox, std::wstring{ HuhuCapture::Brand::name }, 15.f, 24.f);
    brand->setFontFamily(Theme::semiboldFamily);
    brand->setMarginLeft(10.f);
    brand->setMarginBottom(20.f);
    initMenuItems(menuBox);

    contentHost = body->makeChild<Ling::Node>();
    contentHost->setId(L"settings-content-host");
    contentHost->setFlexGrow(1.f);
    contentHost->setFlexShrink(1.f);
    contentHost->setHeightPercent(100.f);
    contentHost->setFlexDirection(Ling::FlexDirection::Column);
    contentHost->setAlignItems(Ling::Align::Center);
    content = contentHost->makeChild<WinSettingCommon>();
    Theme::stylePage(content);

    auto* closeButton = body->makeChild<Ling::Button>();
    closeButton->setId(L"settings-close");
    closeButton->setSize(36.f, 32.f);
    closeButton->setPositionType(Ling::Position::Absolute);
    closeButton->setPosition(Ling::Edge::Right, 6.f);
    closeButton->setPosition(Ling::Edge::Top, 4.f);
    closeButton->setBorderRadius(Theme::controlRadius);
    closeButton->setColor(Theme::secondary);
    closeButton->setHoverColor(Theme::ink);
    closeButton->setHoverBg(Theme::hairline);
    closeButton->setText(L"\ue62d");
    closeButton->setFontFamily(L"icon");
    closeButton->setFontSize(16.f);
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
        menu->setFontFamily(Theme::fontFamily);
        menu->setFontSize(Theme::bodySize);
        menu->setHeight(36.f);
        menu->setFlexShrink(0.f);
        menu->setMarginBottom(4.f);
        menu->setPaddingLeft(10.f);
        menu->setAlignItems(Ling::Align::FlexStart);
        menu->setBorderRadius(Theme::controlRadius);
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

    contentHost->removeChild(content);
    styledContent = nullptr;
    contentWidth = -1.f;
    if (menuIndex == 0) content = contentHost->makeChild<WinSettingCommon>();
    else if (menuIndex == 1) content = contentHost->makeChild<WinSettingShortcut>();
    else content = contentHost->makeChild<WinSettingAbout>();
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
    if (point.x > 0 && point.y > 0 && point.x < w - 48.f * dpi && point.y < 32.f * dpi)
        return HTCAPTION;
    return HTCLIENT;
}
