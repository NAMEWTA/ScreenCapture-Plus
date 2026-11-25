#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "services/Setting.h"
#include "services/Update.h"

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <optional>

namespace {
    namespace Native = HuhuCapture::Platform::Windows::LegacyUpdates;
    using Dispatcher = winrt::Windows::System::DispatcherQueue;
    using Stage = Update::Stage;
    struct Notice { std::uint64_t request{}; Update::Status value; };
    struct UpdateSession {
        std::atomic_bool active{true};
        std::mutex mutex;
        std::optional<Notice> latest;
        bool queued{};
    };
    auto session = std::make_shared<UpdateSession>();
    std::unique_ptr<Native::Task> task;
    Update::Status current;
    winrt::event<winrt::delegate<>> changed;
    bool disposed{}, checked{};
    UINT_PTR checkTimer{}, promptTimer{};
    std::uint64_t requestId{};

    bool busy() { return current.stage == Stage::Checking || current.stage == Stage::Downloading; }
    void publish(Update::Status next) { current = std::move(next); changed(); }
    void promptLater();
    void post(const std::shared_ptr<UpdateSession>& lifetime, const Dispatcher& dispatcher,
        std::uint64_t id, Update::Status next)
    {
        if (!lifetime->active.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lock(lifetime->mutex);
            lifetime->latest = Notice{id, std::move(next)};
            if (lifetime->queued) return;
            lifetime->queued = true;
        }
        bool queued{};
        try {
            queued = dispatcher.TryEnqueue([lifetime] {
                std::optional<Notice> notice;
                { std::lock_guard lock(lifetime->mutex); notice.swap(lifetime->latest); lifetime->queued = false; }
                if (!lifetime->active.load(std::memory_order_acquire) || !notice || notice->request != requestId) return;
                publish(std::move(notice->value));
                if (!disposed && notice->request == requestId && current.stage == Stage::Available) promptLater();
            });
        } catch (...) {}
        if (!queued) { std::lock_guard lock(lifetime->mutex); lifetime->queued = false; lifetime->latest.reset(); }
    }
    long long today()
    { return std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()).time_since_epoch().count(); }
    BOOL CALLBACK onEnumWin(HWND hwnd, LPARAM count)
    {
        if (!IsWindowVisible(hwnd)) return TRUE;
        ++*reinterpret_cast<int*>(count); return FALSE;
    }
    bool isIdle()
    {
        int count{};
        EnumThreadWindows(GetCurrentThreadId(), onEnumWin, reinterpret_cast<LPARAM>(&count));
        return count == 0;
    }
    void CALLBACK onPromptTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (promptTimer == id) promptTimer = 0;
        if (disposed || current.stage != Stage::Available) return;
        if (!isIdle()) { promptLater(); return; }
        const auto text = std::format(L"{} {}\n\n{}", Lang::get(L"update.found"), current.version,
            Lang::get(current.packageKind ? L"update.promptPackageDownload" : L"update.promptDownload"));
        const auto answer = MessageBoxW(nullptr, text.c_str(), HuhuCapture::Brand::name.data(),
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONINFORMATION | MB_SETFOREGROUND);
        if (answer == IDYES && !disposed) Update::downloadLatest();
    }
    void promptLater()
    {
        if (!disposed && current.stage == Stage::Available && !promptTimer)
            promptTimer = SetTimer(nullptr, 0, 5000, onPromptTimer);
    }
    void begin(bool downloadRequested, std::optional<std::wstring> testOrigin = {})
    {
        if (disposed || busy() || (task && !task->finished())) return;
        auto* app = Ling::App::get();
        if (!app) return;
        task.reset(); // A previous terminal worker is already finished; reap it.
        if (checkTimer) KillTimer(nullptr, checkTimer);
        if (promptTimer) KillTimer(nullptr, promptTimer);
        checkTimer = promptTimer = 0;
        if (downloadRequested) {
            checked = true;
            Setting::get()->setUpdateCheckDay(today());
        }
        const auto id = ++requestId;
        const auto version = Ling::Util::getVerNum();
        Native::Options options;
        options.currentVersion = std::format(L"{}.{}.{}", version[0], version[1], version[2]);
        options.download = downloadRequested;
        options.loopbackTestOrigin = std::move(testOrigin);
        const auto dispatcher = app->dq;
        publish({Stage::Checking});
        if (disposed || id != requestId) return;
        try {
            task = std::make_unique<Native::Task>(std::move(options), [lifetime = session, dispatcher, id](Update::Status status) {
                post(lifetime, dispatcher, id, std::move(status));
            });
        } catch (...) { publish({Stage::Failed, {}, {}, 0, 0, Update::Error::Network}); }
    }
    void CALLBACK onCheckTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (checkTimer == id) checkTimer = 0;
        if (disposed || checked || busy()) return;
        if (!isIdle()) { Update::checkLater(); return; }
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
void Update::downloadFromLoopbackForValidation(std::wstring origin) { begin(true, std::move(origin)); }
Update::Status Update::status() { return current; }
winrt::event_token Update::subscribe(std::function<void()> handler)
{ return changed.add([handler = std::move(handler)] { handler(); }); }
void Update::unsubscribe(winrt::event_token token) { changed.remove(token); }
void Update::dispose()
{
    disposed = true;
    session->active.store(false, std::memory_order_release);
    if (checkTimer) KillTimer(nullptr, checkTimer);
    if (promptTimer) KillTimer(nullptr, promptTimer);
    checkTimer = promptTimer = 0;
    // This is final old-process teardown, after its message loop. Task owns all
    // request/stream/file objects and joins before Ling::dispose uninitializes
    // COM. No detached coroutine can outlive these dependencies.
    if (task) { task->cancel(); task->join(); task.reset(); }
    { std::lock_guard lock(session->mutex); session->latest.reset(); }
}
