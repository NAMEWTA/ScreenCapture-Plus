#pragma once
#include <include/Ling.h>
#include <memory>

class Tip;

class WinSettingCommon : public Ling::Node
{
public:
    explicit WinSettingCommon(Ling::WinBase* parent);
    ~WinSettingCommon();
    void hideSelectBox();
    void setCompact(bool compact);
private:
    void initAutoStartCtrls(Ling::Node* card);
    void initLangCtrls(Ling::Node* card);
    void initArchiveCtrls();
    void setAutoStartBtn(Ling::Button* button);
    void showSelectBox(Ling::Button* button);

    Ling::Button* selectBtn{ nullptr };
    Ling::Label* footer{ nullptr };
    Ling::Label* archiveDescription{ nullptr };
    std::unique_ptr<Tip> tip;
    bool menuOpen{};
};
