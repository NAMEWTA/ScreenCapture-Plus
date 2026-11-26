#pragma once

#include "core/UpdatePolicy.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace HuhuCapture::Platform::Windows::LegacyUpdates {

// Transitional WinRT transport for the still-shipping Windows single EXE.
// Metadata and package rules are the same Core::UpdateManifest policy used by
// the Qt updater. This adapter is removed with the old product at P13.
enum class Stage { Idle, Checking, Available, Downloading, Downloaded, Failed, Cancelled };
enum class Error { None, Network, Metadata, Folder, Integrity, Save, DurabilityUnknown };
struct Status {
    Stage stage{Stage::Idle};
    std::wstring version;
    std::filesystem::path file;
    std::uint64_t received{}, total{};
    Error error{Error::None};
    // Empty only for the old, independently verified standalone EXE fallback.
    std::optional<Core::UpdatePackageKind> packageKind;
    std::wstring minimumSystem;
    std::int32_t nativeError{};
};
struct Options {
    std::wstring currentVersion;
    // Empty resolves the actual Windows Downloads known folder on the worker.
    std::filesystem::path downloadsDirectory;
    bool download{};
    // Explicit test injection only; production never reads this from the user,
    // arguments, settings or environment. Only literal local HTTP is accepted.
    std::optional<std::wstring> loopbackTestOrigin;
};

class Task final {
public:
    // Called only on the task's MTA worker. The old UI adapter marshals a
    // coalesced immutable status to its dispatcher, independent of any page.
    using Observer = std::function<void(Status)>;
    Task(Options options, Observer observer);
    ~Task();
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    void cancel() noexcept;
    bool finished() const noexcept;
    // Final process teardown, after cancelling, must join before COM teardown.
    // The running UI polls finished; it never waits on a live network request.
    void join();
private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread worker_;
};

} // namespace HuhuCapture::Platform::Windows::LegacyUpdates
