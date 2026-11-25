#include "services/capture/ScreenCaptureService.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace HuhuCapture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& predicate)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < 10000) { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); QThread::msleep(2); }
    require(predicate(), "the actual capture operation completes within its test deadline");
}
struct Result { Capture::Snapshot source; Capture::Failure failure; };
class ColorWindow final : public QWidget {
public:
    QColor color{39, 118, 203}; int paints{};
    ColorWindow() { setWindowTitle(QStringLiteral("huhucapture actual screen readback")); resize(240, 180); }
protected:
    void paintEvent(QPaintEvent*) override { QPainter painter(this); painter.fillRect(rect(), color); ++paints; }
};
QPoint sourcePoint(const Capture::Snapshot& snapshot, const ColorWindow& window)
{
    const auto logical = window.mapToGlobal(window.rect().center());
    for (const auto& display : snapshot.screens) {
        if (display.screen != window.windowHandle()->screen()) continue;
        const auto source = Capture::sourceFromScreenLocal(display, QPointF(logical) - display.logicalGeometry.topLeft());
        return {qFloor(source.x()), qFloor(source.y())};
    }
    throw std::runtime_error("the actual widget's native screen has one verified source binding");
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        const auto arguments = app.arguments();
        const bool unavailable = arguments.contains(QStringLiteral("--no-backend"));
        const auto artifactIndex = arguments.indexOf(QStringLiteral("--artifacts"));
        const QString artifacts = artifactIndex >= 0 && artifactIndex + 1 < arguments.size() ? arguments[artifactIndex + 1] : QString();
        if (!artifacts.isEmpty()) require(QDir().mkpath(artifacts), "create the actual native readback artifact folder");
        const auto platform = QGuiApplication::platformName();
        if (unavailable) require(platform == QStringLiteral("offscreen") || platform == QStringLiteral("minimal"),
            "the unavailable test runs on an actual non-desktop QPA");
        else require(platform == QStringLiteral("windows") || platform == QStringLiteral("xcb"),
            "native capture validation requires an actual Windows or X11 desktop");
        Capture::ScreenCaptureService service;
        bool initialized = false, stopped = false;
        Capture::CaptureCapabilities capabilities;
        Capture::Failure initialization;
        std::map<quint64, Result> results;
        QObject::connect(&service, &Capture::ScreenCaptureService::initialized, &service,
            [&](const auto& value, const auto& error) { require(!initialized, "capture initializes once"); capabilities = value; initialization = error; initialized = true; });
        QObject::connect(&service, &Capture::ScreenCaptureService::finished, &service,
            [&](quint64 token, const auto& image, const auto& error) {
                require(!results.contains(token), "one actual screen request completes once"); results.emplace(token, Result{image, error});
            });
        QObject::connect(&service, &Capture::ScreenCaptureService::shutdownFinished, &service,
            [&] { require(!stopped, "native resources drain once"); stopped = true; });
        service.initialize(); waitFor([&] { return initialized; });
        if (unavailable) {
            require(initialization.code == Capture::Failure::Code::Unavailable && !capabilities.screenshot.canRequest(),
                "offscreen QPA cannot advertise a synthetic desktop source");
            const auto token = service.request(); waitFor([&] { return results.contains(token); });
            require(results.at(token).failure.code == Capture::Failure::Code::Unavailable && results.at(token).source.image.isNull(),
                "unavailable capture publishes no placeholder pixels");
            require(!service.isCurrent(1, 1), "a nonexistent captured source cannot authorize live capture");
            service.shutdown(); require(service.request() == 0, "shutdown closes capture admission immediately"); waitFor([&] { return stopped; });
            std::cout << "PASS actual unsupported-QPA capture refusal and asynchronous worker shutdown\n"; return 0;
        }
        if (initialization) std::cerr << "INIT " << static_cast<int>(initialization.code) << ' ' << initialization.nativeCode << ' ' << initialization.detail.toStdString() << '\n';
        require(!initialization && capabilities.screenshot.state == Capture::CapabilityState::Available &&
            capabilities.desktopCoordinates.state == Capture::CapabilityState::Available, "the actual native desktop capture is initialized");
        ColorWindow window;
        auto* display = QGuiApplication::primaryScreen(); require(display, "the real desktop has a screen");
        window.move(display->availableGeometry().center() - QPoint(window.width() / 2, window.height() / 2)); window.show(); window.raise();
        waitFor([&] { return window.windowHandle() && window.windowHandle()->isExposed() && window.paints > 0; });
        const auto capture = [&]() {
            const auto token = service.request(); waitFor([&] { return results.contains(token); });
            const auto result = results.at(token);
            if (result.failure) std::cerr << "CAPTURE " << static_cast<int>(result.failure.code) << ' ' << result.failure.nativeCode << ' ' << result.failure.detail.toStdString() << '\n';
            require(!result.failure && !result.source.image.isNull(), "the real backend supplies complete owned pixels");
            require(result.source.coordinates == Capture::CoordinateSpace::DesktopPixels && result.source.desktopOrigin &&
                result.source.image.devicePixelRatio() == 1 && !result.source.screens.isEmpty(), "native pixels have a verified origin and per-screen identity");
            return result.source;
        };
        // Exposed/paint completion precedes compositor presentation. Observe the
        // actual readback, with a bounded retry, rather than assuming a delay or
        // replacing any native pixels with the fixture's application rendering.
        Capture::Snapshot first;
        QElapsedTimer presentation; presentation.start();
        do {
            first = capture();
            if (first.image.pixelColor(sourcePoint(first, window)) == window.color) break;
            QApplication::processEvents();
        } while (presentation.elapsed() < 5000);
        require(first.image.pixelColor(sourcePoint(first, window)) == window.color, "native readback contains the visible Qt window's actual pixel color");
        if (!artifacts.isEmpty()) require(first.image.save(QDir(artifacts).filePath(QStringLiteral("native-desktop.png")), "PNG"),
            "write the actual captured native desktop as a reviewable artifact");
        require(service.isCurrent(first.sourceId, first.generation), "the freshly published native source can admit live capture");
        require(!first.windows.isEmpty(), "the real backend supplies visible window candidates");
        const auto initialPixel = first.image.pixelColor(sourcePoint(first, window));
        window.color = QColor(190, 61, 92); const auto oldPaints = window.paints; window.update();
        waitFor([&] { return window.paints > oldPaints; });
        Capture::Snapshot second;
        presentation.restart();
        do {
            second = capture();
            if (second.image.pixelColor(sourcePoint(second, window)) == window.color) break;
            QApplication::processEvents();
        } while (presentation.elapsed() < 5000);
        require(second.image.pixelColor(sourcePoint(second, window)) == window.color, "a later real native frame sees the updated visible window");
        require(first.image.pixelColor(sourcePoint(first, window)) == initialPixel && first.sourceId != second.sourceId,
            "reused native buffers cannot mutate a published frozen image");
        require(!service.isCurrent(first.sourceId, first.generation) && service.isCurrent(second.sourceId, second.generation),
            "superseded source identities cannot admit recording or scrolling");
        bool moved = false; Capture::Failure cursorFailure; QPoint actual;
        QObject::connect(&service, &Capture::ScreenCaptureService::cursorMoved, &service,
            [&](quint64 source, quint64 generation, QPoint requested, QPoint point, const Capture::Failure& error) {
                require(source == second.sourceId && generation == second.generation, "cursor completion retains the current source identity");
                require(second.image.rect().contains(requested), "cursor completion carries its actual requested source pixel");
                moved = true; actual = point; cursorFailure = error;
            });
        const auto target = sourcePoint(second, window);
        service.moveCursor(second.sourceId, second.generation, target); waitFor([&] { return moved; });
        require(!cursorFailure && actual == target, "the real desktop cursor reaches the requested source pixel");
        moved = false; service.moveCursor(second.sourceId, second.generation, second.cursor); waitFor([&] { return moved; });
        require(!cursorFailure, "the test restores the original real cursor position");
        const auto cancelled = service.request();
        const auto busy = service.request();
        service.cancel(cancelled);
        waitFor([&] { return results.contains(cancelled) && results.contains(busy); });
        require(results.at(cancelled).failure.code == Capture::Failure::Code::Cancelled && results.at(cancelled).source.image.isNull(),
            "cancelled native readback never publishes its late pixels");
        require(results.at(busy).failure.code == Capture::Failure::Code::Busy, "a concurrent source request cannot reuse the active readback buffer");
        const auto closing = service.request(); service.shutdown(); require(service.request() == 0, "shutdown closes capture admission immediately"); waitFor([&] { return stopped; });
        require(results.contains(closing) && results.at(closing).failure.code == Capture::Failure::Code::Cancelled,
            "shutdown reports cancellation before actual native worker resource release");
        std::cout << "PASS actual native capture, per-screen mapping, owned repeated frames, cursor movement, source supersession and cancellation/drain\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
