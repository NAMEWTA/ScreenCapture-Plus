#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "ui/settings/WinSetting.h"
#include "ui/settings/WinSettingAbout.h"


WinSettingAbout::WinSettingAbout(Ling::WinBase* parent):Ling::Node(parent)
{
    auto title = makeChild<Ling::Label>();
    title->setText(std::wstring{ HuhuCapture::Brand::name });
    title->setFontSize(28.f);
    title->setColor(HuhuCapture::Brand::inkRgba);
    title->setHeight(56.f);

    std::vector<std::wstring> keys = { L"version",L"project",L"author" };
    for (auto& key : keys)
    {
        auto box = makeChild<Ling::Node>();
        box->setHeight(39.f);
        box->setFlexDirection(Ling::FlexDirection::Row);
        box->setAlignItems(Ling::Align::Center);

        auto label = box->makeChild<Ling::Label>();
        label->setText(Lang::get(L"about." + key));
        label->setHeightPercent(100.f);
        label->setJustifyContent(Ling::Justify::Center);
        label->setFlexGrow(1.f);

        auto btn = box->makeChild<Ling::Button>();
        btn->setId(key);
        if (key == L"version") {
            auto ver = Ling::Util::getVerNum();
            auto verStr = std::format(L"{}.{}.{}", ver[0], ver[1], ver[2]);
            btn->setText(verStr);
        }
        else if (key == L"project") {
            btn->setText(std::wstring{ HuhuCapture::Brand::repositoryLabel });
            btn->setColor(HuhuCapture::Brand::inkRgba);
            btn->setHoverColor(HuhuCapture::Brand::inkRgba);
            btn->onClick.add([this](Ling::Button* btn) {
                ShellExecute(win->hwnd, L"open", HuhuCapture::Brand::repositoryUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
                });
        }
        else {
            btn->setText(std::wstring{ HuhuCapture::Brand::author });
            btn->setColor(HuhuCapture::Brand::inkRgba);
            btn->setHoverColor(HuhuCapture::Brand::inkRgba);
            btn->onClick.add([this](Ling::Button* btn) {
                ShellExecute(win->hwnd, L"open", HuhuCapture::Brand::authorUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
                });
        }
        btn->setAlignItems(Ling::Align::FlexEnd);
        btn->setHeight(28.f);
        btn->setWidth(260.f);
        btn->setBg(0);
        btn->setHoverBg(0);
        btns.push_back(btn);

        auto border = makeChild<Ling::Node>();
        border->setHeight(1.f);
        border->setBg(0xE0E0E0FF);
    }
}

WinSettingAbout::~WinSettingAbout()
{

}
