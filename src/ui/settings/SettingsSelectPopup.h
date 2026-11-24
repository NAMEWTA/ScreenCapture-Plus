#pragma once

#include <include/Ling.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The caller owns this transient window. Completion is queued after it hides,
// and is cancelled if the popup is destroyed before the queued call runs.
class SettingsSelectPopup final : public Ling::WinBase {
public:
    struct Item {
        std::wstring label;
        std::wstring value;
        bool separatorBefore{};
    };
    using Completion = std::function<void(std::optional<std::wstring>)>;

    SettingsSelectPopup(HWND owner, float scale, RECT screenAnchor,
        std::vector<Item> items, const std::wstring& selectedValue,
        Completion completion);
    ~SettingsSelectPopup();

    void dismiss();
    bool isOpen() const;
    std::wstring highlightedValue() const;
    RECT itemScreenRect(const std::wstring& value) const;
    void layout() override;

private:
    void onCreated() override;
    LRESULT onHitTest(POINT) override { return HTCLIENT; }
    void onMinMaxInfo(MINMAXINFO* info) override;
    BOOL setCursor() override;
    void paint();
    void keyDown(UINT key);
    void pointerMove(POINT point);
    void pointerDown(POINT point, bool right);
    void pointerUp(POINT point, bool right);
    void scroll(float distance);
    void ensureVisible();
    void finish(std::optional<std::wstring> value, bool restoreOwnerFocus);

    struct State;
    std::unique_ptr<State> state;
};
