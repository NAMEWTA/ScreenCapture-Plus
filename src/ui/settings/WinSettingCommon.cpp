#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "services/MediaHistory.h"
#include "services/Setting.h"
#include "ui/Tip.h"
#include "ui/settings/SettingsTheme.h"
#include "ui/settings/WinSetting.h"
#include "ui/settings/WinSettingCommon.h"

#include <algorithm>
#include <iterator>

namespace Theme = HuhuCapture::UI::SettingsTheme;

WinSettingCommon::WinSettingCommon(Ling::WinBase* parent) : Ling::Node(parent)
{
    tip = std::make_unique<Tip>(win);
    Theme::pageHeading(this, Lang::get(L"setting.common"), Lang::get(L"setting.commonDescription"));
    auto* preferences = Theme::card(this);
    initAutoStartCtrls(preferences);
    Theme::divider(preferences);
    initLangCtrls(preferences);
    initArchiveCtrls();
    footer = Theme::label(this, Lang::get(L"setting.savedImmediately"), 14.f, 24.f, Theme::secondary);
    footer->setMarginTop(16.f);
}

WinSettingCommon::~WinSettingCommon()
{
    hideSelectBox();
}

void WinSettingCommon::initAutoStartCtrls(Ling::Node* card)
{
    auto* row = Theme::row(card);
    auto* copy = row->makeChild<Ling::Node>();
    copy->setFlexGrow(1.f);
    Theme::label(copy, Lang::get(L"setting.autoStart"));
    Theme::label(copy, Lang::get(L"setting.autoStartDescription"), 14.f, 22.f, Theme::secondary);

    auto* button = row->makeChild<Ling::Button>();
    button->setId(L"settings-autostart");
    button->setSize(64.f, Theme::controlHeight);
    button->setFlexShrink(0.f);
    button->setBorderRadius(11.f);
    button->setFontFamily(L"icon");
    button->setFontSize(32.f);
    button->setHoverBg(Theme::canvas);
    setAutoStartBtn(button);
    button->onClick.add([this](Ling::Button* clicked) {
        auto* settings = Setting::get();
        settings->setAutoStart(!settings->getAutoStart());
        setAutoStartBtn(clicked);
    });
}

void WinSettingCommon::initLangCtrls(Ling::Node* card)
{
    auto* row = Theme::row(card);
    Theme::label(row, Lang::get(L"setting.language"))->setFlexGrow(1.f);
    const auto current = Setting::get()->getLang();
    const auto languages = Lang::get()->getSupportedLang();
    std::wstring name = L"English";
    for (const auto& language : languages) {
        if (language.second == current) {
            name = language.first;
            break;
        }
    }
    selectBtn = row->makeChild<Ling::Button>();
    selectBtn->setId(L"settings-language");
    Theme::styleButton(selectBtn, 208.f);
    selectBtn->setText(name + L"  ▾");
    selectBtn->onClick.add([this](Ling::Button* button) { showSelectBox(button); });
}

void WinSettingCommon::initArchiveCtrls()
{
    auto* archive = Theme::card(this);
    archive->setMarginTop(20.f);
    archive->setPadding(24.f, 16.f, 24.f, 16.f);
    auto* row = Theme::row(archive, Theme::controlHeight);
    auto* heading = Theme::label(row, Lang::get(L"setting.archiveTitle"));
    heading->setFontFamily(L"Segoe UI Semibold");
    heading->setFlexGrow(1.f);
    auto* open = row->makeChild<Ling::Button>();
    open->setId(L"settings-archive-folder");
    Theme::styleButton(open, 144.f);
    open->setText(Lang::get(L"setting.openArchive"));

    archiveDescription = Theme::label(archive, Lang::get(L"setting.archiveDescription"), 14.f, 24.f, Theme::secondary);
    const auto directory = HuhuCapture::MediaHistory::directory();
    const auto fullPath = directory.wstring();
    // Ling's basic Label deliberately has no wrapping. Compact only this display
    // string, and retain the complete path in the folder button's tooltip/action.
    wchar_t compactPath[64]{};
    const bool compacted = !fullPath.empty() &&
        PathCompactPathExW(compactPath, fullPath.c_str(), 48, 0);
    Theme::label(archive, compacted ? std::wstring{ compactPath } : fullPath,
        14.f, 24.f, Theme::secondary);
    Theme::label(archive, Lang::get(L"setting.archiveNaming"), 14.f, 24.f, Theme::secondary);
    if (!fullPath.empty()) tip->bind(open, fullPath);
    open->onClick.add([this, directory](Ling::Button*) {
        tip->hide();
        if (directory.empty() || !HuhuCapture::MediaHistory::reveal(win->hwnd, directory)) {
            MessageBoxW(win->hwnd, Lang::get(L"setting.archiveUnavailable").c_str(),
                HuhuCapture::Brand::name.data(), MB_OK | MB_ICONWARNING);
        }
    });
}

void WinSettingCommon::setAutoStartBtn(Ling::Button* button)
{
    const bool enabled = Setting::get()->getAutoStart();
    button->setText(enabled ? L"\ue688" : L"\ue687");
    button->setColor(enabled ? Theme::blue : Theme::secondary);
    button->setHoverColor(enabled ? Theme::focusBlue : Theme::ink);
}

void WinSettingCommon::hideSelectBox()
{
    if (tip) tip->hide();
    if (menuOpen) EndMenu();
}

void WinSettingCommon::setCompact(bool compact)
{
    if (compact) {
        footer->hide();
        archiveDescription->hide();
    } else {
        footer->show();
        archiveDescription->show();
    }
}

void WinSettingCommon::showSelectBox(Ling::Button* button)
{
    if (menuOpen) return;
    tip->hide();
    std::vector<std::pair<std::wstring, std::wstring>> languages;
    for (const auto& language : Lang::get()->getSupportedLang()) {
        if (std::none_of(languages.begin(), languages.end(), [&language](const auto& existing) {
            return existing.second == language.second;
        })) languages.push_back(language);
    }
    const auto current = Setting::get()->getLang();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (size_t index = 0; index < languages.size(); ++index) {
        const auto flags = MF_STRING | (languages[index].second == current ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(index + 1), languages[index].first.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const UINT moreId = static_cast<UINT>(languages.size() + 1);
    AppendMenuW(menu, MF_STRING, moreId, Lang::get(L"setting.getMoreLang").c_str());
    POINT anchor{ static_cast<LONG>(button->x + button->w), static_cast<LONG>(button->y + button->h + 6.f * win->dpi) };
    ClientToScreen(win->hwnd, &anchor);
    const auto weakThis = getWeakThis();
    menuOpen = true;
    // The native menu supplies reliable DPI, scrolling, arrow keys and Esc. The
    // framework's basic Button does not compensate for a ScrollerBox offset.
    const UINT selection = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN,
        anchor.x, anchor.y, win->hwnd, nullptr);
    DestroyMenu(menu);
    if (!weakThis.lock()) return;
    menuOpen = false;
    if (selection == moreId) {
        ShellExecuteW(win->hwnd, L"open", HuhuCapture::Brand::languagesUrl.data(),
            nullptr, nullptr, SW_SHOWNORMAL);
    } else if (selection > 0 && selection <= languages.size()) {
        const auto& chosen = languages[selection - 1].second;
        if (chosen == current) return;
        Setting::get()->setLang(chosen);
        win->close();
        Ling::App::get()->dq.TryEnqueue([]() { WinSetting::init(); });
    }
}
