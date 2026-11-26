#include "ui/qt/CaptureWindow.h"
#include "platform/WindowActivation.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#ifdef Q_OS_WIN
#include "platform/windows/WindowFocus.h"
#elif defined(Q_OS_LINUX)
#include <xcb/xcb.h>
#endif

using namespace HuhuCapture;
namespace ui = HuhuCapture::UI::Qt;
namespace {
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
void events()
{ QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
void waitFor(const std::function<bool()>& predicate, const char* message)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < 8000) { events(); QThread::msleep(2); }
    require(predicate(), message);
}
bool nativeFocus(QWindow* window)
{
    if (!window || !window->isVisible() || !window->handle() ||
        QGuiApplication::focusWindow() != window || !window->isActive()) return false;
#ifdef Q_OS_WIN
    return Platform::Windows::hasForegroundFocus(reinterpret_cast<HWND>(window->winId()));
#elif defined(Q_OS_LINUX)
    // Corroborate Qt's state against the server using an independent XCB
    // connection, rather than treating the focus notification as acceptance.
    auto* connection = xcb_connect(nullptr, nullptr);
    if (!connection || xcb_connection_has_error(connection)) {
        if (connection) xcb_disconnect(connection);
        return false;
    }
    auto* reply = xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
    const bool focused = reply && reply->focus == static_cast<xcb_window_t>(window->winId());
    std::free(reply); xcb_disconnect(connection);
    return focused;
#else
    return true;
#endif
}
Capture::Snapshot source(quint64 generation = 1)
{
    // These immutable source pixels exercise the actual product presentation;
    // this test does not claim desktop capture or physical-display acceptance.
    Capture::Snapshot snapshot; snapshot.sourceId = 57; snapshot.generation = generation;
    snapshot.image = QImage(320, 200, QImage::Format_ARGB32);
    snapshot.image.fill(QColor(39, 118, 203)); snapshot.cursor = {120, 100};
    return snapshot;
}
class InputWindow final : public QWidget {
public:
    int paints{}, spontaneousPresses{}, spontaneousReleases{};
    InputWindow()
    { setWindowTitle(QStringLiteral("huhucapture owned activation input")); resize(180, 90); }
protected:
    void paintEvent(QPaintEvent*) override
    { QPainter painter(this); painter.fillRect(rect(), QColor(39, 118, 203)); ++paints; }
    void mousePressEvent(QMouseEvent* event) override
    { if (event->button() == Qt::LeftButton && event->spontaneous()) ++spontaneousPresses; }
    void mouseReleaseEvent(QMouseEvent* event) override
    { if (event->button() == Qt::LeftButton && event->spontaneous()) ++spontaneousReleases; }
};
#ifdef Q_OS_WIN
struct CursorRestore {
    POINT original{}; bool available{GetCursorPos(&original) != FALSE};
    ~CursorRestore() { if (available) SetCursorPos(original.x, original.y); }
};
void clickOwnedWindow(InputWindow& window)
{
    const auto handle = reinterpret_cast<HWND>(window.windowHandle()->winId());
    RECT client{}; require(GetClientRect(handle, &client) != FALSE, "Read the owned native client rectangle");
    POINT point{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
    require(ClientToScreen(handle, &point) != FALSE && SetCursorPos(point.x, point.y) != FALSE,
        "Position the native cursor within the owned input fixture");
    require(GetAncestor(WindowFromPoint(point), GA_ROOT) == handle,
        "The actual input point belongs to this fixture without a competing window");
    require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0,
        "Do not interfere with an already held mouse button");
    std::array<INPUT, 2> input{};
    input[0].type = input[1].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const auto inserted = SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
    if (inserted == 1) SendInput(1, &input[1], sizeof(INPUT));
    require(inserted == input.size(), "Windows accepts the complete owned input click");
    waitFor([&] { return window.spontaneousPresses == 1 && window.spontaneousReleases == 1 &&
        nativeFocus(window.windowHandle()); }, "The real click establishes native foreground ownership");
}
#endif
class HideOnShow final : public QObject {
public:
    bool observed{};
    HideOnShow() { qApp->installEventFilter(this); }
    ~HideOnShow() override { qApp->removeEventFilter(this); }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!observed && widget && widget->objectName() == QStringLiteral("captureSurface") &&
            event->type() == QEvent::Show) {
            observed = true; widget->hide();
        }
        return false;
    }
};
void lifecycleContracts()
{
    require(Platform::applyActivationToken(nullptr, {}) == Platform::ActivationTokenResult::NotNeeded,
        "An absent activation token creates no native window");
    QWindow untouched;
    require(Platform::applyActivationToken(&untouched, QStringLiteral("non-wayland-token")) ==
        Platform::ActivationTokenResult::NotNeeded && !untouched.handle(),
        "Non-Wayland plugins do not create a native surface or claim token attachment");
    {
        HideOnShow hiding;
        ui::CaptureWindow capture;
        const auto result = capture.showSnapshot(source());
        require(hiding.observed && result.code == Capture::Failure::Code::Cancelled && !capture.visible(),
            "A capture hidden by a synchronous show event cannot report successful presentation");
        capture.shutdown();
    }
    auto capture = std::make_unique<ui::CaptureWindow>();
    require(!capture->showSnapshot(source()), "Show the actual capture presentation");
    QPointer<QWidget> surface(capture->dialogParent());
    require(surface && surface->isVisible(), "The product owns a visible capture surface");
    capture.reset();
    events();
    require(!surface, "Destroying the capture owner destroys its native surface without deferred revival");
    std::cout << "PASS actual product show cancellation and owner destruction; no native-focus claim\n";
}
void nativeActivation()
{
    const auto qpa = QGuiApplication::platformName();
    require(qpa == QStringLiteral("windows") || qpa == QStringLiteral("xcb"),
        "This native focus test requires an actual Windows or X11 desktop");
    InputWindow input;
    input.setWindowFlag(Qt::WindowStaysOnTopHint);
    if (auto* screen = QGuiApplication::primaryScreen())
        input.move(screen->availableGeometry().topLeft() + QPoint(24, 24));
    input.show();
    waitFor([&] { return input.windowHandle() && input.windowHandle()->isExposed() && input.paints > 0; },
        "The owned input window is exposed and has painted");
#ifdef Q_OS_WIN
    CursorRestore cursor;
    clickOwnedWindow(input);
#else
    input.activateWindow(); input.setFocus(Qt::ActiveWindowFocusReason);
    waitFor([&] { return nativeFocus(input.windowHandle()); }, "The owned X11 input window has real focus");
#endif
    auto capture = std::make_unique<ui::CaptureWindow>();
    require(!capture->showSnapshot(source()), "The actual product capture presentation accepts its source");
    QPointer<QWidget> surface(capture->dialogParent());
    waitFor([&] { return surface && surface->windowHandle() && surface->windowHandle()->isExposed() &&
        nativeFocus(surface->windowHandle()); },
        "The product activates its capture window with actual native foreground focus");
    capture->cancel();
    waitFor([&] { return !surface || !surface->isVisible(); }, "Cancelling hides the actual active capture surface");
    require(!capture->visible(), "A cancelled capture cannot retain visible native input surfaces");
    require(!capture->showSnapshot(source(2)), "A later capture reuses the same production presentation owner");
    surface = capture->dialogParent();
    waitFor([&] { return surface && nativeFocus(surface->windowHandle()); },
        "The reused production capture surface acquires actual foreground focus");
    capture.reset();
    events();
    require(!surface, "Destruction removes the focused native capture window");
    std::cout << "PASS actual product native focus, cancellation, repeated activation and owner destruction\n";
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        if (app.arguments().contains(QStringLiteral("--native"))) nativeActivation();
        else {
            require(QGuiApplication::platformName() == QStringLiteral("offscreen"),
                "The unavailable-platform case must run on the explicit offscreen plugin");
            lifecycleContracts();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << " qpa=" << QGuiApplication::platformName().toStdString() << '\n';
        for (auto* window : QApplication::topLevelWidgets())
            std::cerr << "owned-window name=" << window->objectName().toStdString()
                << " visible=" << window->isVisible() << " active=" << window->isActiveWindow()
                << " native-focus=" << nativeFocus(window->windowHandle()) << '\n';
        return 1;
    }
}
