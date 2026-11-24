#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "services/MediaHistory.h"
#include "services/Setting.h"
#include "ui/Tip.h"
#include "ui/settings/SettingsTheme.h"
#include "ui/settings/SettingsSelectPopup.h"
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
    footer = Theme::label(this, Lang::get(L"setting.savedImmediately"), Theme::captionSize,
        Theme::captionLineHeight, Theme::secondary);
    footer->setMarginTop(12.f);
    const auto weakThis = getWeakThis();
    const auto hidePopup = [this, weakThis]() { if (weakThis.lock()) hideSelectBox(); };
    movedToken = win->onMoved.add(hidePopup);
    sizeToken = win->onSizeChanged.add(hidePopup);
    dpiToken = win->onDpiChanged.add(hidePopup);
}

WinSettingCommon::~WinSettingCommon()
{
    win->onMoved.remove(movedToken);
    win->onSizeChanged.remove(sizeToken);
    win->onDpiChanged.remove(dpiToken);
    selectPopup.reset();
}

void WinSettingCommon::initAutoStartCtrls(Ling::Node* card)
{
    auto* row = Theme::row(card);
    auto* copy = row->makeChild<Ling::Node>();
    copy->setFlexGrow(1.f);
    copy->setFlexShrink(1.f);
    Theme::label(copy, Lang::get(L"setting.autoStart"));
    Theme::label(copy, Lang::get(L"setting.autoStartDescription"), Theme::captionSize,
        Theme::captionLineHeight, Theme::secondary);

    auto* button = row->makeChild<Ling::Button>();
    button->setId(L"settings-autostart");
    button->setSize(44.f, 36.f);
    button->setFlexShrink(0.f);
    button->setBorderRadius(Theme::controlRadius);
    button->setFontFamily(L"icon");
    button->setFontSize(28.f);
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
    auto* row = Theme::row(card, Theme::singleRowHeight);
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
    Theme::styleButton(selectBtn, 180.f);
    selectBtn->setPadding(12.f, 0.f, 32.f, 0.f);
    selectBtn->setAlignItems(Ling::Align::FlexStart);
    selectBtn->setText(name);
    selectArrow = Theme::label(selectBtn, L"▾", Theme::captionSize, Theme::controlHeight, Theme::secondary);
    selectArrow->setPositionType(Ling::Position::Absolute);
    selectArrow->setPosition(Ling::Edge::Right, 10.f);
    selectArrow->setPosition(Ling::Edge::Top, 0.f);
    selectArrow->setWidth(16.f);
    selectArrow->setAlignItems(Ling::Align::Center);
    selectBtn->onClick.add([this](Ling::Button* button) { showSelectBox(button); });
}

void WinSettingCommon::initArchiveCtrls()
{
    auto* archive = Theme::card(this);
    archive->setMarginTop(Theme::groupGap);
    archive->setPadding(Theme::cardInset, 14.f, Theme::cardInset, 14.f);
    auto* row = Theme::row(archive, Theme::controlHeight);
    auto* heading = Theme::label(row, Lang::get(L"setting.archiveTitle"));
    heading->setFontFamily(Theme::semiboldFamily);
    heading->setFlexGrow(1.f);
    auto* open = row->makeChild<Ling::Button>();
    open->setId(L"settings-archive-folder");
    Theme::styleButton(open, 120.f);
    open->setText(Lang::get(L"setting.openArchive"));

    archiveDescription = Theme::label(archive, Lang::get(L"setting.archiveDescription"),
        Theme::captionSize, Theme::captionLineHeight, Theme::secondary);
    archiveDescription->setMarginTop(6.f);
    const auto directory = HuhuCapture::MediaHistory::directory();
    const auto fullPath = directory.wstring();
    // Ling's basic Label deliberately has no wrapping. Compact only this display
    // string, and retain the complete path in the folder button's tooltip/action.
    wchar_t compactPath[64]{};
    const bool compacted = !fullPath.empty() &&
        PathCompactPathExW(compactPath, fullPath.c_str(), 40, 0);
    Theme::label(archive, compacted ? std::wstring{ compactPath } : fullPath,
        Theme::captionSize, Theme::captionLineHeight, Theme::secondary);
    Theme::label(archive, Lang::get(L"setting.archiveNaming"), Theme::captionSize,
        Theme::captionLineHeight, Theme::secondary);
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
    if (selectPopup) selectPopup->dismiss();
}

SettingsSelectPopup* WinSettingCommon::languagePopup() const
{
    return selectPopup.get();
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
    if (selectPopup) { selectPopup->dismiss(); return; }
    tip->hide();
    std::vector<SettingsSelectPopup::Item> languages;
    for (const auto& language : Lang::get()->getSupportedLang()) {
        if (std::none_of(languages.begin(), languages.end(), [&language](const auto& existing) {
            return existing.value == language.second;
        })) languages.push_back({ language.first, language.second });
    }
    const auto current = Setting::get()->getLang();
    languages.push_back({ Lang::get(L"setting.getMoreLang"), L"__more__", true });
    POINT origin{ static_cast<LONG>(button->x), static_cast<LONG>(button->y) };
    ClientToScreen(win->hwnd, &origin);
    const RECT anchor{ origin.x, origin.y, origin.x + static_cast<LONG>(button->w),
        origin.y + static_cast<LONG>(button->h) };
    const auto weakThis = getWeakThis();
    selectBtn->setBorderColor(Theme::focusBlue);
    selectArrow->setText(L"▴");
    selectPopup = std::make_unique<SettingsSelectPopup>(win->hwnd, win->dpi, anchor,
        std::move(languages), current, [this, weakThis, current](std::optional<std::wstring> chosen) {
            if (!weakThis.lock()) return;
            selectPopup.reset();
            selectBtn->setBorderColor(Theme::hairline);
            selectArrow->setText(L"▾");
            if (!chosen || *chosen == current) return;
            if (*chosen == L"__more__") {
                ShellExecuteW(win->hwnd, L"open", HuhuCapture::Brand::languagesUrl.data(),
                    nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            Setting::get()->setLang(*chosen);
            win->close();
            Ling::App::get()->dq.TryEnqueue([]() { WinSetting::init(); });
        });
}
