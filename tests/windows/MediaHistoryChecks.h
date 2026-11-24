#pragma once

#include "services/MediaHistory.h"
#include "services/Setting.h"
#include "services/Lang.h"
#include "core/MediaFileName.h"
#include "ui/WinClipboard.h"
#include "ui/settings/WinSetting.h"
#include "ui/settings/WinSettingCommon.h"
#include "ui/settings/SettingsSelectPopup.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
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
    const std::function<void(DWORD)>& pump, bool activate = true)
{
    if (activate) SetForegroundWindow(window->hwnd);
    window->refresh();
    pump(150);
    RECT bounds{};
    historyRequire(GetWindowRect(window->hwnd, &bounds) != FALSE, "Read settings/history window bounds");
    Platform::GdiCapture capture;
    historyRequire(capture.capture(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top), "Capture settings/history window");
    historyRequire(ImageExport::saveToFile(destination.wstring(), bounds.right - bounds.left,
        bounds.bottom - bounds.top, const_cast<BYTE*>(capture.pixels().data())), "Write settings/history screenshot");
}

inline Ling::Node* settingsNode(Ling::Node* node, const wchar_t* id)
{
    if (!node) return nullptr;
    if (node->id == id) return node;
    for (const auto& child : node->children)
        if (auto* result = settingsNode(child.get(), id)) return result;
    return nullptr;
}

inline WinSettingCommon* settingsCommon(Ling::Node* node)
{
    if (!node) return nullptr;
    if (auto* common = dynamic_cast<WinSettingCommon*>(node)) return common;
    for (const auto& child : node->children)
        if (auto* common = settingsCommon(child.get())) return common;
    return nullptr;
}

// Drive the actual HWND handlers, including activation before the click. The
// separate trigger-toggle check uses SendInput to exercise OS activation order.
inline void nativeClick(HWND window, POINT point, const std::function<void(DWORD)>& pump)
{
    historyRequire(IsWindow(window) != FALSE, "Settings click target no longer exists");
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
    POINT screen = point;
    historyRequire(ClientToScreen(window, &screen) != FALSE, "Convert settings click to screen coordinates");
    historyRequire(SetCursorPos(screen.x, screen.y) != FALSE, "Position pointer for settings click");
    const auto coordinates = MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
    SendMessageW(window, WM_MOUSEMOVE, 0, coordinates);
    struct KeyboardState {
        std::array<BYTE, 256> original{};
        KeyboardState() { historyRequire(GetKeyboardState(original.data()) != FALSE, "Read settings click keyboard state"); }
        ~KeyboardState() { SetKeyboardState(original.data()); }
    } keyboard;
    auto pressed = keyboard.original;
    pressed[VK_LBUTTON] |= 0x80;
    historyRequire(SetKeyboardState(pressed.data()) != FALSE, "Set settings click button state");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, coordinates);
    SetKeyboardState(keyboard.original.data());
    if (IsWindow(window)) SendMessageW(window, WM_LBUTTONUP, 0, coordinates);
    pump(60);
}

inline void settingsKey(HWND window, UINT key, const std::function<void(DWORD)>& pump)
{
    SendMessageW(window, WM_KEYDOWN, key, 0);
    if (IsWindow(window)) SendMessageW(window, WM_KEYUP, key, 0);
    pump(40);
}

inline SettingsSelectPopup* openSettingsLanguage(WinSetting* settings, const std::function<void(DWORD)>& pump)
{
    auto* common = settingsCommon(settings->body.get());
    auto* trigger = historyButton(settings->body.get(), L"settings-language");
    historyRequire(common && trigger, "General settings language controls are missing");
    nativeClick(settings->hwnd, {static_cast<LONG>(trigger->x + trigger->w / 2.f),
        static_cast<LONG>(trigger->y + trigger->h / 2.f)}, pump);
    auto* popup = common->languagePopup();
    historyRequire(popup && popup->isOpen(), "Mouse click did not open the language popup");
    return popup;
}

inline RECT settingsWorkArea(HWND window)
{
    MONITORINFO monitor{sizeof(MONITORINFO)};
    historyRequire(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
        "Read settings monitor work area");
    return monitor.rcWork;
}

inline RECT setNativeTestDpi(Ling::WinBase* settings, UINT dpi, float logicalWidth, float logicalHeight,
    const std::function<void(DWORD)>& pump)
{
    const auto work = settingsWorkArea(settings->hwnd);
    const float scale = dpi / 96.f;
    const LONG width = (std::min)(static_cast<LONG>(std::lround(logicalWidth * scale)), work.right - work.left - 32);
    const LONG height = (std::min)(static_cast<LONG>(std::lround(logicalHeight * scale)), work.bottom - work.top - 32);
    RECT suggested{work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, 0, 0};
    suggested.right = suggested.left + width;
    suggested.bottom = suggested.top + height;
    // This exercises the production DPI message handler and native layout on the
    // available desktop. It does not reconfigure or simulate a physical monitor.
    SendMessageW(settings->hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
    pump(80);
    RECT actual{};
    historyRequire(GetWindowRect(settings->hwnd, &actual) != FALSE, "Read resized settings bounds");
    historyRequire(std::abs(settings->dpi - scale) < 0.001f, "Settings ignored the DPI change");
    historyRequire(actual.right - actual.left == width && actual.bottom - actual.top == height,
        "Settings physical size does not match the clamped DPI suggestion");
    return actual;
}

inline RECT setSettingsTestDpi(WinSetting* settings, UINT dpi, const std::function<void(DWORD)>& pump)
{
    return setNativeTestDpi(settings, dpi, 720.f, 500.f, pump);
}

inline void runSettingsDesignChecks(WinSetting* settings, const fs::path& evidence,
    const std::function<void(DWORD)>& pump)
{
    auto* menu = historyButton(settings->body.get(), L"settings-menu-common");
    historyRequire(menu != nullptr, "General settings menu missing");
    nativeClick(settings->hwnd, {static_cast<LONG>(menu->x + menu->w / 2.f),
        static_cast<LONG>(menu->y + menu->h / 2.f)}, pump);
    const auto originalLanguage = Setting::get()->getLang();
    historyRequire(originalLanguage == L"zh-CN", "Fresh GUI configuration must start in Simplified Chinese");
    struct ScaleEvidence { UINT dpi; const wchar_t* settingsFile; const wchar_t* menuFile; };
    const ScaleEvidence scales[]{
        {96, L"08-settings-100.png", L"09-language-menu-100.png"},
        {120, L"10-settings-125.png", L"11-language-menu-125.png"},
        {144, L"12-settings-150.png", L"13-language-menu-150.png"}
    };
    for (const auto& scale : scales) {
        const auto bounds = setSettingsTestDpi(settings, scale.dpi, pump);
        const float factor = scale.dpi / 96.f;
        auto* common = settingsCommon(settings->body.get());
        auto* host = settingsNode(settings->body.get(), L"settings-content-host");
        auto* trigger = historyButton(settings->body.get(), L"settings-language");
        historyRequire(common && host && trigger, "Settings content disappeared after DPI change");
        historyRequire(std::abs(trigger->w - 180.f * factor) <= 1.f &&
            std::abs(trigger->h - 32.f * factor) <= 1.f, "Language button was not scaled exactly once");
        const float leftGap = common->x - host->x;
        const float rightGap = host->x + host->w - common->x - common->w;
        historyRequire(leftGap >= 0.f && rightGap >= 0.f && std::abs(leftGap - rightGap) <= 2.f,
            "Settings content column is not horizontally centered");
        captureHistoryWindow(settings, evidence / scale.settingsFile, pump);
        auto* popup = openSettingsLanguage(settings, pump);
        RECT popupBounds{};
        historyRequire(GetWindowRect(popup->hwnd, &popupBounds) != FALSE, "Read language popup bounds");
        const auto work = settingsWorkArea(settings->hwnd);
        historyRequire(popupBounds.left >= work.left && popupBounds.top >= work.top &&
            popupBounds.right <= work.right && popupBounds.bottom <= work.bottom,
            "Language popup escaped the monitor work area");
        historyRequire(std::abs((popupBounds.right - popupBounds.left) - trigger->w) <= 1.f,
            "Language popup width does not match its trigger");
        // Capturing the owner must not activate it and cancel its open popup.
        captureHistoryWindow(settings, evidence / scale.menuFile, pump, false);
        historyRequire(common->languagePopup() == popup && popup->isOpen(), "Capturing settings dismissed the language popup");
        settingsKey(popup->hwnd, VK_HOME, pump);
        settingsKey(popup->hwnd, VK_DOWN, pump);
        historyRequire(popup->highlightedValue() == L"en-US", "Arrow key did not move the language highlight");
        historyRequire(Setting::get()->getLang() == originalLanguage, "Highlighting a language committed it");
        settingsKey(popup->hwnd, VK_ESCAPE, pump);
        historyRequire(common->languagePopup() == nullptr, "Escape did not release the language popup");
        historyRequire(Setting::get()->getLang() == originalLanguage, "Escape changed the saved language");
        std::cout << "PASS settings controlled WM_DPICHANGED " << scale.dpi << " DPI: requested 720x500 DIP, actual "
            << bounds.right - bounds.left << 'x' << bounds.bottom - bounds.top << " px, compact="
            << (settings->h / settings->dpi < 460.f) << ", centered column, 180x32 DIP trigger, menu, arrows and Escape.\n";
    }

    const auto restored = setSettingsTestDpi(settings, 96, pump);
    auto* common = settingsCommon(settings->body.get());
    historyRequire(common != nullptr, "General settings disappeared before dismissal checks");
    struct NativeWindow {
        HWND handle{};
        ~NativeWindow() { if (handle && IsWindow(handle)) DestroyWindow(handle); }
    } outside;
    outside.handle = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Settings focus test", WS_POPUP,
        0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    historyRequire(outside.handle != nullptr, "Create external activation test window");
    openSettingsLanguage(settings, pump);
    ShowWindow(outside.handle, SW_SHOWNORMAL);
    SetForegroundWindow(outside.handle);
    SetActiveWindow(outside.handle);
    SetFocus(outside.handle);
    pump(60);
    historyRequire(GetFocus() == outside.handle, "External activation did not take native focus");
    historyRequire(common->languagePopup() == nullptr, "External activation did not dismiss the language popup");
    DestroyWindow(outside.handle);
    outside.handle = nullptr;

    openSettingsLanguage(settings, pump);
    historyRequire(SetWindowPos(settings->hwnd, nullptr, restored.left + 8, restored.top + 6, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Move settings while the language popup is open");
    pump(60);
    historyRequire(common->languagePopup() == nullptr, "Moving settings did not dismiss its language popup");
    historyRequire(SetWindowPos(settings->hwnd, nullptr, restored.left, restored.top, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Restore settings after the move check");
    pump(40);
    openSettingsLanguage(settings, pump);
    auto* trigger = historyButton(settings->body.get(), L"settings-language");
    POINT togglePoint{static_cast<LONG>(trigger->x + trigger->w / 2.f),
        static_cast<LONG>(trigger->y + trigger->h / 2.f)};
    historyRequire(ClientToScreen(settings->hwnd, &togglePoint) != FALSE &&
        SetCursorPos(togglePoint.x, togglePoint.y) != FALSE, "Position the trigger toggle input");
    historyRequire(GetAncestor(WindowFromPoint(togglePoint), GA_ROOT) == settings->hwnd,
        "Another window obscures the language trigger");
    INPUT toggleInput[2]{};
    toggleInput[0].type = toggleInput[1].type = INPUT_MOUSE;
    toggleInput[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    toggleInput[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    historyRequire(SendInput(2, toggleInput, sizeof(INPUT)) == 2, "Inject the trigger toggle click");
    pump(100);
    historyRequire(common->languagePopup() == nullptr, "Clicking the open language trigger reopened its popup");
    auto* popup = openSettingsLanguage(settings, pump);
    settingsKey(popup->hwnd, VK_TAB, pump);
    historyRequire(common->languagePopup() == nullptr && Setting::get()->getLang() == originalLanguage,
        "Tab did not cancel language selection");
    std::cout << "PASS language popup dismissal on external activation, owner move, repeated trigger click and Tab.\n";

    popup = openSettingsLanguage(settings, pump);
    settingsKey(popup->hwnd, VK_HOME, pump);
    settingsKey(popup->hwnd, VK_DOWN, pump);
    historyRequire(popup->highlightedValue() == L"en-US" && Setting::get()->getLang() == originalLanguage,
        "English keyboard selection committed before Enter");
    settingsKey(popup->hwnd, VK_RETURN, pump);
    pump(100);
    // A language commit destroys and recreates the owner and Common node.
    settings = WinSetting::get();
    historyRequire(settings && IsWindow(settings->hwnd) && Setting::get()->getLang() == L"en-US",
        "Enter did not recreate settings in English");
    captureHistoryWindow(settings, evidence / L"14-settings-english.png", pump);
    popup = openSettingsLanguage(settings, pump);
    const auto chinese = popup->itemScreenRect(L"zh-CN");
    historyRequire(chinese.right > chinese.left && chinese.bottom > chinese.top, "Chinese language row is not visible");
    POINT chinesePoint{(chinese.left + chinese.right) / 2, (chinese.top + chinese.bottom) / 2};
    historyRequire(ScreenToClient(popup->hwnd, &chinesePoint) != FALSE, "Convert language row click coordinates");
    nativeClick(popup->hwnd, chinesePoint, pump);
    pump(100);
    settings = WinSetting::get();
    historyRequire(settings && IsWindow(settings->hwnd) && Setting::get()->getLang() == originalLanguage,
        "Clicking the Chinese row did not restore the original language");
    std::cout << "PASS language Enter commit and mouse row commit recreate settings safely in English and Chinese.\n";

    const auto work = settingsWorkArea(settings->hwnd);
    const RECT anchor{work.right - 184, work.bottom - 48, work.right - 4, work.bottom - 16};
    std::vector<SettingsSelectPopup::Item> longList;
    for (unsigned index = 0; index < 20; ++index)
        longList.push_back({L"Language " + std::to_wstring(index + 1), L"item-" + std::to_wstring(index)});
    bool completed = false;
    std::optional<std::wstring> chosen;
    auto scrolling = std::make_unique<SettingsSelectPopup>(settings->hwnd, 1.f, anchor,
        std::move(longList), L"item-0", [&](std::optional<std::wstring> value) { completed = true; chosen = std::move(value); });
    settingsKey(scrolling->hwnd, VK_END, pump);
    historyRequire(scrolling->highlightedValue() == L"item-19", "End did not highlight the last scrollable menu item");
    RECT scrollBounds{};
    historyRequire(GetWindowRect(scrolling->hwnd, &scrollBounds) != FALSE, "Read scrollable language menu bounds");
    historyRequire(scrollBounds.left >= work.left && scrollBounds.right <= work.right && scrollBounds.top >= work.top &&
        scrollBounds.bottom <= anchor.top - 4, "Long language menu did not open above its anchor within the work area");
    historyRequire(scrollBounds.bottom - scrollBounds.top <= 276, "Long language menu exceeded its bounded visible height");
    const auto last = scrolling->itemScreenRect(L"item-19");
    historyRequire(last.right > last.left && last.bottom > last.top, "End did not scroll the last item into view");
    historyRequire(!completed, "Navigating a long language list committed selection");
    captureHistoryWindow(scrolling.get(), evidence / L"15-language-menu-scroll.png", pump, false);
    POINT lastPoint{(last.left + last.right) / 2, (last.top + last.bottom) / 2};
    historyRequire(ScreenToClient(scrolling->hwnd, &lastPoint) != FALSE, "Convert scrolled menu item coordinates");
    nativeClick(scrolling->hwnd, lastPoint, pump);
    historyRequire(completed && chosen && *chosen == L"item-19" && !scrolling->isOpen(),
        "Scrolled menu hit-testing committed the wrong item");
    scrolling.reset();
    std::cout << "PASS 20-item language menu: bounded height, above-anchor placement, work-area clamp, End scrolling and row hit-testing.\n";
}

inline void requireHistoryClipboard(HWND owner, const Media::Entry& expected)
{
    if (!expected.video) {
        historyRequire(IsClipboardFormatAvailable(CF_DIB) && IsClipboardFormatAvailable(CF_DIBV5) &&
            IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG")), "History row button did not copy the image");
        return;
    }
    historyRequire(OpenClipboard(owner) != FALSE, "Read history row file clipboard");
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    std::vector<wchar_t> file(drop ? DragQueryFileW(drop, 0, nullptr, 0) + 1 : 1);
    if (drop) DragQueryFileW(drop, 0, file.data(), static_cast<UINT>(file.size()));
    CloseClipboard();
    historyRequire(drop && fs::path(file.data()) == expected.path, "History UI copied the wrong or temporary video path");
}

inline void runClipboardDesignChecks(WinClipboard* history, const fs::path& video,
    const fs::path& evidence, const std::function<void(DWORD)>& pump)
{
    const auto entries = Media::list();
    historyRequire(!entries.empty(), "No archived entries for clipboard UI verification");
    struct ScaleEvidence { UINT dpi; const wchar_t* file; };
    const ScaleEvidence scales[]{
        {96, L"16-clipboard-100.png"}, {120, L"17-clipboard-125.png"}, {144, L"18-clipboard-150.png"}
    };
    for (const auto& scale : scales) {
        const auto bounds = setNativeTestDpi(history, scale.dpi, 760.f, 560.f, pump);
        captureHistoryWindow(history, evidence / scale.file, pump);
        historyRequire(OpenClipboard(history->hwnd) != FALSE, "Clear clipboard before the row hit-test");
        const bool emptied = EmptyClipboard() != FALSE;
        CloseClipboard();
        historyRequire(emptied, "Empty clipboard before testing a real history button");
        const auto sequence = GetClipboardSequenceNumber();
        // The visible first-row Copy button is centered at (width - 172, 170)
        // DIP in the Chinese desktop layout. Exercise HWND hit-testing rather
        // than calling the archive service or a private action handler.
        const float factor = scale.dpi / 96.f;
        nativeClick(history->hwnd, {static_cast<LONG>(std::lround(history->w - 172.f * factor)),
            static_cast<LONG>(std::lround(170.f * factor))}, pump);
        historyRequire(GetClipboardSequenceNumber() != sequence, "Compact Copy button did not receive the click");
        requireHistoryClipboard(history->hwnd, entries.front());
        std::cout << "PASS clipboard controlled WM_DPICHANGED " << scale.dpi << " DPI: requested 760x560 DIP, actual "
            << bounds.right - bounds.left << 'x' << bounds.bottom - bounds.top
            << " px, native compact row Copy hit-testing and clipboard formats.\n";
    }

    const auto found = std::find_if(entries.begin(), entries.end(), [&video](const auto& entry) { return entry.path == video; });
    historyRequire(found != entries.end(), "Video fixture is missing from the visible history");
    settingsKey(history->hwnd, VK_HOME, pump);
    for (auto index = entries.begin(); index != found; ++index) settingsKey(history->hwnd, VK_DOWN, pump);
    historyRequire(OpenClipboard(history->hwnd) != FALSE, "Clear clipboard before the history shortcut");
    const bool emptied = EmptyClipboard() != FALSE;
    CloseClipboard();
    historyRequire(emptied, "Empty clipboard before testing the history shortcut");
    const auto sequence = GetClipboardSequenceNumber();
    std::array<BYTE, 256> original{};
    historyRequire(GetKeyboardState(original.data()) != FALSE, "Read history shortcut keyboard state");
    auto control = original;
    control[VK_CONTROL] |= 0x80;
    historyRequire(SetKeyboardState(control.data()) != FALSE, "Set Ctrl for the history copy shortcut");
    SendMessageW(history->hwnd, WM_KEYDOWN, 'C', 0);
    SendMessageW(history->hwnd, WM_KEYUP, 'C', 0);
    SetKeyboardState(original.data());
    pump(40);
    historyRequire(GetClipboardSequenceNumber() != sequence, "History Ctrl+C did not update the clipboard");
    requireHistoryClipboard(history->hwnd, *found);
    setNativeTestDpi(history, 96, 760.f, 560.f, pump);
    std::cout << "PASS compact history keyboard selection and Ctrl+C retain the archived video path.\n";
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
    runSettingsDesignChecks(settings, evidence, pump);
    settings = WinSetting::get();
    historyRequire(settings && IsWindow(settings->hwnd), "Settings was not restored after language tests");
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
    runClipboardDesignChecks(history, video, evidence, pump);
    SendMessageW(history->hwnd, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(history->hwnd, WM_KEYUP, VK_DOWN, 0);
    SendMessageW(history->hwnd, WM_KEYDOWN, VK_F5, 0);
    SendMessageW(history->hwnd, WM_KEYUP, VK_F5, 0);
    pump(200);
    history->close();
    for (unsigned attempt = 0; WinClipboard::get() && attempt < 100; ++attempt) pump(50);
    historyRequire(!WinClipboard::get(), "History worker/window was not released after close");

    const auto originalLanguage = Setting::get()->getLang();
    Setting::get()->setLang(L"en-US");
    WinClipboard::init();
    for (unsigned attempt = 0; WinClipboard::get() && attempt < 100 &&
        (WinClipboard::get()->isLoading() || !WinClipboard::get()->hasThumbnail(first) ||
            !WinClipboard::get()->hasThumbnail(second) || !WinClipboard::get()->hasThumbnail(video)); ++attempt) pump(50);
    history = WinClipboard::get();
    historyRequire(history && !history->isLoading() && history->hasThumbnail(first) &&
        history->hasThumbnail(second) && history->hasThumbnail(video), "English history thumbnails did not finish loading");
    captureHistoryWindow(history, evidence / L"19-clipboard-english.png", pump);
    history->close();
    for (unsigned attempt = 0; WinClipboard::get() && attempt < 100; ++attempt) pump(50);
    historyRequire(!WinClipboard::get(), "English history did not release its worker and window");
    Setting::get()->setLang(originalLanguage);
    std::cout << "PASS English compact history rendering and close/reopen lifecycle.\n";
    std::cout << "PASS F1 defaults, collision-safe Documents archives, real PNG/MP4 covers, image/file clipboard, settings pages and history lifecycle.\n";
}
} // namespace HuhuCapture::Tests
