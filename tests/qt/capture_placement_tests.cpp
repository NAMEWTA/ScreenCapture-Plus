#include "ui/qt/CapturePlacement.h"
#include "ui/qt/EditorWindow.h"
#include "platform/WindowActivation.h"

#include <QApplication>
#include <QScreen>
#include <QWindow>
#include <iostream>
#include <stdexcept>

namespace capture = HuhuCapture::Capture;
namespace ui = HuhuCapture::UI::Qt;
namespace platform = HuhuCapture::Platform;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void settle() { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
capture::ActionRequest desktopRequest(QScreen* screen, QPoint logical)
{
    capture::ActionRequest result;
    result.coordinates = capture::CoordinateSpace::DesktopPixels;
    capture::ScreenBinding binding;
    binding.screen = screen; binding.nativeId = screen->name(); binding.screenId = 1;
    binding.logicalGeometry = screen->geometry(); binding.logicalWorkArea = screen->availableGeometry();
    // Deliberately nonzero source origin and fractional scale exercise the
    // explicit binding math. These are test inputs, not physical-screen evidence.
    binding.sourcePixels = QRect(QPoint(2700, 1300), screen->size() * 1.5);
    binding.pixelsPerDipX = 1.5; binding.pixelsPerDipY = 1.5;
    const QPoint source = binding.sourcePixels.topLeft() + (logical - screen->geometry().topLeft()) * 1.5;
    result.sourceRect = QRect(source, QSize(160, 80));
    result.desktopRect = QRect(source - QPoint(4000, 2000), result.sourceRect.size());
    result.screens = {binding};
    result.image = QImage(result.sourceRect.size(), QImage::Format_ARGB32); result.image.fill(Qt::blue);
    return result;
}
void placement()
{
    auto* screen = QGuiApplication::primaryScreen(); require(screen, "The Qt platform provides an actual screen");
    const auto point = screen->availableGeometry().topLeft() + QPoint(20, 20);
    const auto request = desktopRequest(screen, point);
    ui::EditorWindow editor;
    require(!editor.loadImage(request.image), "Load the selected owned pixels");
    editor.canvas()->setZoom(.25); editor.show(); settle();
    require(ui::placeCapturedEditor(&editor, request), "Verified native selection is placed on its bound screen");
    settle();
    require(editor.canvas()->mapToGlobal(QPoint()) == point, "Initial image origin follows the per-monitor source binding");
    require(editor.canvas()->zoom() == 1, "Initial refit recovers from an earlier primary-screen fit when the image fits");
    editor.move(point + QPoint(30, 40)); editor.canvas()->setZoom(.5); settle();
    const auto moved = editor.pos();
    require(!ui::placeCapturedEditor(&editor, request) && editor.pos() == moved && editor.canvas()->zoom() == .5,
        "Repeated placement cannot recenter or reset a pin after the user moves or zooms it");
    ui::EditorWindow untouched; require(!untouched.loadImage(request.image), "Prepare a second pristine pin");
    untouched.move(point); const auto before = untouched.pos();
    auto sourceOnly = request; sourceOnly.coordinates = capture::CoordinateSpace::AuthorizedSourcePixels; sourceOnly.desktopRect.reset();
    require(!ui::placeCapturedEditor(&untouched, sourceOnly) && untouched.pos() == before,
        "Authorized source pixels do not invent desktop placement");
    auto stale = request; stale.screens[0].logicalGeometry.translate(1, 0);
    require(!ui::placeCapturedEditor(&untouched, stale) && untouched.pos() == before,
        "A changed monitor binding cannot move a new pin using stale geometry");
    auto malformed = request; malformed.desktopRect->setWidth(malformed.desktopRect->width() + 1);
    require(!ui::placeCapturedEditor(&untouched, malformed), "Source and physical selection extents must agree");
}
void activationNoOp()
{
    require(platform::applyActivationToken(nullptr, {}) == platform::ActivationTokenResult::NotNeeded,
        "An absent activation token is a no-op without creating a native surface");
    if (!QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        QWindow window;
        require(platform::applyActivationToken(&window, QStringLiteral("test-non-wayland-only")) ==
            platform::ActivationTokenResult::NotNeeded && !window.handle(),
            "Other platform plugins never create a Wayland connection or window for a token");
    }
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); QApplication::setQuitOnLastWindowClosed(false);
    try { placement(); activationNoOp(); std::cout << "Capture placement and activation no-op contracts passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
