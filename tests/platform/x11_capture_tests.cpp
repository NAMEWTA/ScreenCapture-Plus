#include "app/MediaWorkspace.h"
#include "app/ScrollController.h"
#include "platform/linux/X11Capture.h"
#include "platform/NativeScrollSource.h"
#include "platform/DesktopPointer.h"
#include "services/capture/DesktopCapture.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"
#include "ui/qt/EditorWindow.h"

#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace capture = HuhuCapture::Capture;
namespace platform = HuhuCapture::Platform;
namespace desktop = HuhuCapture::Desktop;
namespace app = HuhuCapture::Application;
namespace ui = HuhuCapture::UI::Qt;
using namespace std::chrono_literals;
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void wait(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!predicate()) { require(std::chrono::steady_clock::now() < deadline, message); settle(); std::this_thread::sleep_for(1ms); }
}
template<class Work> auto worker(Work work)
{
    auto future = std::async(std::launch::async, std::move(work));
    wait([&] { return future.wait_for(0ms) == std::future_status::ready; }, "The real X11 worker did not settle");
    return future.get();
}
bool equalPixels(const QImage& image, const QImage& expected)
{
    if (image.size() != expected.size() || image.format() != QImage::Format_ARGB32) return false;
    for (int y = 0; y < image.height(); ++y)
        if (std::memcmp(image.constScanLine(y), expected.constScanLine(y), static_cast<std::size_t>(image.width()) * 4)) return false;
    return true;
}
class Pattern final : public QWidget {
public:
    QImage page{320, 1800, QImage::Format_ARGB32};
    std::atomic<int> displayedOffset{-1};
    int offset{};
    Pattern()
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setCursor(Qt::ArrowCursor); resize(320, 300); move(60, 70);
        for (int y = 0; y < page.height(); ++y) for (int x = 0; x < page.width(); ++x)
            page.setPixel(x, y, qRgb((x * 17 + y * 29 + (y >> 4) * 43) % 251,
                (x * 7 + y * 13 + (y >> 3) * 37) % 253, (x * 3 + y * 19 + (y >> 2) * 11) % 255));
    }
    void reset() { displayedOffset = -1; offset = 0; update(); wait([&] { return displayedOffset.load() == 0; }, "Paint the initial real X11 surface"); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.drawImage(rect(), page, QRect(0, offset, width(), height()));
        const int current = offset;
        QTimer::singleShot(0, this, [this, current] { QGuiApplication::sync(); displayedOffset.store(current); });
    }
    void wheelEvent(QWheelEvent* event) override
    {
        offset = std::clamp(offset + (event->angleDelta().y() < 0 ? 40 : -40), 0, page.height() - height());
        update(); event->accept();
    }
};
void nativePixels(Pattern& pattern)
{
    pattern.reset();
    const auto rectangle = QRect(pattern.mapToGlobal(QPoint()), pattern.size());
    const auto nativeWindow = static_cast<std::uintptr_t>(pattern.winId());
    const auto pointer = rectangle.topLeft() + QPoint(101, 99);
    require(platform::requestDesktopPointerMove(pointer), "Submit exact physical X11 pointer coordinates on the GUI connection");
    QGuiApplication::sync(); // Order the GUI request before querying on a different XCB connection.
    const auto page = pattern.page;
    worker([&pattern, rectangle, nativeWindow, pointer, page] {
        platform::Linux::X11Capture source;
        platform::Linux::X11Desktop layout;
        require(source.desktop(layout), source.error());
        require(!layout.monitors.empty(), "Read actual RandR monitor bounds");
        require(QPoint(layout.cursorX, layout.cursorY) == pointer, "Native pointer motion preserves exact physical pixel coordinates");
        platform::Linux::X11Rectangle actualWindow;
        require(source.windowRectangle(nativeWindow, actualWindow) &&
            QRect(actualWindow.x, actualWindow.y, actualWindow.width, actualWindow.height) == rectangle,
            "Control-window exclusion uses actual root-relative native geometry");
        require(!source.windowRectangle(0, actualWindow) && !source.error().empty(), "Invalid control XIDs fail instead of guessing a desktop rectangle");
        require(std::any_of(layout.windows.begin(), layout.windows.end(), [&](const auto& window) {
            return QRect(window.x, window.y, window.width, window.height) == rectangle;
        }), "Actual visible window bounds are available for screenshot snapping");
        auto capture = [&] {
            require(source.capture(rectangle.x(), rectangle.y(), rectangle.width(), rectangle.height(), false), source.error());
            return QImage(source.pixels().data(), source.width(), source.height(), static_cast<qsizetype>(source.stride()), QImage::Format_ARGB32).copy();
        };
        require(equalPixels(capture(), page.copy(0, 0, rectangle.width(), rectangle.height())), "Real X11 source pixels match the painted surface exactly");
        const auto* reused = source.pixels().data(); capture();
        require(source.pixels().data() == reused, "The native source reuses its owned same-sized BGRA storage");
        require(!source.capture(-1, 0, 30, 30) && !source.error().empty(), "Out-of-bounds native capture fails explicitly");
        require(source.scroll(rectangle.center().x(), rectangle.center().y(), 1), source.error());
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (pattern.displayedOffset.load() != 40 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
        require(pattern.displayedOffset.load() == 40, "Real XTEST wheel input reaches the owned target");
        const auto moved = capture();
        require(equalPixels(moved, page.copy(0, 40, rectangle.width(), rectangle.height())), "Native capture reads the actually scrolled surface");
        require(source.capture(rectangle.x(), rectangle.y(), rectangle.width(), rectangle.height(), true), source.error());
        const QImage cursor(source.pixels().data(), source.width(), source.height(), static_cast<qsizetype>(source.stride()), QImage::Format_ARGB32);
        require(!equalPixels(cursor, moved), "Explicit XFixes cursor capture adds the real native cursor");
        for (int y = 0; y < cursor.height(); ++y) for (int x = 0; x < cursor.width(); ++x)
            require(qAlpha(cursor.pixel(x, y)) == 255, "Cursor-composited desktop pixels remain opaque");
        platform::NativeScrollSource shared(rectangle.x(), rectangle.y(), rectangle.width(), rectangle.height());
        require(shared.capture(), shared.error());
        require(shared.scroll() == platform::ScrollInputResult::Scrolled, shared.error());
        const auto next = std::chrono::steady_clock::now() + 5s;
        while (pattern.displayedOffset.load() != 80 && std::chrono::steady_clock::now() < next) std::this_thread::sleep_for(1ms);
        require(pattern.displayedOffset.load() == 80 && shared.capture(), "Shared native scrolling keeps its real source alive");
        const QImage sharedImage(shared.pixels().data(), rectangle.width(), rectangle.height(), rectangle.width() * 4, QImage::Format_ARGB32);
        require(equalPixels(sharedImage, page.copy(0, 80, rectangle.width(), rectangle.height())), "The shared scroll adapter preserves exact source bytes");
    });
}
capture::ActionRequest desktopSession(Pattern& pattern)
{
    pattern.reset(); capture::DesktopCapture service; service.initialize();
    require(service.capability().canRequest(), "The native X11 capture service is available");
    capture::Snapshot snapshot; quint64 hostToken{}; int completed{}, stopped{};
    QObject::connect(&service, &capture::DesktopCapture::finished, &service,
        [&](quint64 token, const capture::Snapshot& result, const capture::Failure& failure) {
            require(token == hostToken && hostToken != 0, "The host receives its accepted token before finished delivery");
            require(!failure, failure.detail.toStdString()); snapshot = result; ++completed;
        });
    QObject::connect(&service, &capture::DesktopCapture::shutdownFinished, &service, [&] { ++stopped; });
    hostToken = service.request(); require(hostToken && completed == 0, "A real capture request completes asynchronously");
    wait([&] { return completed == 1; }, "The actual desktop screenshot worker completes");
    require(snapshot.coordinates == capture::CoordinateSpace::DesktopPixels && snapshot.desktopOrigin && !snapshot.screens.isEmpty(),
        "Physical desktop capture publishes verified per-monitor GUI bindings");
    capture::ActionRequest request;
    request.action = capture::Action::Scroll; request.coordinates = snapshot.coordinates; request.screens = snapshot.screens;
    request.desktopRect = QRect(pattern.mapToGlobal(QPoint()), pattern.size());
    request.sourceRect = request.desktopRect->translated(-*snapshot.desktopOrigin);
    request.image = snapshot.image.copy(request.sourceRect);
    require(equalPixels(request.image, pattern.page.copy(0, 0, pattern.width(), pattern.height())), "The shared desktop snapshot contains exact selected pixels");
    service.shutdown(); wait([&] { return stopped == 1 && !service.busy(); }, "Snapshot shutdown settles every worker and terminal delivery");
    service.shutdown(); settle(); require(stopped == 1, "Snapshot shutdown notification is emitted once");
    return request;
}
void cancellation()
{
    for (const bool duringAdmission : {false, true}) {
        capture::DesktopCapture service; service.initialize();
        quint64 assigned{}; int terminals{}, shutdowns{};
        QObject::connect(&service, &capture::DesktopCapture::finished, &service,
            [&](quint64 token, const capture::Snapshot&, const capture::Failure& failure) {
                require(assigned && token == assigned && failure.code == capture::Failure::Code::Cancelled,
                    "Accepted cancellation preserves the token assigned by the host"); ++terminals;
            });
        QObject::connect(&service, &capture::DesktopCapture::shutdownFinished, &service, [&] {
            require(terminals == 1 && !service.busy(), "Shutdown follows terminal delivery and actual worker drain"); ++shutdowns;
        });
        if (duringAdmission) QObject::connect(&service, &capture::DesktopCapture::busyChanged, &service,
            [&](bool busy) { if (busy) service.shutdown(); });
        assigned = service.request();
        require(assigned && terminals == 0 && shutdowns == 0, "No accepted terminal is delivered within request admission");
        if (!duringAdmission) { service.cancel(assigned); service.shutdown(); }
        require(service.busy(), "Cancellation does not pretend pending worker/terminal delivery is already drained");
        wait([&] { return shutdowns == 1; }, "Cancelled native screenshot shutdown completes");
        service.cancel(assigned); service.shutdown(); settle();
        require(terminals == 1 && shutdowns == 1, "Late duplicate cancellation cannot produce duplicate completions");
    }
}
QWidget* scrollView()
{
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QStringLiteral("scrollCaptureWindow")) return widget;
    return nullptr;
}
void scrolling(Pattern& pattern, const capture::ActionRequest& request)
{
    QTemporaryDir directory; require(directory.isValid(), "Create isolated output storage");
    auto archive = std::make_shared<HuhuCapture::Media::MediaArchiveService>(directory.path());
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    app::MediaWorkspace workspace(archive, clipboard, revealer);
    app::ScrollController controller(archive, workspace);
    QString failure; QObject::connect(&controller, &app::ScrollController::operationFailed, &controller, [&](const QString& text) { failure = text; });
    pattern.reset(); require(controller.begin(request), "Open the real shared scrolling controls");
    auto* view = scrollView(); require(view, "The production scroll window exists");
    auto* start = view->findChild<QPushButton*>(QStringLiteral("scrollStart"));
    auto* stop = view->findChild<QPushButton*>(QStringLiteral("scrollStop"));
    auto* dimensions = view->findChild<QLabel*>(QStringLiteral("scrollDimensions"));
    require(start && stop && dimensions, "Real scrolling actions and progress are present");
    view->hide(); controller.activate(); require(view->isVisible(), "The global activation path recovers hidden controls");
    start->click();
    wait([&] { return !failure.isEmpty() || dimensions->text().split(QChar(u' ')).value(2).toInt() >= 340; }, "Actual scrolling appends new source rows");
    require(failure.isEmpty(), failure.toStdString()); stop->click();
    wait([&] { return !stop->isEnabled(); }, "The real native scrolling worker stops before output");
    const int height = dimensions->text().split(QChar(u' ')).value(2).toInt();
    require(height >= 340, "The stitched result contains more than the initial screenshot");
    QPointer<ui::EditorWindow> pin;
    QObject::connect(&workspace, &app::MediaWorkspace::editorOpened, &workspace, [&](ui::EditorWindow* window) { pin = window; });
    view->findChild<QPushButton*>(QStringLiteral("scrollPin"))->click();
    wait([&] { return pin && !controller.busy(); }, "Completed scrolling pixels open in the shared real editor");
    require(equalPixels(pin->canvas()->originalImage(), pattern.page.copy(0, 0, pattern.width(), height)),
        "The pinned scrolling result preserves every actual source row exactly");
    int shutdowns{}; QObject::connect(&controller, &app::ScrollController::shutdownFinished, &controller, [&] { ++shutdowns; });
    controller.shutdown(); wait([&] { return shutdowns == 1; }, "Idle scrolling controller shuts down once");
    controller.shutdown(); settle(); require(shutdowns == 1, "Repeated scrolling shutdown is idempotent");
    pin->close(); settle();

    // Stop while its exclusion interval is awaiting the GUI. This exercises the
    // stop-aware native capture gate with real widgets and a real worker.
    app::ScrollController interrupted(archive, workspace); pattern.reset();
    require(interrupted.begin(request), "Start another owned native scrolling session");
    QPointer<QWidget> hidden = scrollView(); require(hidden, "The second control window exists");
    hidden->move(request.desktopRect->topLeft());
    hidden->findChild<QPushButton*>(QStringLiteral("scrollStart"))->click();
    wait([&] { return !hidden->isVisible(); }, "Overlapping controls are excluded before native capture");
    interrupted.activate(); require(!hidden->isVisible(), "Hotkey activation waits until the in-flight capture exclusion ends");
    int interruptedShutdowns{};
    QObject::connect(&interrupted, &app::ScrollController::shutdownFinished, &interrupted, [&] { ++interruptedShutdowns; });
    interrupted.shutdown();
    wait([&] { return interruptedShutdowns == 1 && !interrupted.busy(); }, "Shutdown wakes a worker waiting on the GUI capture gate");
    require(!hidden, "Shutdown releases the actual hidden scroll window");
    bool workspaceStopped{}; QObject::connect(&workspace, &app::MediaWorkspace::shutdownFinished, &workspace, [&] { workspaceStopped = true; });
    workspace.shutdown(); wait([&] { return workspaceStopped; }, "The shared output workspace drains");
    clipboard.shutdown(); revealer.shutdown(); settle();
}
}
int main(int argc, char** argv)
{
    if (qEnvironmentVariable("HUHUCAPTURE_TEST_X11_ISOLATED") != QStringLiteral("1")) {
        std::cerr << "Run only with scripts/test-x11-capture.py; this test injects wheel input into its owned display\n"; return 1;
    }
    QApplication application(argc, argv); QApplication::setQuitOnLastWindowClosed(false);
    try {
        require(QGuiApplication::platformName() == QStringLiteral("xcb") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"),
            "Require the explicitly isolated native X11 platform");
        Pattern pattern; pattern.show();
        wait([&] { return pattern.windowHandle() && pattern.windowHandle()->isExposed(); }, "Expose the actual owned X11 pattern window");
        require(pattern.devicePixelRatioF() == 1, "This native-byte fixture uses an explicit 1:1 X11 test display");
        nativePixels(pattern); const auto request = desktopSession(pattern); cancellation(); scrolling(pattern, request);
        std::cout << "Real X11 pixels, RandR bindings, XFixes cursor, XTEST scroll, exact stitching, pin output and cancellation/shutdown passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
