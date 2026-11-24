#pragma once
#include <include/Ling.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Metadata checks are automatic and idle-only. A download is explicit: the
// verified executable is retained in the user's Windows Downloads known folder.
// Downloading never replaces or launches the running application.
class Update
{
public:
    enum class Stage { Idle, Checking, Available, Downloading, Downloaded, Failed };
    enum class Error { None, Network, Metadata, Folder, Integrity, Save };
    struct Status {
        Stage stage{ Stage::Idle };
        std::wstring version;
        std::filesystem::path file;
        std::uint64_t received{}, total{};
        Error error{ Error::None };
    };

    // These entry points and subscriptions belong to the UI thread. Reopening
    // settings observes the existing request rather than starting another one.
    static void checkLater();
    static void downloadLatest();
    static Status status();
    static winrt::event_token subscribe(std::function<void()> handler);
    static void unsubscribe(winrt::event_token token);
    static void dispose();
};
