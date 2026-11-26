#include "services/capture/DesktopCapture.h"
#include "platform/DesktopPointer.h"

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
        Capture::DesktopCapture service;
        bool initialized = false, stopped = false;
        Capture::Capability capability;
        std::map<quint64, Result> results;
        QObject::connect(&service, &Capture::DesktopCapture::initialized, &service,
            [&](const auto& value) { require(!initialized, "capture initializes once"); capability = value; initialized = true; });
        QObject::connect(&service, &Capture::DesktopCapture::finished, &service,
            [&](quint64 token, const auto& image, const auto& error) {
                require(!results.contains(token), "one actual screen request completes once"); results.emplace(token, Result{image, error});
            });
        QObject::connect(&service, &Capture::DesktopCapture::shutdownFinished, &service,
            [&] { require(!stopped, "native resources drain once"); stopped = true; });
        service.initialize(); waitFor([&] { return initialized; });
        if (unavailable) {
            require(!capability.canRequest() && !capability.reason.isEmpty(),
                "offscreen QPA cannot advertise a synthetic desktop source");
            require(service.request() == 0 && results.empty(),
                "unavailable capture rejects admission and publishes no placeholder pixels");
            service.shutdown(); require(service.request() == 0, "shutdown closes capture admission immediately"); waitFor([&] { return stopped; });
            std::cout << "PASS actual unsupported-QPA capture refusal and asynchronous worker shutdown\n"; return 0;
        }
        require(capability.state == Capture::CapabilityState::Available,
            "the actual native desktop capture is initialized");
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
        require(first.sourceId && first.generation, "the actual published source has a nonzero identity and generation");
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
        // Native pointer movement is the shared platform action used by the
        // final CaptureWindow. Verify its actual position by another readback.
        const auto target = sourcePoint(second, window);
        require(Platform::requestDesktopPointerMove(target + *second.desktopOrigin),
            "the actual platform accepts the native source-pixel cursor request");
        waitFor([&] { return capture().cursor == target; });
        require(Platform::requestDesktopPointerMove(second.cursor + *second.desktopOrigin),
            "restore the original actual native cursor position");
        waitFor([&] { return capture().cursor == second.cursor; });
        const auto cancelled = service.request();
        const auto busy = service.request();
        service.cancel(cancelled);
        waitFor([&] { return results.contains(cancelled); });
        require(results.at(cancelled).failure.code == Capture::Failure::Code::Cancelled && results.at(cancelled).source.image.isNull(),
            "cancelled native readback never publishes its late pixels");
        require(busy == 0, "a concurrent source request is rejected before it can reuse the active readback buffer");
        // Cancellation may be delivered while the native worker is still
        // releasing its resources. New admission stays closed until that real
        // drain completes; a terminal notification alone is not an idle state.
        waitFor([&] { return !service.busy(); });
        const auto closing = service.request();
        require(closing != 0, "the drained native worker accepts the shutdown test request");
        service.shutdown(); require(service.request() == 0, "shutdown closes capture admission immediately"); waitFor([&] { return stopped; });
        require(results.contains(closing) && results.at(closing).failure.code == Capture::Failure::Code::Cancelled,
            "shutdown reports cancellation before actual native worker resource release");
        std::cout << "PASS actual native capture, per-screen mapping, owned repeated frames, cursor movement, source supersession and cancellation/drain\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
