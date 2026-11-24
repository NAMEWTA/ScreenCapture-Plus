#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "ui/settings/SettingsTheme.h"
#include "ui/settings/WinSettingAbout.h"

namespace Theme = HuhuCapture::UI::SettingsTheme;

WinSettingAbout::WinSettingAbout(Ling::WinBase* parent) : Ling::Node(parent)
{
    Theme::pageHeading(this, Lang::get(L"setting.about"), Lang::get(L"about.description"));
    auto* information = Theme::card(this);
    information->setPadding(Theme::cardInset, 12.f, Theme::cardInset, 4.f);
    auto* name = Theme::label(information, std::wstring{ HuhuCapture::Brand::name }, 20.f, 32.f);
    name->setFontFamily(Theme::semiboldFamily);
    name->setMarginBottom(12.f);

    const std::wstring keys[]{ L"version", L"project", L"author" };
    for (const auto& key : keys) {
        Theme::divider(information);
        auto* row = Theme::row(information, Theme::singleRowHeight);
        Theme::label(row, Lang::get(L"about." + key))->setFlexGrow(1.f);
        if (key == L"version") {
            const auto version = Ling::Util::getVerNum();
            auto* value = Theme::label(row, std::format(L"{}.{}.{}", version[0], version[1], version[2]));
            value->setId(L"settings-version");
            value->setWidth(220.f);
            value->setAlignItems(Ling::Align::FlexEnd);
            continue;
        }
        auto* link = row->makeChild<Ling::Button>();
        link->setId(L"settings-" + key);
        Theme::styleButton(link, 220.f);
        link->setBorderWidth(0.f);
        link->setColor(Theme::blue);
        link->setHoverColor(Theme::focusBlue);
        link->setAlignItems(Ling::Align::FlexEnd);
        link->setPaddingRight(8.f);
        link->setText(std::wstring{ key == L"project" ? HuhuCapture::Brand::repositoryLabel : HuhuCapture::Brand::author });
        link->onClick.add([this, key](Ling::Button*) {
            const auto url = key == L"project" ? HuhuCapture::Brand::repositoryUrl : HuhuCapture::Brand::authorUrl;
            ShellExecuteW(win->hwnd, L"open", url.data(), nullptr, nullptr, SW_SHOWNORMAL);
        });
    }

    auto* actions = Theme::row(this, Theme::controlHeight);
    actions->setMarginTop(Theme::groupGap);
    auto* releases = actions->makeChild<Ling::Button>();
    releases->setId(L"settings-releases");
    Theme::styleButton(releases, 156.f, true);
    releases->setText(Lang::get(L"about.releases"));
    releases->onClick.add([this](Ling::Button*) {
        ShellExecuteW(win->hwnd, L"open", HuhuCapture::Brand::releasesUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
    });
}

WinSettingAbout::~WinSettingAbout() = default;
