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
    const auto scale = window->scale;
    POINT press{static_cast<LONG>(std::lround(before.x * scale)), static_cast<LONG>(std::lround(before.y * scale))};
    activate(window->hwnd);
    POINT screen = press;
    ClientToScreen(window->hwnd, &screen);
    SetCursorPos(screen.x, screen.y);
    pump(20);
    SendMessageW(window->hwnd, WM_MOUSEMOVE, 0, coordinates(press));
    std::array<BYTE, 256> saved{}, pressed{};
    GetKeyboardState(saved.data());
    pressed = saved;
    pressed[VK_LBUTTON] |= 0x80;
    SetKeyboardState(pressed.data());
    SendMessageW(window->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, coordinates(press));
    screen.x += 24;
    screen.y += 16;
    SetCursorPos(screen.x, screen.y);
    POINT move = screen;
    ScreenToClient(window->hwnd, &move);
    SendMessageW(window->hwnd, WM_MOUSEMOVE, MK_LBUTTON, coordinates(move));
    SetKeyboardState(saved.data());
    SendMessageW(window->hwnd, WM_LBUTTONUP, 0, coordinates(move));
    pump();
    const auto after = marker(window, 2)->center();
    require(std::abs(after.x - before.x - 24.0f / scale) <= 2.0f &&
        std::abs(after.y - before.y - 16.0f / scale) <= 2.0f,
        "Reopening the notes rail made the annotation jump while dragging");
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

void clipboardPng(const fs::path& path, UINT sourceWidth, UINT sourceHeight)
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

        imageClick(window, 110, 90);
        require(markers(window).size() == 1, "The first click did not create one annotation");
        replaceNote(window, editor, L"First 中文 note");
        require(marker(window, 1)->note() == L"First 中文 note", "First note was not stored in its shape");
        imageClick(window, 300, 170);
        require(markers(window).size() == 2, "Clicking the image after note editing failed to create a second pin");
        replaceNote(window, editor, L"Second independent note");
        require(marker(window, 1)->note() == L"First 中文 note" && marker(window, 2)->note() == L"Second independent note",
            "Editing the second note overwrote the first annotation");
        const auto firstColor = marker(window, 1)->color(), secondColor = marker(window, 2)->color();
        require(firstColor.r != secondColor.r || firstColor.g != secondColor.g || firstColor.b != secondColor.b,
            "Consecutive annotations do not have distinct colors");
        std::cout << "PASS two real marker placements and independent WM_CHAR note editing.\n";

        selectFirstRow(window, panel);
        require(editor->getText() == L"First 中文 note", "Clicking the first list row selected the wrong note");
        replaceNote(window, editor, L"First edited from list");
        require(marker(window, 1)->note() == L"First edited from list" && marker(window, 2)->note() == L"Second independent note",
            "List selection and shared editor wrote to different annotations");
        screenshot(window, directory / "01-editor-two-notes.png");
        const auto geometry = ScreenCapture::UI::annotationPanelGeometry(panel->h, window->dpi);
        click(window->hwnd, {static_cast<LONG>(panel->x + panel->w - 30 * window->dpi),
            static_cast<LONG>(panel->y + geometry.editorHeadingTop + 16 * window->dpi)});
        require(markers(window).size() == 1 && marker(window, 2)->note() == L"Second independent note",
            "Deleting the selected list entry affected the wrong annotation");
        toolbar(window, L"undo");
        require(markers(window).empty(), "Undo left a stale annotation active");
        toolbar(window, L"redo");
        require(markers(window).size() == 1 && marker(window, 2)->note() == L"Second independent note",
            "Redo failed to restore the matching annotation and note");
        std::cout << "PASS real list selection, selected deletion, toolbar undo and redo.\n";

        window->selectAnnotation(2, true);
        pump();
        imageClick(window, 130, 270);
        require(markers(window).size() == 2 && marker(window, 3)->number() == 3,
            "New annotation reused a deleted identity or failed after focused editing");
        replaceNote(window, editor, L"Third note after history changes");
        dragAfterCollapse(window, panel);
        require(marker(window, 2)->note() == L"Second independent note" &&
            marker(window, 3)->note() == L"Third note after history changes", "Dragging or collapsing changed note ownership");
        screenshot(window, directory / "02-editor-after-drag.png");
        std::cout << "PASS creation after focused editing and edge-position drag after rail collapse.\n";
        toolbar(window, L"clipboard");
        clipboardPng(directory / "03-real-editor-clipboard.png", width, height);
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
