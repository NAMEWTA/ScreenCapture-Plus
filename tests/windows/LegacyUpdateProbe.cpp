#include "core/UpdatePolicy.h"
#include "platform/windows/LegacyUpdateTask.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <winrt/Windows.Data.Json.h>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Native = HuhuCapture::Platform::Windows::LegacyUpdates;
using namespace winrt::Windows::Data::Json;
using namespace std::chrono_literals;

namespace {
const wchar_t* stageName(Native::Stage value)
{
    switch (value) {
    case Native::Stage::Idle: return L"idle";
    case Native::Stage::Checking: return L"checking";
    case Native::Stage::Available: return L"available";
    case Native::Stage::Downloading: return L"downloading";
    case Native::Stage::Downloaded: return L"downloaded";
    case Native::Stage::Failed: return L"failed";
    case Native::Stage::Cancelled: return L"cancelled";
    }
    return L"invalid";
}
std::size_t peakWorkingSet()
{
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        throw std::runtime_error("Actual process working-set counter is unavailable");
    return memory.PeakWorkingSetSize;
}
JsonObject encode(const Native::Status& status)
{
    JsonObject value;
    value.SetNamedValue(L"stage", JsonValue::CreateStringValue(stageName(status.stage)));
    value.SetNamedValue(L"version", JsonValue::CreateStringValue(status.version));
    value.SetNamedValue(L"file", JsonValue::CreateStringValue(status.file.wstring()));
    value.SetNamedValue(L"received", JsonValue::CreateNumberValue(static_cast<double>(status.received)));
    value.SetNamedValue(L"total", JsonValue::CreateNumberValue(static_cast<double>(status.total)));
    value.SetNamedValue(L"error", JsonValue::CreateNumberValue(static_cast<int>(status.error)));
    value.SetNamedValue(L"nativeError", JsonValue::CreateNumberValue(status.nativeError));
    const auto kind = status.packageKind ? HuhuCapture::Core::updatePackagePolicy(*status.packageKind).kind : "legacy-executable";
    value.SetNamedValue(L"packageKind", JsonValue::CreateStringValue(winrt::to_hstring(kind)));
    return value;
}
} // namespace

// Only this separate test executable accepts a loopback origin. The shipped old
// application has no argument/environment/settings path to override its source.
int wmain(int argc, wchar_t** argv)
{
    std::cout.setf(std::ios::unitbuf);
    try {
        if (argc < 5 || argc > 6) throw std::runtime_error("Usage: LegacyUpdateProbe <version> <loopback-origin> <Downloads-or-dash> <download|check|cancel-first> [cancel-after-bytes]");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        Native::Options options;
        options.currentVersion = argv[1]; options.loopbackTestOrigin = argv[2];
        if (std::wstring_view(argv[3]) != L"-") options.downloadsDirectory = argv[3];
        const std::wstring_view mode(argv[4]);
        if (mode != L"download" && mode != L"check" && mode != L"cancel-first") throw std::runtime_error("Invalid probe action");
        options.download = mode != L"check";
        std::uint64_t cancelAfter{};
        if (argc == 6) {
            wchar_t* end{};
            cancelAfter = std::wcstoull(argv[5], &end, 10);
            if (!end || *end || !cancelAfter) throw std::runtime_error("Invalid actual cancellation threshold");
        }
        const auto baseline = peakWorkingSet();
        std::mutex mutex;
        Native::Status latest;
        std::atomic_bool cancelRequested{};
        const auto start = std::chrono::steady_clock::now();
        Native::Task task(std::move(options), [&](Native::Status status) {
            if (cancelAfter && status.stage == Native::Stage::Downloading && status.received >= cancelAfter)
                cancelRequested.store(true, std::memory_order_release);
            const auto message = encode(status);
            std::cout << winrt::to_string(message.Stringify()) << '\n';
            std::lock_guard lock(mutex); latest = std::move(status);
        });
        if (mode == L"cancel-first") task.cancel();
        bool timedOut{};
        while (!task.finished()) {
            if (cancelRequested.exchange(false, std::memory_order_acq_rel)) task.cancel();
            if (std::chrono::steady_clock::now() - start > 120s) { timedOut = true; task.cancel(); }
            std::this_thread::sleep_for(5ms);
        }
        task.join();
        JsonObject final;
        { std::lock_guard lock(mutex); final = encode(latest); }
        final.SetNamedValue(L"joined", JsonValue::CreateBooleanValue(true));
        final.SetNamedValue(L"timedOut", JsonValue::CreateBooleanValue(timedOut));
        const auto peak = peakWorkingSet();
        final.SetNamedValue(L"peakAdditionalWorkingSet", JsonValue::CreateNumberValue(static_cast<double>(peak > baseline ? peak - baseline : 0)));
        final.SetNamedValue(L"milliseconds", JsonValue::CreateNumberValue(static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count())));
        std::cout << winrt::to_string(final.Stringify()) << '\n';
        return timedOut ? 1 : 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "Legacy bridge native harness HRESULT " << error.code().value << '\n'; return 1;
    } catch (const std::exception& error) { std::cerr << "Legacy bridge harness: " << error.what() << '\n'; return 1; }
}
