#pragma once
#include <include/Ling.h>
#include <memory>

class Tip;
class SettingsSelectPopup;

class WinSettingCommon : public Ling::Node
{
public:
    explicit WinSettingCommon(Ling::WinBase* parent);
    ~WinSettingCommon();
    void hideSelectBox();
    void setCompact(bool compact);
    SettingsSelectPopup* languagePopup() const;
private:
    void initAutoStartCtrls(Ling::Node* card);
    void initLangCtrls(Ling::Node* card);
    void initArchiveCtrls();
    void setAutoStartBtn(Ling::Button* button);
    void showSelectBox(Ling::Button* button);

    Ling::Button* selectBtn{ nullptr };
    Ling::Label* selectArrow{ nullptr };
    Ling::Label* footer{ nullptr };
    Ling::Label* archiveDescription{ nullptr };
    std::unique_ptr<Tip> tip;
    std::unique_ptr<SettingsSelectPopup> selectPopup;
    winrt::event_token movedToken{}, sizeToken{}, dpiToken{};
};
