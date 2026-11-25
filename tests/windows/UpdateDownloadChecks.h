#pragma once

#include "MediaHistoryChecks.h"
#include "core/ReleaseVersion.h"
#include "platform/FileIdentity.h"
#include "services/Update.h"
#include <set>
#include <shlobj.h>

namespace HuhuCapture::Tests {

inline void runUpdateDownloadChecks(const fs::path& evidence, const std::function<void(DWORD)>& pump,
    const std::wstring& loopbackOrigin = {})
{
    PWSTR knownPath = nullptr;
    const auto folderResult = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &knownPath);
    const fs::path requested = SUCCEEDED(folderResult) && knownPath ? fs::path(knownPath) : fs::path{};
    CoTaskMemFree(knownPath);
    historyRequire(!requested.empty(), "The Windows Downloads known folder is unavailable");
    const auto downloads = fs::canonical(requested);
    std::set<fs::path> existing;
    for (const auto& entry : fs::directory_iterator(downloads)) existing.insert(entry.path());
    struct CreatedDownload {
        fs::path path;
        std::optional<Platform::FileSnapshot> snapshot;
        ~CreatedDownload()
        {
            if (path.empty() || !snapshot) return;
            std::error_code error;
            const auto actual = Platform::readFileSnapshot(path, error);
            if (actual && actual->identity == snapshot->identity) fs::remove(path, error);
        }
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
    if (loopbackOrigin.empty()) {
        nativeClick(settings->hwnd, {static_cast<LONG>(button->x + button->w / 2.f),
            static_cast<LONG>(button->y + button->h / 2.f)}, pump);
    } else {
        // The public button retains its real GitHub check above. The separate
        // integration harness injects only a bounded loopback source into the
        // same app-owned service to exercise unpublished complete packages.
        Update::downloadFromLoopbackForValidation(loopbackOrigin);
        pump(50);
    }
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
    std::error_code identityError;
    created.snapshot = Platform::readFileSnapshot(created.path, identityError);
    historyRequire(created.snapshot.has_value(), "Cannot establish ownership of the actual completed test download");
    historyRequire(fs::is_regular_file(result.file) && fs::file_size(result.file) == result.total && result.total > 0,
        "Downloaded executable is missing or incomplete");
    if (result.packageKind) {
        historyRequire(*result.packageKind == Core::UpdatePackageKind::WindowsPortableZip && result.file.extension() == L".zip",
            "Standalone old installation did not receive a complete portable package");
    } else {
        historyRequire(result.file.extension() == L".exe", "Updater exposed a partial download as the result");
        const auto version = Core::parseReleaseVersion(result.version);
        historyRequire(version && Ling::Util::getVerNum(result.file.wstring()) == *version,
            "Downloaded executable version differs from the selected release");
    }
    historyRequire(loopbackOrigin.empty() || result.packageKind.has_value(), "Controlled bridge journey unexpectedly used legacy fallback");
    historyRequire(historyButton(settings->body.get(), L"settings-update-folder") &&
        settingsNode(settings->body.get(), L"settings-update-status") &&
        settingsNode(settings->body.get(), L"settings-update-path"), "Completed download controls are missing after reopening About");
    captureHistoryWindow(settings, evidence / L"20-update-downloaded.png", pump);
    std::cout << "PASS actual About download: selected " << Ling::Util::convertToStr(result.version)
        << ", Windows Downloads known folder, " << result.total
        << " bytes, verified retained file and close/reopen status.\n";
    std::cout << "Downloaded filename: " << Ling::Util::convertToStr(result.file.filename().wstring()) << '\n';
    settings->close();
    pump(100);
    if (result.packageKind) {
        const auto previousLanguage = Setting::get()->getLang();
        for (const auto* language : {L"en-US", L"zh-CN", L"zh-TW", L"ja-JP", L"ru-RU", L"id-ID"}) {
            Setting::get()->setLang(language);
            settings = about();
            SetWindowPos(settings->hwnd, nullptr, 0, 0, static_cast<int>(640.f * settings->dpi),
                static_cast<int>(400.f * settings->dpi), SWP_NOMOVE | SWP_NOZORDER);
            pump(200);
            auto* node = settingsNode(settings->body.get(), L"settings-update-status");
            auto* path = settingsNode(settings->body.get(), L"settings-update-path");
            auto* label = dynamic_cast<Ling::Label*>(node);
            historyRequire(label && path, "Complete package status/path missing after language recreation");
            auto displayed = label->getText();
            std::erase(displayed, L'\n');
            const auto expected = std::format(L"v{} · {}", result.version, Lang::get(L"update.extractWindows"));
            historyRequire(displayed == expected, "About omitted full localized package instructions");
            historyRequire(node->x >= 0 && node->x + node->w <= settings->w + 1 &&
                path->y + path->h <= settings->h - 8.f * settings->dpi,
                "Complete package instructions/path escaped minimum-size About");
            captureHistoryWindow(settings, evidence / (std::wstring(L"21-package-instructions-") + language + L".png"), pump);
            settings->close(); pump(100);
        }
        Setting::get()->setLang(previousLanguage);
        std::cout << "PASS actual completed package state across six language/window recreations and minimum-size layout.\n";
    }
}

} // namespace HuhuCapture::Tests
