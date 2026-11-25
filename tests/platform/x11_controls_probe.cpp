#include "platform/linux/X11Capture.h"
#include "core/MediaClock.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QException>
#include <QImage>
#include <QPainter>
#include <QRegion>
#include <QWidget>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <xcb/xcb.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace core = HuhuCapture::Core;
namespace native = HuhuCapture::Platform::Linux;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* detail)
{ if (!value) throw std::runtime_error(detail); }
template<class Predicate> void waitFor(Predicate predicate)
{
    const auto deadline = core::MediaClock::now() + 10s;
    while (!predicate()) {
        require(core::MediaClock::now() < deadline, "The actual native control transition did not complete");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
}
class Painted final : public QWidget {
public:
    explicit Painted(QColor color) : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint |
        Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint), color_(color) { setAttribute(Qt::WA_OpaquePaintEvent); }
    unsigned paints{};
protected:
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(rect(), color_); ++paints; }
private:
    QColor color_;
};
struct Free { void operator()(void* value) const noexcept { std::free(value); } };
native::X11Rectangle physical(QWidget& widget)
{
    int number{};
    auto* connection = xcb_connect(nullptr, &number);
    struct Close { xcb_connection_t* value; ~Close() { if (value) xcb_disconnect(value); } } close{connection};
    require(connection && !xcb_connection_has_error(connection), "Open the owned native coordinate connection");
    auto root = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int index = 0; index < number && root.rem; ++index) xcb_screen_next(&root);
    require(root.rem, "Read the actual native X screen");
    const auto window = static_cast<xcb_window_t>(widget.winId());
    const std::unique_ptr<xcb_get_geometry_reply_t, Free> geometry(
        xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr));
    const std::unique_ptr<xcb_translate_coordinates_reply_t, Free> origin(
        xcb_translate_coordinates_reply(connection, xcb_translate_coordinates(connection, window, root.data->root, 0, 0), nullptr));
    require(geometry && origin && origin->same_screen, "Read actual control/selection pixels without a guessed DPR");
    return {origin->dst_x, origin->dst_y, geometry->width, geometry->height};
}
struct Capture { bool accepted{}; std::string error; QImage pixels; };
Capture capture(native::X11Rectangle selected, core::CaptureControlSurface control)
{
    // The probe paints and changes SHAPE on Qt's XCB connection, whereas the
    // real capture worker owns another connection. Expose/paintEvent counters
    // and a query on a third connection do not flush/order Qt's requests.
    // Qt's public XCB sync performs its own connection roundtrip after event
    // delivery. This is test-window request ordering, not a compositor fence.
    QGuiApplication::sync();
    auto future = QtConcurrent::run([=] {
        native::X11Capture source;
        Capture value;
        value.accepted = source.initialize() && source.captureGuarded(selected.x, selected.y,
            selected.width, selected.height, false, std::array{control});
        value.error = source.error();
        if (value.accepted) value.pixels = QImage(source.pixels().data(), source.width(), source.height(),
            static_cast<qsizetype>(source.stride()), QImage::Format_ARGB32).copy();
        return value;
    });
    waitFor([&] { return future.isFinished(); });
    try { return future.takeResult(); }
    catch (const QUnhandledException& error) {
        if (error.exception()) std::rethrow_exception(error.exception());
        throw;
    }
}
void cleanPixels(const Capture& value, QColor expected, const char* stage)
{
    if (!value.accepted) throw std::runtime_error(std::string(stage) + ": real guarded readback failed: " + value.error);
    require(!value.pixels.isNull(), "Read actual guarded pixels");
    const auto directory = QDir::current().filePath(QStringLiteral("x11-controls-evidence"));
    require(QDir().mkpath(directory), "Create actual X11 pixel evidence directory");
    const auto path = QDir(directory).filePath(QString::fromLatin1(stage) + QStringLiteral(".png"));
    require(value.pixels.save(path), "Save every actual guarded readback before inspecting it");
    for (int y = 0; y < value.pixels.height(); ++y)
        for (int x = 0; x < value.pixels.width(); ++x) {
            const auto actual = value.pixels.pixelColor(x, y);
            if (actual != expected)
                throw std::runtime_error(std::string(stage) + ": selected pixel (" + std::to_string(x) + "," +
                    std::to_string(y) + ") is " + actual.name(QColor::HexArgb).toStdString() + ", expected " +
                    expected.name(QColor::HexArgb).toStdString() + "; actual readback: " + path.toStdString());
        }
    std::cout << "PASS actual guarded pixels: " << stage << " (" << value.pixels.width() << 'x' <<
        value.pixels.height() << ")\n";
}
void refused(const Capture& value)
{ require(!value.accepted && !value.error.empty() && value.pixels.isNull(), "A real unsafe control transition must not publish any captured pixels"); }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        require(QGuiApplication::platformName() == QStringLiteral("xcb"), "Native control checks require a real isolated X server");
        const QColor target(30, 80, 160), controlColor(250, 20, 80);
        Painted pattern(target); pattern.setGeometry(200, 180, 256, 192); pattern.show();
        waitFor([&] { return pattern.windowHandle() && pattern.windowHandle()->isExposed() && pattern.paints; });
        const auto selected = physical(pattern);
        Painted control(controlColor); control.setGeometry(30, 200, 90, 44); control.show(); control.raise();
        waitFor([&] { return control.windowHandle() && control.windowHandle()->isExposed() && control.paints; });
        const auto id = static_cast<std::uintptr_t>(control.winId());
        cleanPixels(capture(selected, {id, core::CaptureControlPlacement::Outside}), target, "initial-outside");

        control.move(pattern.pos() + QPoint(30, 30));
        waitFor([&] { return physical(control).x > selected.x; });
        refused(capture(selected, {id, core::CaptureControlPlacement::Outside}));
        control.setGeometry(30, 200, 90, 44);
        waitFor([&] { const auto value = physical(control); return value.x + value.width < selected.x; });
        cleanPixels(capture(selected, {id, core::CaptureControlPlacement::Outside}), target, "moved-outside");
        control.resize(260, 44);
        waitFor([&] { const auto value = physical(control); return value.x + value.width > selected.x; });
        refused(capture(selected, {id, core::CaptureControlPlacement::Outside}));

        control.hide();
        waitFor([&] { return !control.windowHandle()->isExposed(); });
        cleanPixels(capture(selected, {id, core::CaptureControlPlacement::Hidden}), target, "hidden");
        control.setGeometry(pattern.geometry().adjusted(-10, -10, 10, 10));
        const auto hole = control.rect().adjusted(8, 8, -8, -8);
        control.setMask(QRegion(control.rect()) - QRegion(hole));
        const auto before = control.paints; control.show(); control.raise();
        waitFor([&] { return control.windowHandle()->isExposed() && control.paints > before; });
        cleanPixels(capture(selected, {id, core::CaptureControlPlacement::Outside}), target, "hollow-outline");
        refused(capture(selected, {id, core::CaptureControlPlacement::Hidden}));
        control.clearMask();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        refused(capture(selected, {id, core::CaptureControlPlacement::Outside}));
        refused(capture(selected, {id, core::CaptureControlPlacement::Excluded}));

        auto destroyed = std::make_unique<Painted>(controlColor);
        destroyed->setGeometry(30, 30, 60, 30); destroyed->show();
        waitFor([&] { return destroyed->windowHandle() && destroyed->windowHandle()->isExposed(); });
        const auto removedId = static_cast<std::uintptr_t>(destroyed->winId());
        destroyed.reset(); QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        refused(capture(selected, {removedId, core::CaptureControlPlacement::Outside}));
        std::cout << "PASS actual X11 outside/hidden controls, native move/resize/shape/destroy refusal and every pixel under a real hollow window\n";
        std::cout << "NOT RUN here: composited X11 desktop timing, physical mixed DPI, Wayland presentation or Windows affinity\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL native X11 controls: " << error.what() << '\n'; return 1; }
}
