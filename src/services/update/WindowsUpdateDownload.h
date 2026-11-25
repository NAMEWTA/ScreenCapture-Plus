#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace HuhuCapture::Updates::WindowsBridge {

enum class Stage { Idle, Checking, Available, Downloading, Downloaded, Failed, Cancelled };
enum class Error { None, Network, Metadata, Folder, Integrity, Save, DurabilityUnknown };
enum class PackageKind { LegacyExecutable, WindowsInstaller, WindowsPortableZip };
struct Snapshot {
    Stage stage{Stage::Idle};
    std::wstring version;
    std::filesystem::path file;
    std::uint64_t received{}, total{};
    Error error{Error::None};
    PackageKind packageKind{PackageKind::LegacyExecutable};
};
struct Options {
    std::array<int, 3> currentVersion{};
    bool download{true};
};
// Explicit test construction only. Production has no command-line, configuration
// or environment override for either the canonical release source or Downloads.
// Metadata still contains canonical GitHub URLs; only transport is looped back.
struct LoopbackFixture {
    std::wstring origin;
    std::filesystem::path downloadsDirectory;
};

// One temporary legacy bridge request. All WinRT networking, hashing, parsing and
// file operations run on its MTA worker. The sink receives copied snapshots on
// that worker and must marshal them to the UI; it must not destroy this object.
// Destruction cancels the current native operation, joins, and finishes owned-file
// cleanup. Closing a settings page never owns or cancels this request.
class Download final {
public:
    Download(Options options, std::function<void(Snapshot)> sink,
        std::optional<LoopbackFixture> fixture = std::nullopt);
    ~Download();
    Download(const Download&) = delete;
    Download& operator=(const Download&) = delete;
    void cancel() noexcept;
    bool waitFor(std::chrono::milliseconds timeout);
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Updates::WindowsBridge
