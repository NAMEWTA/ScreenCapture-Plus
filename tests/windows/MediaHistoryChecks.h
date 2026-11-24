#pragma once

#include "services/MediaHistory.h"
#include "services/Setting.h"
#include "services/Lang.h"
#include "core/MediaFileName.h"
#include "ui/WinClipboard.h"
#include "ui/settings/WinSetting.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace HuhuCapture::Tests {
namespace Media = HuhuCapture::MediaHistory;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

inline void historyRequire(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

inline void checked(HRESULT result, const char* message)
{
    historyRequire(SUCCEEDED(result), message);
}

inline void writeVideoFixture(const fs::path& destination)
{
    struct Session {
        Session() { checked(MFStartup(MF_VERSION), "Start Media Foundation fixture"); }
        ~Session() { MFShutdown(); }
    } session;
    constexpr UINT width = 64, height = 48, fps = 10;
    ComPtr<IMFSinkWriter> writer;
    checked(MFCreateSinkWriterFromURL(destination.c_str(), nullptr, nullptr, writer.GetAddressOf()), "Create MP4 fixture writer");
    ComPtr<IMFMediaType> output, input;
    checked(MFCreateMediaType(output.GetAddressOf()), "Create MP4 output type");
    checked(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set output video type");
    checked(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Set H264 fixture codec");
    checked(output->SetUINT32(MF_MT_AVG_BITRATE, 256000), "Set fixture bitrate");
    checked(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set output progressive mode");
    checked(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height), "Set output dimensions");
    checked(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1), "Set output framerate");
    checked(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set output aspect");
    DWORD stream = 0;
    checked(writer->AddStream(output.Get(), &stream), "Add fixture stream");
    checked(MFCreateMediaType(input.GetAddressOf()), "Create fixture input type");
    checked(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set input video type");
    checked(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Set RGB32 fixture input");
    checked(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set input progressive mode");
    checked(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height), "Set input dimensions");
    checked(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1), "Set input framerate");
    checked(MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set input aspect");
    checked(writer->SetInputMediaType(stream, input.Get(), nullptr), "Set fixture input format");
    checked(writer->BeginWriting(), "Begin fixture writing");
    for (unsigned frame = 0; frame < 5; ++frame) {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        checked(MFCreateMemoryBuffer(width * height * 4, buffer.GetAddressOf()), "Allocate fixture frame");
        BYTE* pixels = nullptr;
        checked(buffer->Lock(&pixels, nullptr, nullptr), "Lock fixture pixels");
        for (unsigned i = 0; i < width * height; ++i) {
            pixels[i * 4] = 210;
            pixels[i * 4 + 1] = static_cast<BYTE>(80 + frame * 12);
            pixels[i * 4 + 2] = 30;
            pixels[i * 4 + 3] = 255;
        }
        checked(buffer->Unlock(), "Unlock fixture pixels");
        checked(buffer->SetCurrentLength(width * height * 4), "Set fixture frame length");
        checked(MFCreateSample(sample.GetAddressOf()), "Create fixture sample");
        checked(sample->AddBuffer(buffer.Get()), "Attach fixture frame");
        checked(sample->SetSampleTime(frame * 1000000LL), "Set fixture sample time");
        checked(sample->SetSampleDuration(1000000LL), "Set fixture sample duration");
        checked(writer->WriteSample(stream, sample.Get()), "Write fixture video frame");
    }
    checked(writer->Finalize(), "Finalize fixture MP4");
}

inline Ling::Button* historyButton(Ling::Node* node, const wchar_t* id)
{
    if (auto* button = dynamic_cast<Ling::Button*>(node); button && button->id == id) return button;
    for (const auto& child : node->children)
        if (auto* button = historyButton(child.get(), id)) return button;
    return nullptr;
}

inline void captureHistoryWindow(Ling::WinBase* window, const fs::path& destination,
    const std::function<void(DWORD)>& pump)
{
    SetForegroundWindow(window->hwnd);
    window->refresh();
    pump(150);
    RECT bounds{};
    historyRequire(GetWindowRect(window->hwnd, &bounds) != FALSE, "Read settings/history window bounds");
    Platform::GdiCapture capture;
    historyRequire(capture.capture(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top), "Capture settings/history window");
    historyRequire(ImageExport::saveToFile(destination.wstring(), bounds.right - bounds.left,
        bounds.bottom - bounds.top, const_cast<BYTE*>(capture.pixels().data())), "Write settings/history screenshot");
}

inline void runMediaHistoryChecks(const fs::path& evidence, const std::function<void(DWORD)>& pump)
{
    struct Files {
        std::vector<fs::path> values;
        ~Files() { for (const auto& path : values) { std::error_code error; fs::remove(path, error); } }
    } created;
    historyRequire(Setting::get()->getShortcutKey(L"cap") == L"F1", "Fresh configuration must default to F1");
    historyRequire(Core::parseMediaFileName(ImageExport::createFileName(L"png")).has_value(), "Suggested filename must match the history contract");
    constexpr int width = 80, height = 60;
    std::vector<BYTE> pixels(width * height * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 225; pixels[i + 1] = 170; pixels[i + 2] = 60; pixels[i + 3] = 255;
    }
    const auto first = Media::archiveImage(width, height, pixels.data());
    historyRequire(!first.empty(), "PNG capture was not archived in Documents");
    created.values.push_back(first);
    const auto second = Media::archiveImage(width, height, pixels.data());
    historyRequire(!second.empty() && first != second, "Consecutive saves overwrote an archive");
    created.values.push_back(second);
    historyRequire(first.parent_path() == Media::directory(), "Archive missed the system Documents folder");
    historyRequire(Media::archiveFile(first) == first, "Archiving an existing managed capture duplicated it");
    historyRequire(Media::archiveFile(evidence / L"missing.mp4").empty(), "Missing recording was archived");
    historyRequire(Media::archiveImage(0, height, pixels.data()).empty(), "Empty image was archived");
    const auto fixture = evidence / L"recording-fixture.mp4";
    writeVideoFixture(fixture);
    const auto video = Media::archiveFile(fixture);
    historyRequire(!video.empty() && fs::exists(fixture), "Video archive removed its source before successful completion");
    created.values.push_back(video);
    const auto preview = Media::loadThumbnail(video, 192);
    historyRequire(preview != nullptr, "MP4 history has no real video cover");
    BITMAP thumbnail{};
    const auto bitmapResult = GetObjectW(preview, sizeof(thumbnail), &thumbnail);
    DeleteObject(preview);
    historyRequire(bitmapResult && thumbnail.bmWidth > 0 && thumbnail.bmWidth <= 192 && thumbnail.bmHeight > 0,
        "MP4 cover dimensions are invalid");
    const auto pngPreview = Media::loadThumbnail(first, 192);
    historyRequire(pngPreview != nullptr, "PNG history has no thumbnail");
    DeleteObject(pngPreview);

    WinSetting::init();
    pump(200);
    auto* settings = WinSetting::get();
    historyRequire(settings && IsWindow(settings->hwnd), "Settings window did not open");
    historyRequire(Media::copy(settings->hwnd, {first, first.filename().wstring(), fs::file_size(first), false}), "History PNG copy failed");
    historyRequire(IsClipboardFormatAvailable(CF_DIB) && IsClipboardFormatAvailable(CF_DIBV5) &&
        IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG")), "History PNG lost supported clipboard image formats");
    historyRequire(Media::copy(settings->hwnd, {video, video.filename().wstring(), fs::file_size(video), true}), "History video copy failed");
    historyRequire(OpenClipboard(settings->hwnd) != FALSE, "Read history file clipboard");
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    std::vector<wchar_t> file(drop ? DragQueryFileW(drop, 0, nullptr, 0) + 1 : 1);
    if (drop) DragQueryFileW(drop, 0, file.data(), static_cast<UINT>(file.size()));
    CloseClipboard();
    historyRequire(drop && fs::path(file.data()) == video, "Video clipboard still references a temporary recording");

    captureHistoryWindow(settings, evidence / L"04-settings-general.png", pump);
    auto* menu = historyButton(settings->body.get(), L"settings-menu-shortcut");
    historyRequire(menu != nullptr, "Shortcut settings menu missing");
    menu->onClick(menu);
    pump(100);
    auto* shortcut = historyButton(settings->body.get(), L"cap");
    historyRequire(shortcut && shortcut->getText() == L"F1", "Shortcut control does not display F1");
    shortcut->onClick(shortcut);
    SendMessageW(settings->hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    SendMessageW(settings->hwnd, WM_KEYUP, VK_ESCAPE, 0);
    historyRequire(Setting::get()->getShortcutKey(L"cap") == L"F1", "Escape changed the saved shortcut");
    captureHistoryWindow(settings, evidence / L"05-settings-shortcut.png", pump);
    menu = historyButton(settings->body.get(), L"settings-menu-about");
    historyRequire(menu != nullptr, "About settings menu missing");
    menu->onClick(menu);
    pump(100);
    captureHistoryWindow(settings, evidence / L"06-settings-about.png", pump);
    settings->close();
    pump(150);

    WinClipboard::init();
    const auto deadline = GetTickCount64() + 5000;
    while (WinClipboard::get() &&
        (WinClipboard::get()->isLoading() || !WinClipboard::get()->hasThumbnail(first) ||
            !WinClipboard::get()->hasThumbnail(second) || !WinClipboard::get()->hasThumbnail(video)) &&
        GetTickCount64() < deadline) pump(50);
    auto* history = WinClipboard::get();
    historyRequire(history && !history->isLoading() && history->entryCount() >= 3, "History list did not finish loading");
    historyRequire(history->hasThumbnail(first) && history->hasThumbnail(second) && history->hasThumbnail(video),
        "History list did not render the specific PNG and MP4 previews");
    captureHistoryWindow(history, evidence / L"07-clipboard-history.png", pump);
    SendMessageW(history->hwnd, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(history->hwnd, WM_KEYUP, VK_DOWN, 0);
    SendMessageW(history->hwnd, WM_KEYDOWN, VK_F5, 0);
    SendMessageW(history->hwnd, WM_KEYUP, VK_F5, 0);
    pump(200);
    history->close();
    for (unsigned attempt = 0; WinClipboard::get() && attempt < 100; ++attempt) pump(50);
    historyRequire(!WinClipboard::get(), "History worker/window was not released after close");
    std::cout << "PASS F1 defaults, collision-safe Documents archives, real PNG/MP4 covers, image/file clipboard, settings pages and history lifecycle.\n";
}
} // namespace HuhuCapture::Tests
