#pragma once
#include <include/Ling.h>
#include <memory>

class Tip;

class WinSettingAbout : public Ling::Node
{
public:
    explicit WinSettingAbout(Ling::WinBase* parent);
    ~WinSettingAbout();
private:
    void refreshUpdate();

    Ling::Button* downloadButton{ nullptr };
    Ling::Button* folderButton{ nullptr };
    Ling::Label* updateStatus{ nullptr };
    Ling::Label* downloadPath{ nullptr };
    std::unique_ptr<Tip> tip;
    winrt::event_token updateToken{};
};

