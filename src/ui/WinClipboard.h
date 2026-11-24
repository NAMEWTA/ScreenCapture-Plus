#pragma once

#include <include/Ling.h>
#include <cstddef>
#include <filesystem>
#include <memory>

// A bounded, keyboard-operable view over the files owned by MediaHistory.
// The window owns only presentation state; file operations stay in the service.
class WinClipboard final : public Ling::WinBase {
public:
    ~WinClipboard();
    static void init();
    static void dispose();
    static WinClipboard* get();

    // UI-thread snapshots for diagnostics and asynchronous GUI verification.
    bool isLoading() const;
    std::size_t entryCount() const;
    std::size_t thumbnailCount() const;
    bool hasThumbnail(const std::filesystem::path& path) const;

    void layout() override;

private:
    WinClipboard();
    void onCreated() override;
    LRESULT onHitTest(POINT position) override;
    BOOL setCursor() override;
    void reload();
    void showPage(std::size_t page);
    void pollWorker();
    void stopWorker();
    void paint();
    void keyDown(UINT key);
    void pointerMove(POINT position);
    void pointerDown(POINT position, bool right);
    void pointerUp(POINT position, bool right);
    void scroll(float amount);
    void selectEntry(std::size_t index);
    void activate(int action, std::size_t index);

    struct State;
    std::unique_ptr<State> state;
};
