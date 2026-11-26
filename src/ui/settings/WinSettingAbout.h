#pragma once
#include <include/Ling.h>
#include <memory>
#include <string>
#include <vector>

class Tip;

class WinSettingAbout : public Ling::Node
{
public:
    explicit WinSettingAbout(Ling::WinBase* parent);
    ~WinSettingAbout();
    // Called by the existing settings host before its Yoga layout pass.
    void setContentMetrics(float width, bool compact);
private:
    void refreshUpdate();
    void setStatusText(std::wstring text);
    void wrapStatusText();

    Ling::Button* downloadButton{ nullptr };
    Ling::Button* folderButton{ nullptr };
    Ling::Label* updateStatus{ nullptr };
    Ling::Label* downloadPath{ nullptr };
    std::unique_ptr<Tip> tip;
    winrt::event_token updateToken{};
    std::vector<Ling::Node*> informationRows;
    std::wstring statusText;
    float statusWidth{-1.f}, statusDpi{-1.f};
    bool compactInformation{};
};

