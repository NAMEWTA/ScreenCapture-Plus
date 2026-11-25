#include "pch.h"
#include "app/Brand.h"
#include "services/Lang.h"
#include "services/Update.h"
#include "ui/Tip.h"
#include "ui/settings/SettingsTheme.h"
#include "ui/settings/WinSettingAbout.h"

#include <algorithm>
#include <shlwapi.h>

namespace Theme = HuhuCapture::UI::SettingsTheme;

WinSettingAbout::WinSettingAbout(Ling::WinBase* parent) : Ling::Node(parent)
{
    tip = std::make_unique<Tip>(win);
    Theme::pageHeading(this, Lang::get(L"setting.about"), Lang::get(L"about.description"));
    auto* information = Theme::card(this);
    information->setPadding(Theme::cardInset, 12.f, Theme::cardInset, 4.f);
    auto* name = Theme::label(information, std::wstring{ HuhuCapture::Brand::name }, 20.f, 28.f);
    name->setFontFamily(Theme::semiboldFamily);
    name->setMarginBottom(4.f);

    const std::wstring keys[]{ L"version", L"project", L"author" };
    for (const auto& key : keys) {
        Theme::divider(information);
        auto* row = Theme::row(information, 44.f);
        Theme::label(row, Lang::get(L"about." + key))->setFlexGrow(1.f);
        if (key == L"version") {
            const auto version = Ling::Util::getVerNum();
            auto* value = Theme::label(row, std::format(L"{}.{}.{}", version[0], version[1], version[2]));
            value->setId(L"settings-version");
            value->setWidth(220.f);
            value->setAlignItems(Ling::Align::FlexEnd);
            continue;
        }
        auto* link = row->makeChild<Ling::Button>();
        link->setId(L"settings-" + key);
        Theme::styleButton(link, 220.f);
        link->setBorderWidth(0.f);
        link->setColor(Theme::blue);
        link->setHoverColor(Theme::focusBlue);
        link->setAlignItems(Ling::Align::FlexEnd);
        link->setPaddingRight(8.f);
        link->setText(std::wstring{ key == L"project" ? HuhuCapture::Brand::repositoryLabel : HuhuCapture::Brand::author });
        link->onClick.add([this, key](Ling::Button*) {
            const auto url = key == L"project" ? HuhuCapture::Brand::repositoryUrl : HuhuCapture::Brand::authorUrl;
            ShellExecuteW(win->hwnd, L"open", url.data(), nullptr, nullptr, SW_SHOWNORMAL);
        });
    }

    auto* actions = Theme::row(this, Theme::controlHeight);
    actions->setMarginTop(Theme::groupGap);
    downloadButton = actions->makeChild<Ling::Button>();
    downloadButton->setId(L"settings-download-update");
    Theme::styleButton(downloadButton, 160.f, true);
    downloadButton->onClick.add([](Ling::Button*) { Update::downloadLatest(); });

    folderButton = actions->makeChild<Ling::Button>();
    folderButton->setId(L"settings-update-folder");
    Theme::styleButton(folderButton, 128.f);
    folderButton->setMarginLeft(8.f);
    folderButton->setText(Lang::get(L"update.openFolder"));
    folderButton->onClick.add([this](Ling::Button*) {
        const auto state = Update::status();
        if (state.file.empty()) return;
        const auto directory = state.file.parent_path();
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(win->hwnd, L"open", directory.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            MessageBoxW(win->hwnd, Lang::get(L"update.openFolderError").c_str(),
                HuhuCapture::Brand::name.data(), MB_OK | MB_ICONWARNING);
        }
    });
    folderButton->onEnter.add([this](Ling::Button* button) {
        const auto state = Update::status();
        if (!state.file.empty()) tip->showAbove(button, state.file.wstring());
    });
    folderButton->onLeave.add([this](Ling::Button* button) { tip->hide(button); });

    auto* releases = actions->makeChild<Ling::Button>();
    releases->setId(L"settings-releases");
    Theme::styleButton(releases, 120.f);
    releases->setMarginLeft(8.f);
    releases->setBorderWidth(0.f);
    releases->setBg(Theme::canvas);
    releases->setColor(Theme::blue);
    releases->setText(Lang::get(L"about.releases"));
    releases->onClick.add([this](Ling::Button*) {
        ShellExecuteW(win->hwnd, L"open", HuhuCapture::Brand::releasesUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
    });

    updateStatus = Theme::label(this, L"", Theme::captionSize, Theme::captionLineHeight, Theme::secondary);
    updateStatus->setId(L"settings-update-status");
    updateStatus->setMarginTop(8.f);
    updateInstructions = Theme::label(this, L"", Theme::captionSize, Theme::captionLineHeight * 3.f, Theme::secondary);
    updateInstructions->setId(L"settings-update-instructions");
    updateInstructions->setMarginTop(4.f);
    downloadPath = Theme::label(this, L"", Theme::captionSize, Theme::captionLineHeight, Theme::secondary);
    downloadPath->setId(L"settings-update-path");
    downloadPath->setMarginTop(4.f);

    const auto weakThis = getWeakThis();
    updateToken = Update::subscribe([this, weakThis] { if (weakThis.lock()) refreshUpdate(); });
    refreshUpdate();
}

WinSettingAbout::~WinSettingAbout()
{
    Update::unsubscribe(updateToken);
}

void WinSettingAbout::refreshUpdate()
{
    using Stage = Update::Stage;
    const auto state = Update::status();
    const bool busy = state.stage == Stage::Checking || state.stage == Stage::Downloading;
    downloadButton->setColor(busy ? Theme::secondary : Theme::surface);
    downloadButton->setHoverColor(busy ? Theme::secondary : Theme::surface);
    downloadButton->setBg(busy ? Theme::hairline : Theme::blue);
    downloadButton->setHoverBg(busy ? Theme::hairline : Theme::focusBlue);
    downloadButton->setBorderColor(busy ? Theme::hairline : Theme::blue);
    downloadButton->setHoverBorderColor(busy ? Theme::hairline : Theme::focusBlue);
    downloadButton->setText(Lang::get(state.stage == Stage::Failed ? L"update.retry" : L"update.download"));
    updateStatus->setColor(state.stage == Stage::Failed ? Theme::warning : Theme::secondary);
    tip->hide();
    folderButton->hide();
    downloadPath->hide();
    updateInstructions->hide();

    switch (state.stage) {
    case Stage::Idle:
    case Stage::Cancelled:
        updateStatus->setText(Lang::get(L"update.idle"));
        break;
    case Stage::Checking:
        downloadButton->setText(Lang::get(L"update.checking"));
        updateStatus->setText(Lang::get(L"update.idle"));
        break;
    case Stage::Available:
        updateStatus->setText(Lang::get(L"update.found") + L" v" + state.version);
        break;
    case Stage::Downloading: {
        const auto percentage = state.total ? (std::min)(state.received, state.total) * 100 / state.total : 0;
        downloadButton->setText(std::format(L"{} {}%", Lang::get(L"update.downloading"), percentage));
        updateStatus->setText(std::format(L"v{} · {}", state.version, Lang::get(L"update.idle")));
        break;
    }
    case Stage::Downloaded: {
        const auto* key = state.packageKind == Update::PackageKind::WindowsInstaller ? L"update.installWindows" :
            state.packageKind == Update::PackageKind::WindowsPortableZip ? L"update.extractWindows" : L"update.downloaded";
        updateStatus->setText(std::format(L"v{} · {}", state.version, Lang::get(L"update.downloaded")));
        if (state.packageKind != Update::PackageKind::LegacyExecutable) {
            auto instructions = Lang::get(key);
            // Ling's basic Label keeps explicit newlines but does not wrap. The
            // existing six-language messages contain two short sentences; give
            // each its own line instead of clipping the installation action.
            auto boundary = instructions.find(L". ");
            if (boundary != instructions.npos) instructions.replace(boundary + 1, 1, L"\n");
            else if ((boundary = instructions.find(L'。')) != instructions.npos) instructions.insert(boundary + 1, L"\n");
            updateInstructions->setText(instructions);
            updateInstructions->show();
        }
        break;
    }
    case Stage::Failed: {
        const wchar_t* key = L"update.networkError";
        switch (state.error) {
        case Update::Error::Metadata: key = L"update.metadataError"; break;
        case Update::Error::Folder: key = L"update.folderError"; break;
        case Update::Error::Integrity: key = L"update.integrityError"; break;
        case Update::Error::Save:
        case Update::Error::DurabilityUnknown: key = L"update.saveError"; break;
        default: break;
        }
        updateStatus->setText(Lang::get(key));
        break;
    }
    }
    if (!state.file.empty()) {
        // A retained file must remain discoverable even when durable completion
        // is uncertain or safe cleanup failed. No path is executed here.
        const auto path = state.file.wstring();
        wchar_t compactPath[64]{};
        const bool compacted = PathCompactPathExW(compactPath, path.c_str(), 32, 0) != FALSE;
        downloadPath->setText(compacted ? compactPath : state.file.filename().wstring());
        downloadPath->show();
        folderButton->show();
    }
}
