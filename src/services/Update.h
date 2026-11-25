#pragma once
#include <include/Ling.h>
#include "platform/windows/LegacyUpdateTask.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Metadata checks are automatic and idle-only. A download is explicit: the
// verified complete package (or legacy EXE) is retained in Windows Downloads.
// Downloading never replaces or launches the running application.
class Update
{
public:
    using Stage = HuhuCapture::Platform::Windows::LegacyUpdates::Stage;
    using Error = HuhuCapture::Platform::Windows::LegacyUpdates::Error;
    using Status = HuhuCapture::Platform::Windows::LegacyUpdates::Status;

    // These entry points and subscriptions belong to the UI thread. Reopening
    // settings observes the existing request rather than starting another one.
    static void checkLater();
    static void downloadLatest();
    // Separate native integration harness only, equivalent to the Qt service's
    // explicit Options injection. No product UI/CLI/configuration calls this;
    // the worker rejects every origin except literal loopback HTTP.
    static void downloadFromLoopbackForValidation(std::wstring origin);
    static Status status();
    static winrt::event_token subscribe(std::function<void()> handler);
    static void unsubscribe(winrt::event_token token);
    // Called after the old main message loop exits. Cancel and join the real
    // MTA worker before App tears down UI/COM; closing About never calls this.
    static void dispose();
};
