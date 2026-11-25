#include "pch.h"
#include "core/ShortcutKeys.h"
#include "services/Lang.h"
#include "services/Setting.h"
#include "ui/settings/SettingsTheme.h"
#include "ui/settings/WinSetting.h"
#include "ui/settings/WinSettingShortcut.h"

namespace Theme = HuhuCapture::UI::SettingsTheme;

namespace {
    using HuhuCapture::Core::normalizeShortcutKeys;
    using HuhuCapture::Core::joinShortcutKeys;

    bool isShortcutModifierKey(const std::wstring& key)
    {
        return HuhuCapture::Core::isShortcutModifier(key);
    }

    bool isValidShortcutKeys(const std::vector<std::wstring>& keys)
    {
        bool supported = false;
        for (const auto& key : keys) {
            if (HuhuCapture::Core::isShortcutModifier(key)) continue;
            auto lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            supported = Ling::Util::strToKey(lower) != 0;
        }
        return HuhuCapture::Core::validateShortcutKeys(keys, supported) ==
            HuhuCapture::Core::ShortcutValidity::Valid;
    }
}



WinSettingShortcut::WinSettingShortcut(Ling::WinBase* parent) : Ling::Node(parent)
{
    Theme::pageHeading(this, Lang::get(L"setting.shortcut"), Lang::get(L"shortcut.description"));
    auto* shortcuts = Theme::card(this);
    auto* row = Theme::row(shortcuts);
    Theme::label(row, Lang::get(L"shortcut.cap"))->setFlexGrow(1.f);
    auto* capture = row->makeChild<Ling::Button>();
    capture->setId(L"cap");
    Theme::styleButton(capture, 180.f);
    capture->setFontFamily(Theme::semiboldFamily);
    capture->setText(Setting::get()->getShortcutKey(L"cap"));
    capture->onClick.add([this](Ling::Button* button) { onBtnClick(button); });
    btns.push_back(capture);

    auto* actions = Theme::row(this, Theme::controlHeight);
    actions->setMarginTop(Theme::groupGap);
    status = Theme::label(actions, L"", Theme::captionSize, Theme::controlHeight, Theme::secondary);
    status->setId(L"settings-shortcut-status");
    status->setFlexGrow(1.f);
    status->setFlexShrink(1.f);
    status->setMarginRight(12.f);
    auto* reset = actions->makeChild<Ling::Button>();
    reset->setId(L"settings-reset-shortcut");
    Theme::styleButton(reset, 120.f);
    reset->setText(Lang::get(L"shortcut.reset"));
    reset->onClick.add([this](Ling::Button*) {
        endCapture();
        const bool saved = Setting::get()->setShortcutKey(L"cap", { L"F1" });
        btns.front()->setText(Setting::get()->getShortcutKey(L"cap"));
        status->setText(Lang::get(saved ? L"shortcut.saved" : L"shortcut.conflict"));
        status->setColor(saved ? Theme::secondary : Theme::warning);
    });

    const auto weakThis = getWeakThis();
    onKeyDownToken = win->onKeyDown.add([this, weakThis](UINT key) {
        if (weakThis.lock() && !curKey.empty()) onKeyDown(key);
    });
    onKeyUpToken = win->onKeyUp.add([this, weakThis](UINT key) {
        if (weakThis.lock() && !curKey.empty()) onKeyUp(key);
    });
    onMouseDownToken = win->onMouseDown.add([this, weakThis](POINT position, bool) {
        if (!weakThis.lock() || curKey.empty()) return;
        for (auto* button : btns)
            if (button->isPosIn(position)) return;
        endCapture();
    });
    onBlurToken = win->onBlur.add([this, weakThis]() {
        if (weakThis.lock()) endCapture();
    });
}

WinSettingShortcut::~WinSettingShortcut()
{
    endCapture();
    win->onBlur.remove(onBlurToken);
    win->onMouseDown.remove(onMouseDownToken);
    win->onKeyDown.remove(onKeyDownToken);
    win->onKeyUp.remove(onKeyUpToken);
}

void WinSettingShortcut::onBtnClick(Ling::Button* btn)
{
    // 再次点击当前正在捕获的按钮 → 取消
    if (curKey == btn->id) {
        endCapture();
        return;
    }
    // 已经在捕获别的按钮 → 先复位旧的
    if (!curKey.empty()) {
        endCapture();
    }
    beginCapture(btn);
}

void WinSettingShortcut::beginCapture(Ling::Button* btn)
{
    curKey = btn->id;
    tempKeys.clear();
    Setting::get()->setShortcutCaptureActive(true);
    status->setText(L"");
    btn->setBg(Theme::blueTint);
    btn->setHoverBg(Theme::blueTint);
    btn->setColor(Theme::blue);
    btn->setBorder(2.f, Theme::focusBlue);
    btn->setText(Lang::get(L"shortcut.pressKey"));
}

void WinSettingShortcut::endCapture()
{
    if (curKey.empty()) return;
    for (auto& btn : btns)
    {
        if (btn->id == curKey) {
            Theme::styleButton(btn, 180.f);
            btn->setFontFamily(Theme::semiboldFamily);
            btn->setText(Setting::get()->getShortcutKey(curKey));
            break;
        }
    }
    curKey.clear();
    tempKeys.clear();
    Setting::get()->setShortcutCaptureActive(false);
}

void WinSettingShortcut::onKeyDown(UINT key)
{
    if (key == VK_ESCAPE) {
        endCapture();
        return;
    }
    auto keyStr = keyToStr(key);
    if (keyStr.empty()) return;

    // 当按下的是普通键（非修饰键）时，主动补齐当前按住的修饰键
    // 因为 Alt 等键走 WM_SYSKEYDOWN，Ling 框架可能不会转发到 onKeyDown
    if (!isShortcutModifierKey(keyStr)) {
        auto ensure = [&](int vk, const std::wstring& name) {
            if ((GetAsyncKeyState(vk) & 0x8000) &&
                std::find(tempKeys.begin(), tempKeys.end(), name) == tempKeys.end()) {
                tempKeys.push_back(name);
            }
        };
        ensure(VK_CONTROL, L"Ctrl");
        ensure(VK_MENU,    L"Alt");
        ensure(VK_SHIFT,   L"Shift");
        ensure(VK_LWIN,    L"LWin");
        ensure(VK_RWIN,    L"RWin");
    }

    bool isContains = std::find(tempKeys.begin(), tempKeys.end(), keyStr) != tempKeys.end();
    if (isContains) return;
    tempKeys.push_back(keyStr);
    auto ordered = tempKeys;
    normalizeShortcutKeys(ordered);
    for (auto* button : btns)
        if (button->id == curKey) button->setText(joinShortcutKeys(ordered));
}

void WinSettingShortcut::onKeyUp(UINT key)
{
    if (curKey.empty()) return;
    normalizeShortcutKeys(tempKeys);
    const bool valid = isValidShortcutKeys(tempKeys);
    const bool saved = valid && Setting::get()->setShortcutKey(curKey, tempKeys);
    endCapture();
    status->setText(Lang::get(saved ? L"shortcut.saved" :
        (valid ? L"shortcut.conflict" : L"shortcut.invalid")));
    status->setColor(saved ? Theme::secondary : Theme::warning);
}

std::wstring WinSettingShortcut::keyToStr(UINT vkCode)
{
    switch (vkCode) {
        // --- 修饰键 ---
    case VK_CONTROL: return L"Ctrl";
    case VK_MENU:    return L"Alt";
    case VK_SHIFT:   return L"Shift";
    case VK_LWIN:    return L"LWin";
    case VK_RWIN:    return L"RWin";
    case VK_CAPITAL: return L"CapsLock";
        // --- 功能键 (F1 - F12) ---
    case VK_F1: return L"F1";
    case VK_F2: return L"F2";
    case VK_F3: return L"F3";
    case VK_F4: return L"F4";
    case VK_F5: return L"F5";
    case VK_F6: return L"F6";
    case VK_F7: return L"F7";
    case VK_F8: return L"F8";
    case VK_F9: return L"F9";
    case VK_F10: return L"F10";
    case VK_F11: return L"F11";
    case VK_F12: return L"F12";
        // --- 方向键 ---
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
        // --- 控制与编辑键 ---
    case VK_RETURN:  return L"Enter";
    case VK_ESCAPE:  return L"Esc";
    case VK_TAB:     return L"Tab";
    case VK_SPACE:   return L"Space";
    case VK_BACK:    return L"Backspace";
    case VK_DELETE:  return L"Delete";
    case VK_INSERT:  return L"Insert";
    case VK_HOME:    return L"Home";
    case VK_END:     return L"End";
    case VK_PRIOR:   return L"PageUp";
    case VK_NEXT:    return L"PageDown";
    case VK_SNAPSHOT:return L"PrintScreen";
    case VK_SCROLL:  return L"ScrollLock";
    case VK_PAUSE:   return L"Pause";
        // --- 小键盘 ---
    case VK_NUMLOCK: return L"NumLock";
    case VK_MULTIPLY: return L"*";
    case VK_ADD:      return L"+";
    case VK_SUBTRACT: return L"-";
    case VK_DIVIDE:   return L"/";
    case VK_DECIMAL:  return L".";
    default:
        // --- 字母键 (A-Z 对应 ASCII 码 65-90) ---
        if (vkCode >= 'A' && vkCode <= 'Z') {
            return std::wstring(1, static_cast<wchar_t>(vkCode));
        }
        // --- 主键盘数字键 (0-9 对应 ASCII 码 48-57) ---
        if (vkCode >= '0' && vkCode <= '9') {
            return std::wstring(1, static_cast<wchar_t>(vkCode));
        }
        // --- 小键盘数字键 (0-9) ---
        if (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9) {
            return L"Num" + std::to_wstring(vkCode - VK_NUMPAD0);
        }
        if (vkCode == VK_OEM_3 || vkCode == VK_OEM_1 || vkCode == VK_OEM_4 ||
            vkCode == VK_OEM_6 || vkCode == VK_OEM_7 || vkCode == VK_OEM_5 ||
            vkCode == VK_OEM_2 || vkCode == VK_OEM_COMMA || vkCode == VK_OEM_PERIOD ||
            vkCode == VK_OEM_MINUS || vkCode == VK_OEM_PLUS)
        {
            wchar_t ch = 0;
            // MAPVK_VK_TO_CHAR (2) 会将虚拟键码转换为不带 Shift 状态的基础字符
            UINT scanCode = MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
            int result = MapVirtualKeyW(vkCode, MAPVK_VK_TO_CHAR);

            // 结果的低 16 位是字符，如果最高位(0x80000000)被置位，说明是死键(Dead Key)
            if (result != 0 && !(result & 0x80000000)) {
                ch = static_cast<wchar_t>(result & 0xFFFF);
                return std::wstring(1, ch);
            }
        }
        return L"";
    }
}
