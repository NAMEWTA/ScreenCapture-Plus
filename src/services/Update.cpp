#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "services/Setting.h"
#include "services/Update.h"

#include <atomic>
#include <chrono>
#include <format>
#include <memory>

namespace {
    using Stage = Update::Stage;
    using HuhuCapture::Updates::WindowsBridge::Download;
    using HuhuCapture::Updates::WindowsBridge::Options;

    struct UiLifetime { std::atomic<bool> active{true}; };
    auto lifetime = std::make_shared<UiLifetime>();
    std::unique_ptr<Download> request;
    Update::Status current;
    winrt::event<winrt::delegate<>> changed;
    bool disposed{}, checked{};
    UINT_PTR checkTimer{}, promptTimer{};
    std::uint64_t requestId{};

    bool busy() { return current.stage == Stage::Checking || current.stage == Stage::Downloading; }
    void publish(Update::Status next) { current = std::move(next); changed(); }
    long long today()
    {
        return std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()).time_since_epoch().count();
    }
    BOOL CALLBACK onEnumWin(HWND hwnd, LPARAM count)
    {
        if (!IsWindowVisible(hwnd)) return TRUE;
        ++*reinterpret_cast<int*>(count);
        return FALSE;
    }
    bool isIdle()
    {
        int count{};
        EnumThreadWindows(GetCurrentThreadId(), onEnumWin, reinterpret_cast<LPARAM>(&count));
        return count == 0;
    }
    void promptLater();
    void CALLBACK onPromptTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (promptTimer == id) promptTimer = 0;
        if (disposed || current.stage != Stage::Available) return;
        if (!isIdle()) { promptLater(); return; }
        const auto text = std::format(L"{} {}\n\n{}", Lang::get(L"update.found"), current.version,
            Lang::get(L"update.promptDownload"));
        const auto answer = MessageBoxW(nullptr, text.c_str(), HuhuCapture::Brand::name.data(),
            MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND);
        if (answer == IDYES) Update::downloadLatest();
    }
    void promptLater()
    {
        if (!disposed && current.stage == Stage::Available && !promptTimer)
            promptTimer = SetTimer(nullptr, 0, 5000, onPromptTimer);
    }
    void begin(bool downloadRequested)
    {
        if (disposed || busy()) return;
        auto* app = Ling::App::get();
        if (!app) return;
        if (checkTimer) KillTimer(nullptr, checkTimer);
        if (promptTimer) KillTimer(nullptr, promptTimer);
        checkTimer = promptTimer = 0;
        if (downloadRequested) {
            checked = true;
            Setting::get()->setUpdateCheckDay(today());
        }
        // Capture all UI-owned state before notification can close a window or
        // dispose the application. A page never owns this application request.
        const auto dispatcher = app->dq;
        const auto localVersion = Ling::Util::getVerNum();
        const auto id = ++requestId;
        const auto alive = lifetime;
        request.reset();
        publish({Stage::Checking});
        if (!alive->active) return;
        try {
            request = std::make_unique<Download>(Options{localVersion, downloadRequested},
                [alive, dispatcher, id](Update::Status next) {
                    if (!alive->active) return;
                    try {
                        dispatcher.TryEnqueue([alive, id, next = std::move(next)]() mutable {
                            if (!alive->active || id != requestId) return;
                            const bool available = next.stage == Stage::Available;
                            publish(std::move(next));
                            if (available && alive->active) promptLater();
                        });
                    } catch (...) {}
                });
        } catch (...) {
            publish({Stage::Failed, {}, {}, 0, 0, Update::Error::Network});
        }
    }
    void CALLBACK onCheckTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (checkTimer == id) checkTimer = 0;
        if (disposed || checked || !isIdle() || busy()) return;
        checked = true;
        Setting::get()->setUpdateCheckDay(today());
        begin(false);
    }
}

void Update::checkLater()
{
    if (disposed || busy() || checked || checkTimer) return;
    auto* app = Ling::App::get();
    if (!app || app->args[L"--auto-quit"] == L"true" || Setting::get()->getUpdateCheckDay() >= today()) return;
    checkTimer = SetTimer(nullptr, 0, 15000, onCheckTimer);
}
void Update::downloadLatest() { begin(true); }
Update::Status Update::status() { return current; }
winrt::event_token Update::subscribe(std::function<void()> handler)
{ return changed.add([handler = std::move(handler)] { handler(); }); }
void Update::unsubscribe(winrt::event_token token) { changed.remove(token); }
void Update::dispose()
{
    disposed = true;
    lifetime->active = false;
    request.reset(); // Cancel the native operation, join, then finish cleanup.
    if (checkTimer) KillTimer(nullptr, checkTimer);
    if (promptTimer) KillTimer(nullptr, promptTimer);
    checkTimer = promptTimer = 0;
}
