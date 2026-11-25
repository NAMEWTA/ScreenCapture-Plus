#pragma once
#include <include/Ling.h>
#include "services/update/WindowsUpdateDownload.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Metadata checks are automatic and idle-only. A download is explicit: the
// verified package is retained in the user's Windows Downloads known folder.
// Downloading never replaces or launches the running application.
class Update
{
public:
    using Stage = HuhuCapture::Updates::WindowsBridge::Stage;
    using Error = HuhuCapture::Updates::WindowsBridge::Error;
    using PackageKind = HuhuCapture::Updates::WindowsBridge::PackageKind;
    using Status = HuhuCapture::Updates::WindowsBridge::Snapshot;

    // These entry points and subscriptions belong to the UI thread. Reopening
    // settings observes the existing request rather than starting another one.
    static void checkLater();
    static void downloadLatest();
    static Status status();
    static winrt::event_token subscribe(std::function<void()> handler);
    static void unsubscribe(winrt::event_token token);
    // Cancels the actual request and waits for its worker/file cleanup. A page
    // destructor only unsubscribes; it must never call this application method.
    static void dispose();
};
