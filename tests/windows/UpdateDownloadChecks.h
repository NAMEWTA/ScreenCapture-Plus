#pragma once

#include "MediaHistoryChecks.h"
#include "core/ReleaseVersion.h"
#include "services/Update.h"
#include <set>
#include <fstream>
#include <shlobj.h>

namespace HuhuCapture::Tests {

inline void runUpdateDownloadChecks(const fs::path& evidence, const std::function<void(DWORD)>& pump)
{
    PWSTR knownPath = nullptr;
    const auto folderResult = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &knownPath);
    const fs::path downloads = SUCCEEDED(folderResult) && knownPath ? fs::path(knownPath) : fs::path{};
    CoTaskMemFree(knownPath);
    historyRequire(!downloads.empty(), "The Windows Downloads known folder is unavailable");
    std::set<fs::path> existing;
    for (const auto& entry : fs::directory_iterator(downloads)) existing.insert(entry.path());
    struct CreatedDownload {
        fs::path path;
        ~CreatedDownload() { if (!path.empty()) { std::error_code error; fs::remove(path, error); } }
    } created;

    const auto about = [&pump]() {
        WinSetting::init();
        pump(100);
        auto* settings = WinSetting::get();
        historyRequire(settings && IsWindow(settings->hwnd), "Open settings for the live update check");
        auto* menu = historyButton(settings->body.get(), L"settings-menu-about");
        historyRequire(menu != nullptr, "About menu missing from the update check");
        nativeClick(settings->hwnd, {static_cast<LONG>(menu->x + menu->w / 2.f),
            static_cast<LONG>(menu->y + menu->h / 2.f)}, pump);
        return settings;
    };

    auto* settings = about();
    auto* button = historyButton(settings->body.get(), L"settings-download-update");
    historyRequire(button != nullptr, "About page has no direct download button");
    nativeClick(settings->hwnd, {static_cast<LONG>(button->x + button->w / 2.f),
        static_cast<LONG>(button->y + button->h / 2.f)}, pump);
    historyRequire(Update::status().stage != Update::Stage::Idle, "Download button did not start a request");

    // Remove the original subscriber while the request may still be active.
    // A new About page must observe the same request and receive its completion.
    settings->close();
    for (unsigned attempt = 0; WinSetting::get() && attempt < 20; ++attempt) pump(25);
    historyRequire(!WinSetting::get(), "Settings was not released during the download");
    settings = about();
    const auto deadline = GetTickCount64() + 80000;
    while (GetTickCount64() < deadline) {
        const auto stage = Update::status().stage;
        if (stage == Update::Stage::Downloaded || stage == Update::Stage::Failed) break;
        pump(50);
    }
    const auto result = Update::status();
    if (result.stage == Update::Stage::Failed)
        throw std::runtime_error("Live Downloads request failed; updater error " + std::to_string(static_cast<int>(result.error)));
    historyRequire(result.stage == Update::Stage::Downloaded, "Live Downloads request did not finish within its network budget");
    historyRequire(result.file.parent_path() == downloads, "Updater saved outside the Windows Downloads known folder");
    historyRequire(!existing.contains(result.file), "Updater reused or overwrote a pre-existing Downloads file");
    created.path = result.file;
    historyRequire(fs::is_regular_file(result.file) && fs::file_size(result.file) == result.total && result.total > 0,
        "Downloaded package is missing or incomplete");
    const auto version = Core::parseReleaseVersion(result.version);
    historyRequire(version.has_value(), "Selected stable version is invalid");
    if (result.packageKind == Update::PackageKind::WindowsPortableZip) {
        std::ifstream file(result.file, std::ios::binary);
        std::array<char, 4> header{}; file.read(header.data(), header.size());
        historyRequire(result.file.extension() == L".zip" && header == std::array<char, 4>{'P', 'K', 3, 4},
            "Completed portable package is not a ZIP");
    } else {
        historyRequire(result.file.extension() == L".exe" && Ling::Util::getVerNum(result.file.wstring()) == *version,
            "Downloaded application/installer PE version differs from the selected release");
    }
    historyRequire(historyButton(settings->body.get(), L"settings-update-folder") &&
        settingsNode(settings->body.get(), L"settings-update-status") &&
        settingsNode(settings->body.get(), L"settings-update-path"), "Completed download controls are missing after reopening About");
    captureHistoryWindow(settings, evidence / L"20-update-downloaded.png", pump);
    std::cout << "PASS live About download: remote " << Ling::Util::convertToStr(result.version)
        << ", Windows Downloads known folder, " << result.total
        << " bytes, validated package form, retained file and close/reopen status.\n";
    std::cout << "Downloaded filename: " << Ling::Util::convertToStr(result.file.filename().wstring()) << '\n';
    settings->close();
    pump(100);
}

} // namespace HuhuCapture::Tests
