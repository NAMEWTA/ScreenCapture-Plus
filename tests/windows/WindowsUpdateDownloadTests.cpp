// Native transport probe driven by scripts/test-windows-update-bridge.py. This
// compiles the actual bridge worker without Ling or Qt; the Python driver owns a
// real loopback HTTP server and verifies every retained file's exact bytes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include "services/update/WindowsUpdateDownload.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace bridge = HuhuCapture::Updates::WindowsBridge;
using namespace std::chrono_literals;
using namespace winrt::Windows::Data::Json;

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc != 5) throw std::runtime_error("Expected: origin downloads-directory new-report.json download|check|cancel");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const std::wstring mode{argv[4]};
        if (mode != L"download" && mode != L"check" && mode != L"cancel") throw std::runtime_error("Unknown fixture mode");
        std::mutex mutex;
        bridge::Snapshot last;
        std::uint64_t maximumReceived{};
        bool cancelSent{};
        const auto start = std::chrono::steady_clock::now();
        {
            bridge::Download worker({{0, 0, 5}, mode != L"check"}, [&](bridge::Snapshot value) {
                std::lock_guard lock(mutex);
                maximumReceived = std::max(maximumReceived, value.received);
                last = std::move(value);
            }, bridge::LoopbackFixture{argv[1], std::filesystem::absolute(argv[2])});
            while (!worker.waitFor(10ms)) {
                if (std::chrono::steady_clock::now() - start > 60s) throw std::runtime_error("Native bridge fixture deadline exceeded");
                bool received{};
                { std::lock_guard lock(mutex); received = maximumReceived > 0; }
                if (mode == L"cancel" && received && !cancelSent) { worker.cancel(); cancelSent = true; }
            }
        } // Must join and close every native request/file before reporting.
        JsonObject result;
        const auto set = [&](const wchar_t* key, const std::wstring& value) { result.SetNamedValue(key, JsonValue::CreateStringValue(value)); };
        const auto number = [&](const wchar_t* key, std::uint64_t value) { result.SetNamedValue(key, JsonValue::CreateNumberValue(static_cast<double>(value))); };
        const wchar_t* stage{};
        switch (last.stage) {
        case bridge::Stage::Idle: stage = L"idle"; break;
        case bridge::Stage::Checking: stage = L"checking"; break;
        case bridge::Stage::Available: stage = L"available"; break;
        case bridge::Stage::Downloading: stage = L"downloading"; break;
        case bridge::Stage::Downloaded: stage = L"downloaded"; break;
        case bridge::Stage::Failed: stage = L"failed"; break;
        case bridge::Stage::Cancelled: stage = L"cancelled"; break;
        }
        set(L"stage", stage); set(L"file", last.file.wstring()); set(L"version", last.version);
        set(L"packageKind", last.packageKind == bridge::PackageKind::WindowsInstaller ? L"installer" :
            last.packageKind == bridge::PackageKind::WindowsPortableZip ? L"portable-zip" : L"legacy-exe");
        number(L"error", static_cast<unsigned>(last.error)); number(L"received", last.received);
        number(L"total", last.total); number(L"maximumReceived", maximumReceived);
        number(L"elapsedMilliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        result.SetNamedValue(L"cancelSent", JsonValue::CreateBooleanValue(cancelSent));
        const auto bytes = winrt::to_string(result.Stringify());
        const auto file = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Fixture report must be exclusively created");
        DWORD written{};
        const bool saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
        CloseHandle(file);
        if (!saved) throw std::runtime_error("Cannot write native bridge fixture report");
        std::cout << bytes << '\n';
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "Native bridge probe failed: " << winrt::to_string(error.message()) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Native bridge probe failed: " << error.what() << '\n';
    }
    return 1;
}
