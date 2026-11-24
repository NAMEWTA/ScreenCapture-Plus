#include "pch.h"
#include "app/App.h"
#include "editor/WinPin.h"
#include "editor/History.h"
#include "editor/shapes/ShapeAnnotation.h"
#include "platform/windows/GdiCapture.h"
#include "services/ImageExport.h"
#include "services/Update.h"
#include "ui/AnnotationPanel.h"
#include "ui/AnnotationPanelLayout.h"
#include "ui/toolbars/ToolMain.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
fs::path evidenceDirectory;
void screenshot(WinPin* window, const fs::path& path, bool activateWindow = true);

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void pump(DWORD milliseconds = 80)
{
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            require(message.message != WM_QUIT, "The application quit during GUI verification");
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= end) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
}

struct ApplicationLifetime {
    ~ApplicationLifetime() { App::dispose(); Ling::dispose(); }
};

template<class T>
T* findNode(Ling::Node* node)
{
    if (!node) return nullptr;
    if (auto* result = dynamic_cast<T*>(node)) return result;
    for (const auto& child : node->children)
        if (auto* result = findNode<T>(child.get())) return result;
    return nullptr;
}

Ling::Button* findButton(Ling::Node* node, const std::wstring& id)
{
    if (!node) return nullptr;
    if (auto* button = dynamic_cast<Ling::Button*>(node); button && button->id == id) return button;
    for (const auto& child : node->children)
        if (auto* result = findButton(child.get(), id)) return result;
    return nullptr;
}

std::vector<ShapeAnnotation*> markers(WinPin* window)
{
    std::vector<ShapeAnnotation*> result;
    for (const auto& shape : window->history->shapes())
        if (auto* annotation = dynamic_cast<ShapeAnnotation*>(shape.get())) result.push_back(annotation);
    return result;
}

ShapeAnnotation* marker(WinPin* window, std::uint64_t number)
{
    for (auto* value : markers(window)) if (value->number() == number) return value;
    throw std::runtime_error("The expected annotation is absent from active history");
}

void state(WinPin* window, const char* label)
{
    std::cout << "STATE " << label << ": pin=" << window->hwnd << " toolbar=" << window->toolMain->hwnd
        << " focus=" << GetFocus() << " foreground=" << GetForegroundWindow() << " capture=" << GetCapture()
        << " hasDragged=" << window->hasDragged << " scale=" << window->scale;
    if (auto* panel = findNode<AnnotationPanel>(window->body.get()))
        if (auto* editor = findNode<Ling::TextBox>(panel)) std::cout << " editorFocused=" << editor->isFocused();
    for (auto* note : markers(window))
        std::cout << " [#" << note->number() << " center=" << note->center().x << ',' << note->center().y
            << " bounds=" << note->bounds().left << ',' << note->bounds().top << ','
            << note->bounds().right << ',' << note->bounds().bottom
            << " textLength=" << note->note().size() << ']';
    std::cout << '\n';
}

LPARAM coordinates(POINT point)
{
    return MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
}

void activate(HWND window)
{
    require(IsWindow(window) != FALSE, "The target GUI window does not exist");
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
    pump(20);
}

// Drive the actual HWND procedure. Keep cursor and per-thread button state in
// sync because the editor also reads GetCursorPos/GetKeyState during gestures.
void click(HWND window, POINT point)
{
    activate(window);
    POINT screen = point;
    ClientToScreen(window, &screen);
    require(SetCursorPos(screen.x, screen.y) != FALSE, "Cannot move the cursor on the input desktop");
    pump(20);
    ScreenToClient(window, &screen);
    SendMessageW(window, WM_MOUSEMOVE, 0, coordinates(screen));
    std::array<BYTE, 256> saved{}, pressed{};
    require(GetKeyboardState(saved.data()) != FALSE, "Cannot read keyboard state");
    pressed = saved;
    pressed[VK_LBUTTON] |= 0x80;
    SetKeyboardState(pressed.data());
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, coordinates(screen));
    SetKeyboardState(saved.data());
    if (IsWindow(window)) {
        GetCursorPos(&screen);
        ScreenToClient(window, &screen);
        SendMessageW(window, WM_LBUTTONUP, 0, coordinates(screen));
    }
    pump();
}

void toolbar(WinPin* window, const wchar_t* id)
{
    auto* button = findButton(window->toolMain->body.get(), id);
    require(button != nullptr, "The toolbar command is missing");
    const POINT point{static_cast<LONG>(button->x + button->w / 2), static_cast<LONG>(button->y + button->h / 2)};
    click(window->toolMain->hwnd, point);
}

void imageClick(WinPin* window, float x, float y)
{
    std::cout << "STEP image click at original pixels " << x << ',' << y << '\n';
    click(window->hwnd, {static_cast<LONG>(std::lround(x * window->scale)),
        static_cast<LONG>(std::lround(y * window->scale))});
    state(window, "after image click");
}

void imageDrag(WinPin* window, D2D1_POINT_2F start, D2D1_POINT_2F end)
{
    std::cout << "STEP image drag at original pixels " << start.x << ',' << start.y
        << " -> " << end.x << ',' << end.y << '\n';
    activate(window->hwnd);
    const float scale = window->scale;
    POINT press{static_cast<LONG>(std::lround(start.x * scale)), static_cast<LONG>(std::lround(start.y * scale))};
    POINT screen = press;
    ClientToScreen(window->hwnd, &screen);
    require(SetCursorPos(screen.x, screen.y) != FALSE, "Cannot position the pointer for an image drag");
    pump(20);
    SendMessageW(window->hwnd, WM_MOUSEMOVE, 0, coordinates(press));
    std::array<BYTE, 256> saved{}, pressed{};
    require(GetKeyboardState(saved.data()) != FALSE, "Cannot read keyboard state before image drag");
    pressed = saved;
    pressed[VK_LBUTTON] |= 0x80;
    SetKeyboardState(pressed.data());
    SendMessageW(window->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, coordinates(press));
    const bool captured = GetCapture() == window->hwnd;
    POINT move{static_cast<LONG>(std::lround(end.x * scale)), static_cast<LONG>(std::lround(end.y * scale))};
    screen = move;
    ClientToScreen(window->hwnd, &screen);
    SetCursorPos(screen.x, screen.y);
    SendMessageW(window->hwnd, WM_MOUSEMOVE, MK_LBUTTON, coordinates(move));
    // A real WM_MOUSELEAVE is emitted when crossing the image/window boundary.
    // Its Ling sentinel must not replace the last meaningful drag coordinate.
    SendMessageW(window->hwnd, WM_MOUSELEAVE, 0, 0);
    SetKeyboardState(saved.data());
    SendMessageW(window->hwnd, WM_LBUTTONUP, 0, coordinates(move));
    pump();
    require(captured, "The image drag did not own mouse capture");
    require(GetCapture() != window->hwnd, "The annotation kept mouse capture after release");
    state(window, "after image drag");
}

struct PartialDrag {
    std::size_t activeCount;
    std::uint64_t number;
    D2D1_RECT_F bounds;
};

PartialDrag interruptImageDrag(WinPin* window, D2D1_POINT_2F start,
    D2D1_POINT_2F dragged, D2D1_POINT_2F afterCaptureLoss)
{
    std::cout << "STEP interrupt captured image drag at original pixels " << start.x << ',' << start.y
        << " -> " << dragged.x << ',' << dragged.y << '\n';
    activate(window->hwnd);
    const float scale = window->scale;
    const auto clientPoint = [scale](D2D1_POINT_2F point) {
        return POINT{static_cast<LONG>(std::lround(point.x * scale)),
            static_cast<LONG>(std::lround(point.y * scale))};
    };
    const auto positionPointer = [window](POINT point) {
        ClientToScreen(window->hwnd, &point);
        require(SetCursorPos(point.x, point.y) != FALSE, "Cannot position the pointer for an interrupted drag");
    };
    const POINT press = clientPoint(start);
    positionPointer(press);
    pump(20);
    SendMessageW(window->hwnd, WM_MOUSEMOVE, 0, coordinates(press));
    std::array<BYTE, 256> saved{}, pressed{};
    require(GetKeyboardState(saved.data()) != FALSE, "Cannot read keyboard state before interrupted drag");
    pressed = saved;
    pressed[VK_LBUTTON] |= 0x80;
    SetKeyboardState(pressed.data());
    SendMessageW(window->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, coordinates(press));
    const bool captured = GetCapture() == window->hwnd;
    const POINT move = clientPoint(dragged);
    positionPointer(move);
    SendMessageW(window->hwnd, WM_MOUSEMOVE, MK_LBUTTON, coordinates(move));
    auto* active = dynamic_cast<ShapeAnnotation*>(window->shapeHover);
    const bool valid = active && active->hasValidRegion();
    const PartialDrag partial{markers(window).size(), active ? active->number() : 0,
        active ? active->bounds() : D2D1::RectF(0, 0, 0, 0)};
    // Simulate a lost capture followed by releasing outside the window: there
    // is deliberately no WM_LBUTTONUP for WinPin to receive.
    ReleaseCapture();
    saved[VK_LBUTTON] &= 0x7F;
    SetKeyboardState(saved.data());
    const POINT resume = clientPoint(afterCaptureLoss);
    positionPointer(resume);
    SendMessageW(window->hwnd, WM_MOUSEMOVE, 0, coordinates(resume));
    pump();
    require(captured && valid, "The interruption fixture never reached a valid captured region drag");
    require(GetCapture() != window->hwnd, "The interrupted annotation reclaimed mouse capture");
    state(window, "after interrupted image drag");
    return partial;
}

bool nearlyEqual(float left, float right, float tolerance = 2.0f)
{
    return std::abs(left - right) <= tolerance;
}

bool sameBounds(D2D1_RECT_F actual, D2D1_RECT_F expected)
{
    return nearlyEqual(actual.left, expected.left) && nearlyEqual(actual.top, expected.top) &&
        nearlyEqual(actual.right, expected.right) && nearlyEqual(actual.bottom, expected.bottom);
}

void regionGeometry(ShapeAnnotation* note, D2D1_RECT_F expected, float creationScale, float dpi)
{
    require(note->hasValidRegion(), "The dragged annotation has no valid region");
    require(sameBounds(note->bounds(), expected), "The annotation region does not match the normalized drag bounds");
    const auto bounds = note->bounds();
    const auto center = note->center();
    const float radius = note->badgeRadius();
    const float diameterDip = radius * 2.0f * creationScale / dpi;
    require(diameterDip >= 20.0f && diameterDip <= 30.0f,
        "The single-digit annotation badge is not the smaller readable size");
    require(center.x >= bounds.left + radius - 1.0f && center.y >= bounds.top + radius - 1.0f &&
        center.x <= bounds.left + radius + 4.0f * dpi / creationScale &&
        center.y <= bounds.top + radius + 4.0f * dpi / creationScale,
        "The annotation badge is not anchored inside the rectangle's top-left corner");
}

void resizeAndClamp(WinPin* window)
{
    auto* second = marker(window, 2);
    const auto beforeResize = second->bounds();
    imageDrag(window, {beforeResize.right, beforeResize.bottom},
        {beforeResize.right + 20.0f, beforeResize.bottom + 12.0f});
    require(sameBounds(second->bounds(), {beforeResize.left, beforeResize.top,
        beforeResize.right + 20.0f, beforeResize.bottom + 12.0f}),
        "Dragging the bottom-right handle failed to resize the annotation");
    imageDrag(window, {second->bounds().right, second->bounds().bottom},
        {beforeResize.right, beforeResize.bottom});
    require(sameBounds(second->bounds(), beforeResize), "The annotation resize could not be restored");

    auto* third = marker(window, 3);
    const auto initial = third->bounds();
    const auto center = third->center();
    const float width = initial.right - initial.left, height = initial.bottom - initial.top;
    imageDrag(window, center, {-30.0f, -30.0f});
    require(sameBounds(third->bounds(), {0.0f, 0.0f, width, height}),
        "Moving a region beyond the top-left image edge changed its size or lost the region");
    const auto upperLeftCenter = third->center();
    imageDrag(window, upperLeftCenter,
        {upperLeftCenter.x + 520.0f, upperLeftCenter.y + 400.0f});
    const auto imageSize = window->screenImg->GetPixelSize();
    require(sameBounds(third->bounds(), {static_cast<float>(imageSize.width) - width,
        static_cast<float>(imageSize.height) - height, static_cast<float>(imageSize.width),
        static_cast<float>(imageSize.height)}),
        "Moving a region beyond the bottom-right image edge changed its size or lost the region");
    const auto bottomRightCenter = third->center();
    imageDrag(window, bottomRightCenter, {bottomRightCenter.x + initial.left - third->bounds().left,
        bottomRightCenter.y + initial.top - third->bounds().top});
    require(sameBounds(third->bounds(), initial), "Moving a clamped region back changed its original geometry");
    std::cout << "PASS region resizing and size-preserving moves clamped at all image edges.\n";
}

void interruptedGestures(WinPin* window)
{
    const auto count = markers(window).size();
    const auto secondBefore = marker(window, 2)->bounds();
    auto* third = marker(window, 3);
    const auto thirdBefore = third->bounds();
    const auto noteBefore = third->note();
    const auto cancelled = interruptImageDrag(window, {245.0f, 35.0f}, {330.0f, 80.0f}, {340.0f, 90.0f});
    require(cancelled.activeCount == count + 1 && cancelled.number > 3,
        "The interrupted new-region gesture did not create its own temporary object");
    require(markers(window).size() == count && sameBounds(marker(window, 2)->bounds(), secondBefore) &&
        sameBounds(third->bounds(), thirdBefore),
        "Capture loss kept an unfinished region or changed an existing annotation");

    const auto center = third->center();
    const auto stopped = interruptImageDrag(window, center,
        {center.x + 12.0f, center.y + 8.0f}, {center.x + 42.0f, center.y + 28.0f});
    require(stopped.activeCount == count && stopped.number == 3 &&
        sameBounds(stopped.bounds, {thirdBefore.left + 12.0f, thirdBefore.top + 8.0f,
            thirdBefore.right + 12.0f, thirdBefore.bottom + 8.0f}),
        "The interrupted move did not target the existing annotation geometry");
    require(sameBounds(third->bounds(), stopped.bounds) && third->note() == noteBefore,
        "The existing region continued following the unpressed pointer after capture loss");
    const auto stoppedCenter = third->center();
    imageDrag(window, stoppedCenter, {stoppedCenter.x - 12.0f, stoppedCenter.y - 8.0f});
    require(sameBounds(third->bounds(), thirdBefore) && third->note() == noteBefore,
        "Normal dragging did not recover after the interrupted gesture");
    std::cout << "PASS capture loss discards unfinished regions, stops existing moves and permits the next drag.\n";
}

void emptyGestures(WinPin* window)
{
    const auto before = markers(window).size();
    imageClick(window, 245.0f, 35.0f);
    require(markers(window).size() == before, "A click without dragging left an empty annotation");
    imageDrag(window, {245.0f, 35.0f}, {245.0f, 80.0f});
    require(markers(window).size() == before, "A zero-width drag left an empty annotation");
    imageDrag(window, {245.0f, 35.0f}, {310.0f, 35.0f});
    require(markers(window).size() == before, "A zero-height drag left an empty annotation");
    imageDrag(window, {245.0f, 35.0f}, {246.0f, 36.0f});
    require(markers(window).size() == before, "A one-pixel pointer jitter left an annotation");
    std::cout << "PASS clicks, zero-area drags and one-pixel jitter leave no annotation in active history.\n";
}

void replaceNote(WinPin* window, Ling::TextBox* editor, const std::wstring& value)
{
    if (!editor->isFocused()) {
        state(window, "note editor focus failure");
        try { screenshot(window, evidenceDirectory / "failure-editor-focus.png", false); }
        catch (const std::exception& error) { std::cerr << "Failure screenshot: " << error.what() << '\n'; }
        throw std::runtime_error("The shared note editor did not receive focus");
    }
    editor->selectAll();
    for (const wchar_t character : value) SendMessageW(window->hwnd, WM_CHAR, character, 1);
    pump();
    require(editor->getText() == value, "The real text editor did not accept WM_CHAR input");
}

void selectFirstRow(WinPin* window, AnnotationPanel* panel)
{
    POINT point{static_cast<LONG>(panel->x + 60 * window->dpi),
        static_cast<LONG>(panel->y + 78 * window->dpi)};
    POINT screen = point;
    ClientToScreen(window->hwnd, &screen);
    SetCursorPos(screen.x, screen.y);
    SendMessageW(window->hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA * 10), coordinates(screen));
    pump();
    click(window->hwnd, point);
}

void dragAfterCollapse(WinPin* window, AnnotationPanel* panel)
{
    // With no drawing tool selected, dragging intentionally moves the window.
    // Keep another tool selected so hitting an existing annotation edits it.
    toolbar(window, L"rect");
    click(window->hwnd, {static_cast<LONG>(panel->x + panel->w - 30 * window->dpi),
        static_cast<LONG>(panel->y + 24 * window->dpi)});
    MONITORINFO monitor{sizeof(MONITORINFO)};
    require(GetMonitorInfoW(MonitorFromWindow(window->hwnd, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
        "Cannot query the test monitor");
    // Reopening the rail at this edge requires moving/fitting the window. The
    // pointer drag must stay in its original image coordinates until release.
    window->setPosition(monitor.rcWork.right - static_cast<int>(window->w) - 4, window->y);
    pump();
    const auto before = marker(window, 2)->center();
    const auto bounds = marker(window, 2)->bounds();
    const auto scale = window->scale;
    imageDrag(window, before, {before.x + 24.0f / scale, before.y + 16.0f / scale});
    const auto after = marker(window, 2)->center();
    require(nearlyEqual(after.x - before.x, 24.0f / scale) && nearlyEqual(after.y - before.y, 16.0f / scale),
        "Reopening the notes rail made the annotation jump while dragging");
    const auto moved = marker(window, 2)->bounds();
    require(nearlyEqual(moved.right - moved.left, bounds.right - bounds.left) &&
        nearlyEqual(moved.bottom - moved.top, bounds.bottom - bounds.top) &&
        nearlyEqual(moved.left - bounds.left, after.x - before.x) && nearlyEqual(moved.top - bounds.top, after.y - before.y),
        "Dragging the annotation badge did not move its complete rectangle");
}

void screenshot(WinPin* window, const fs::path& path, bool activateWindow)
{
    if (activateWindow) activate(window->hwnd);
    window->refresh();
    window->toolMain->refresh();
    pump(250);
    DwmFlush();
    RECT bounds{}, toolbarBounds{};
    GetWindowRect(window->hwnd, &bounds);
    GetWindowRect(window->toolMain->hwnd, &toolbarBounds);
    UnionRect(&bounds, &bounds, &toolbarBounds);
    ScreenCapture::Platform::GdiCapture capture;
    require(capture.capture(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top),
        "The desktop could not be captured for GUI evidence");
    std::vector<BYTE> pixels(capture.pixels().begin(), capture.pixels().end());
    require(ImageExport::saveToFile(path.wstring(), capture.width(), capture.height(), pixels.data()),
        "The GUI screenshot could not be written");
}

struct ExportRegion {
    std::uint64_t number;
    D2D1_RECT_F bounds;
    D2D1_POINT_2F badgeCenter;
    float badgeRadius;
    std::uint32_t rgb;
};

std::vector<ExportRegion> exportRegions(WinPin* window)
{
    std::vector<ExportRegion> result;
    for (auto* note : markers(window))
        result.push_back({note->number(), note->bounds(), note->center(), note->badgeRadius(),
            ScreenCapture::Core::annotationColor(note->number()).backgroundRgb});
    return result;
}

std::size_t colorPixels(const std::vector<BYTE>& pixels, UINT width, UINT height,
    D2D1_RECT_F area, std::uint32_t rgb)
{
    const auto left = (std::max)(0, static_cast<int>(std::floor(area.left)));
    const auto top = (std::max)(0, static_cast<int>(std::floor(area.top)));
    const auto right = (std::min)(static_cast<int>(width), static_cast<int>(std::ceil(area.right)));
    const auto bottom = (std::min)(static_cast<int>(height), static_cast<int>(std::ceil(area.bottom)));
    const int red = (rgb >> 16) & 0xFF, green = (rgb >> 8) & 0xFF, blue = rgb & 0xFF;
    std::size_t matches{};
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
        if (std::abs(static_cast<int>(pixels[offset]) - blue) <= 3 &&
            std::abs(static_cast<int>(pixels[offset + 1]) - green) <= 3 &&
            std::abs(static_cast<int>(pixels[offset + 2]) - red) <= 3 && pixels[offset + 3] == 255) ++matches;
    }
    return matches;
}

void clipboardPng(const fs::path& path, UINT sourceWidth, UINT sourceHeight,
    const std::vector<BYTE>& sourcePixels, const std::vector<ExportRegion>& regions)
{
    const auto format = RegisterClipboardFormatW(L"PNG");
    require(OpenClipboard(nullptr) != FALSE, "Cannot inspect the exported clipboard");
    const auto memory = static_cast<HGLOBAL>(GetClipboardData(format));
    if (!memory) { CloseClipboard(); throw std::runtime_error("The copy command did not publish a PNG"); }
    const auto size = GlobalSize(memory);
    const auto* bytes = static_cast<const char*>(GlobalLock(memory));
    if (!bytes || size == 0) { CloseClipboard(); throw std::runtime_error("The clipboard PNG is empty"); }
    std::ofstream output(path, std::ios::binary);
    output.write(bytes, static_cast<std::streamsize>(size));
    GlobalUnlock(memory);
    CloseClipboard();
    output.close();
    require(output.good(), "Cannot retain the clipboard PNG");
    ComPtr<IWICImagingFactory> factory;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()))), "Cannot decode the clipboard PNG");
    ComPtr<IWICBitmapDecoder> decoder;
    require(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())), "The actual clipboard PNG is not decodable");
    ComPtr<IWICBitmapFrameDecode> frame;
    require(SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())), "The clipboard PNG has no image frame");
    UINT width{}, height{};
    require(SUCCEEDED(frame->GetSize(&width, &height)) && width > sourceWidth && height >= sourceHeight,
        "The real copy command omitted the notes rail or clipped the captured image");
    require(width <= 4096 && height <= 4096, "The small GUI fixture exported an unexpectedly large bitmap");
    ComPtr<IWICFormatConverter> converter;
    require(SUCCEEDED(factory->CreateFormatConverter(converter.GetAddressOf())) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)),
        "Cannot read back the production clipboard's BGRA pixels");
    std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
    require(pixels.size() <= UINT_MAX && SUCCEEDED(converter->CopyPixels(nullptr, width * 4,
        static_cast<UINT>(pixels.size()), pixels.data())), "The production PNG pixels could not be read");
    for (const auto& region : regions) {
        const auto bounds = region.bounds;
        const float middle = bounds.left + (bounds.right - bounds.left) * 0.65f;
        require(colorPixels(pixels, width, height,
            {middle - 12.0f, bounds.top - 3.0f, middle + 12.0f, bounds.top + 3.0f}, region.rgb) >= 12,
            "The exported annotation rectangle is missing or uses the wrong automatic color");
        require(colorPixels(pixels, width, height,
            {region.badgeCenter.x - region.badgeRadius, region.badgeCenter.y - region.badgeRadius,
             region.badgeCenter.x + region.badgeRadius, region.badgeCenter.y + region.badgeRadius}, region.rgb) >= 30,
            "The exported top-left number badge does not match its rectangle color");
        require(colorPixels(pixels, width, height,
            {static_cast<float>(sourceWidth), 0.0f, static_cast<float>(width), static_cast<float>(height)}, region.rgb) >= 30,
            "The exported note rail lost the matching numbered color badge");
        // The fixture regions do not overlap at their centers. A transparent
        // region must preserve the source there; a filled rectangle would fail.
        const UINT x = static_cast<UINT>((bounds.left + bounds.right) * 0.5f);
        const UINT y = static_cast<UINT>((bounds.top + bounds.bottom) * 0.5f);
        const auto sourceOffset = (static_cast<std::size_t>(y) * sourceWidth + x) * 4;
        const auto outputOffset = (static_cast<std::size_t>(y) * width + x) * 4;
        require(std::equal(sourcePixels.begin() + sourceOffset, sourcePixels.begin() + sourceOffset + 4,
            pixels.begin() + outputOffset), "The annotation region unexpectedly painted over its interior");
        std::cout << "PASS exported region #" << region.number << ": colored outline, small top-left badge, "
            "matching note badge and unmodified interior.\n";
    }
    std::cout << "PASS production clipboard PNG: " << width << 'x' << height << '\n';
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::cout.setf(std::ios::unitbuf);
    try {
        require(argc >= 2, "Usage: GuiIntegrationTests.exe <image-directory> --enter=tray");
        const fs::path directory(argv[1]);
        evidenceDirectory = directory;
        fs::create_directories(directory);
        std::cout << "Desktop capability: " << GetSystemMetrics(SM_CXVIRTUALSCREEN) << 'x'
            << GetSystemMetrics(SM_CYVIRTUALSCREEN) << ", DPI " << GetDpiForSystem() << '\n';
        ComPtr<ID3D11Device> probe;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1};
        const auto hardware = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION, probe.GetAddressOf(), nullptr, nullptr);
        if (FAILED(hardware)) {
            std::cerr << "UNAVAILABLE: the pinned Ling desktop requires D3D11 hardware; HRESULT 0x"
                << std::hex << static_cast<unsigned long>(hardware) << ". WARP renderer evidence is separate.\n";
            return 77;
        }
        probe.Reset();
        const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
        if (!desktop) {
            std::cerr << "UNAVAILABLE: no accessible Windows input desktop; Win32 error " << GetLastError() << '\n';
            return 77;
        }
        CloseDesktop(desktop);
        std::cout << "Starting the production application and native editor window.\n";
        ApplicationLifetime lifetime;
        App::init();
        Update::dispose(); // This isolated GUI check never performs a background update.
        constexpr UINT width = 480, height = 360;
        std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[offset] = static_cast<BYTE>(220 + (x / 24) % 20);
            pixels[offset + 1] = static_cast<BYTE>(220 + (y / 24) % 20);
            pixels[offset + 2] = 245;
            pixels[offset + 3] = 255;
        }
        WinPin::initFromData(60, 60, width, height, pixels);
        pump(250);
        WinPin* window = nullptr;
        for (auto* candidate : Ling::App::get()->windows)
            if (auto* pin = dynamic_cast<WinPin*>(candidate)) { window = pin; break; }
        require(window && IsWindow(window->hwnd), "The production WinPin window was not created");
        toolbar(window, L"annotation");
        require(window->toolMain->curId == L"annotation", "The real toolbar did not select the annotation tool");
        auto* panel = findNode<AnnotationPanel>(window->body.get());
        auto* editor = panel ? findNode<Ling::TextBox>(panel) : nullptr;
        require(panel && editor, "The real shared annotation editor is missing");
        window->onMouseMove.add([window](POINT point) {
            if (point.x == INT_MAX || point.y == INT_MAX || GetCapture() == window->hwnd)
                std::cout << "TRACE move " << point.x << ',' << point.y << " capture=" << GetCapture()
                    << " hasDragged=" << window->hasDragged << " leftDown=" << ((GetKeyState(VK_LBUTTON) & 0x8000) != 0) << '\n';
        });
        window->onBlur.add([]() {
            std::cout << "TRACE host blur focus=" << GetFocus() << " foreground=" << GetForegroundWindow() << '\n';
        });
        editor->onFocusChanged.add([](Ling::TextBox*, bool focused) {
            std::cout << "TRACE editor focused=" << focused << " focus=" << GetFocus()
                << " foreground=" << GetForegroundWindow() << '\n';
        });

        const float creationScale = window->scale;
        imageDrag(window, {46.0f, 42.0f}, {210.0f, 145.0f});
        require(markers(window).size() == 1, "The first drag did not create one annotation region");
        regionGeometry(marker(window, 1), {46.0f, 42.0f, 210.0f, 145.0f}, creationScale, window->dpi);
        replaceNote(window, editor, L"First 中文 note");
        require(marker(window, 1)->note() == L"First 中文 note", "First note was not stored in its shape");
        const std::wstring secondNote = L"检查按钮状态，矩形边框与编号颜色一致。";
        imageDrag(window, {424.0f, 232.0f}, {258.0f, 110.0f});
        require(markers(window).size() == 2, "Dragging after note editing failed to create a second region");
        regionGeometry(marker(window, 2), {258.0f, 110.0f, 424.0f, 232.0f}, creationScale, window->dpi);
        replaceNote(window, editor, secondNote);
        require(marker(window, 1)->note() == L"First 中文 note" && marker(window, 2)->note() == secondNote,
            "Editing the second note overwrote the first annotation");
        const auto firstColor = marker(window, 1)->color(), secondColor = marker(window, 2)->color();
        require(firstColor.r != secondColor.r || firstColor.g != secondColor.g || firstColor.b != secondColor.b,
            "Consecutive annotations do not have distinct colors");
        std::cout << "PASS forward/reverse region drags, small top-left badges and independent WM_CHAR notes.\n";

        selectFirstRow(window, panel);
        require(editor->getText() == L"First 中文 note", "Clicking the first list row selected the wrong note");
        replaceNote(window, editor, L"First edited from list");
        require(marker(window, 1)->note() == L"First edited from list" && marker(window, 2)->note() == secondNote,
            "List selection and shared editor wrote to different annotations");
        screenshot(window, directory / "01-editor-two-notes.png");
        const auto geometry = ScreenCapture::UI::annotationPanelGeometry(panel->h, window->dpi);
        click(window->hwnd, {static_cast<LONG>(panel->x + panel->w - 30 * window->dpi),
            static_cast<LONG>(panel->y + geometry.editorHeadingTop + 16 * window->dpi)});
        require(markers(window).size() == 1 && marker(window, 2)->note() == secondNote,
            "Deleting the selected list entry affected the wrong annotation");
        toolbar(window, L"undo");
        require(markers(window).empty(), "Undo left a stale annotation active");
        toolbar(window, L"redo");
        require(markers(window).size() == 1 && marker(window, 2)->note() == secondNote,
            "Redo failed to restore the matching annotation and note");
        regionGeometry(marker(window, 2), {258.0f, 110.0f, 424.0f, 232.0f}, creationScale, window->dpi);
        require(nearlyEqual(marker(window, 2)->color().r, secondColor.r, 0.001f) &&
            nearlyEqual(marker(window, 2)->color().g, secondColor.g, 0.001f) &&
            nearlyEqual(marker(window, 2)->color().b, secondColor.b, 0.001f),
            "History restoration changed the annotation's automatic color");
        std::cout << "PASS real list selection, selected deletion, toolbar undo and redo preserve region, color and note.\n";

        window->selectAnnotation(2, true);
        pump();
        imageDrag(window, {46.0f, 252.0f}, {225.0f, 330.0f});
        require(markers(window).size() == 2 && marker(window, 3)->number() == 3,
            "New annotation reused a deleted identity or failed after focused editing");
        const std::wstring thirdNote = L"调整这块内容，拖动后仍然对应同一条批注。";
        replaceNote(window, editor, thirdNote);
        resizeAndClamp(window);
        interruptedGestures(window);
        emptyGestures(window);
        dragAfterCollapse(window, panel);
        require(marker(window, 2)->note() == secondNote && marker(window, 3)->note() == thirdNote,
            "Resizing, dragging or collapsing changed note ownership");
        require(markers(window).size() == 2, "Invalid gestures left an orphan annotation in active history");
        screenshot(window, directory / "02-editor-after-drag.png");
        std::cout << "PASS creation after focused editing and edge-position drag after rail collapse.\n";
        // Copy closes the real editor; retain only value snapshots beforehand.
        const auto regions = exportRegions(window);
        toolbar(window, L"clipboard");
        clipboardPng(directory / "03-real-editor-clipboard.png", width, height, pixels, regions);
        std::cout << "Real native annotation GUI integration checks passed.\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "FAIL WinRT: 0x" << std::hex << static_cast<unsigned long>(error.code().value)
            << ' ' << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
