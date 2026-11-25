#include "app/MediaWorkspace.h"
#include "app/ScrollController.h"
#include "services/capture/DesktopCapture.h"
#include "services/capture/ScreenCastSource.h"
#include "services/desktop/FileRevealer.h"
#include "services/desktop/SystemClipboard.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QEventLoop>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace capture = HuhuCapture::Capture;
namespace app = HuhuCapture::Application;
namespace desktop = HuhuCapture::Desktop;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void wait(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate()) { require(std::chrono::steady_clock::now() < deadline, message); settle(); QThread::msleep(1); }
}
void screenshotInitialization()
{
    capture::DesktopCapture service; int initialized{}, terminals{}, shutdowns{};
    QObject::connect(&service, &capture::DesktopCapture::initialized, &service, [&](const capture::Capability& capability) {
        require(!capability.canRequest() && !capability.reason.isEmpty(), "The absent real Portal reports unavailable instead of capture success"); ++initialized;
    });
    QObject::connect(&service, &capture::DesktopCapture::finished, &service, [&] { ++terminals; });
    QObject::connect(&service, &capture::DesktopCapture::shutdownFinished, &service, [&] { ++shutdowns; });
    service.initialize();
    require(service.busy(), "Actual Portal capability worker is included in busy state");
    wait([&] { return initialized == 1; }, "Real missing-Portal initialization completes");
    require(!service.request() && !service.busy() && terminals == 0, "Unavailable capability rejects admission without fabricating a token or screenshot");
    service.shutdown(); wait([&] { return shutdowns == 1; }, "Unavailable screenshot service drains");
    service.shutdown(); settle(); require(shutdowns == 1, "Screenshot shutdown is emitted once");

    capture::DesktopCapture interrupted; int interruptedShutdowns{};
    QObject::connect(&interrupted, &capture::DesktopCapture::shutdownFinished, &interrupted, [&] {
        require(!interrupted.busy(), "Shutdown follows actual initialization completion"); ++interruptedShutdowns;
    });
    interrupted.initialize(); interrupted.shutdown();
    require(interrupted.busy() && interruptedShutdowns == 0, "Stopping cannot skip a still-running Portal initialization worker");
    wait([&] { return interruptedShutdowns == 1; }, "Stop during real Portal initialization settles asynchronously");
    require(!interrupted.request(), "A stopped screenshot service cannot restart");
}
void sourceInitialization(QWidget& parent)
{
    capture::ScreenCastSource source; int failed{}, stopped{}, previews{};
    QObject::connect(&source, &capture::ScreenCastSource::failed, &source, [&](const capture::Failure& failure) {
        require(failure && !failure.detail.isEmpty(), "Preserve the real missing-Portal failure"); ++failed;
    });
    QObject::connect(&source, &capture::ScreenCastSource::stopped, &source, [&] { require(!source.busy(), "Stopped means the source owner is drained"); ++stopped; });
    QObject::connect(&source, &capture::ScreenCastSource::ready, &source, [&] { ++previews; });
    require(source.start(parent.windowHandle(), false, 8), "Begin asynchronous real Portal discovery from an actual Qt surface");
    require(!source.takeFrameSource(), "No authorized preview means no frame reader");
    wait([&] { return stopped == 1; }, "Absent actual ScreenCast service settles");
    require(failed == 1 && previews == 0 && !source.takeFrameSource(), "A Portal fault cannot create a fabricated preview or reader");
    source.stop(); settle(); require(stopped == 1, "Repeated stop cannot duplicate source termination");

    require(source.start(parent.windowHandle(), false, 8), "A drained owner may start a fresh authorization generation");
    source.stop();
    require(source.busy() && stopped == 1, "Stop retains the owner until the active initialization callback returns");
    wait([&] { return stopped == 2; }, "Cancelled source initialization actually drains");
    require(failed == 1 && previews == 0, "Explicit cancellation does not redispatch an old source failure");

    auto* removed = new capture::ScreenCastSource; QPointer<capture::ScreenCastSource> guard(removed);
    QObject::connect(removed, &capture::ScreenCastSource::failed, removed, [removed] { delete removed; });
    require(removed->start(parent.windowHandle(), false), "Begin a lifetime-sensitive actual Portal failure");
    wait([&] { return guard.isNull(); }, "A synchronous failure observer may destroy the source safely");
}
void scrollInitialization()
{
    QTemporaryDir directory; require(directory.isValid(), "Create actual temporary output storage");
    auto archive = std::make_shared<HuhuCapture::Media::MediaArchiveService>(directory.path());
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    app::MediaWorkspace workspace(archive, clipboard, revealer);
    capture::ActionRequest request;
    request.action = capture::Action::Scroll; request.coordinates = capture::CoordinateSpace::AuthorizedSourcePixels;
    request.sourceRect = QRect(0, 0, 320, 240);
    request.image = QImage(request.sourceRect.size(), QImage::Format_ARGB32); request.image.fill(Qt::green);
    // This image is just a retained input fixture. The real missing Portal must
    // never turn it into an authorized streaming frame or fake scroll result.
    app::ScrollController controller(archive, workspace); int failures{}, shutdowns{};
    QObject::connect(&controller, &app::ScrollController::operationFailed, &controller, [&](const QString& message) {
        require(!message.isEmpty(), "Scrolling reports the actual authorization failure"); ++failures;
    });
    QObject::connect(&controller, &app::ScrollController::shutdownFinished, &controller, [&] { ++shutdowns; });
    require(controller.begin(request), "Open real manual-scroll controls before authorization");
    wait([&] { return failures == 1; }, "Manual scrolling sees the actual missing Portal");
    require(controller.busy(), "A failed source keeps the recoverable input window until dismissal");
    controller.shutdown(); wait([&] { return shutdowns == 1 && !controller.busy(); }, "Scroll shutdown releases failed source and real Qt controls");
    controller.shutdown(); settle(); require(shutdowns == 1, "Scroll shutdown completion is emitted once");

    auto* removed = new app::ScrollController(archive, workspace); QPointer<app::ScrollController> guard(removed);
    QObject::connect(removed, &app::ScrollController::busyChanged, removed, [removed](bool busy) { if (busy) delete removed; });
    require(removed->begin(request) && guard.isNull(), "A synchronous busy observer can destroy newly created scroll controls safely");
    settle();
    bool drained{}; QObject::connect(&workspace, &app::MediaWorkspace::shutdownFinished, &workspace, [&] { drained = true; });
    workspace.shutdown(); wait([&] { return drained; }, "Shared workspace shutdown settles");
    clipboard.shutdown(); revealer.shutdown(); settle();
}
}
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("XDG_SESSION_TYPE", "wayland");
    qputenv("LIBPORTAL_PORTAL_BUS_NAME", "org.huhucapture.CaptureServiceUnavailable");
    QApplication application(argc, argv); QApplication::setQuitOnLastWindowClosed(false);
    try {
        require(QAbstractEventDispatcher::instance()->inherits("QEventDispatcherGlib"), "Actual Qt/GLib dispatcher is required");
        QWidget parent; parent.resize(320, 200); parent.show(); parent.winId(); settle();
        screenshotInitialization(); sourceInitialization(parent); scrollInitialization();
        std::cout << "Real missing-Portal initialization, cancellation, owner lifetime and capture/scroll shutdown passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
