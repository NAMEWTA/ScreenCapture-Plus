#include "capture/CaptureSession.h"
#include "ui/qt/CaptureWindow.h"

#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace capture = HuhuCapture::Capture;
namespace ui = HuhuCapture::UI::Qt;
using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void wait(Predicate predicate, const char* message)
{
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) { require(std::chrono::steady_clock::now() < end, message); settle(); QThread::msleep(1); }
}
capture::Snapshot image(quint64 identity = 1)
{
    capture::Snapshot result;
    result.image = QImage(320, 240, QImage::Format_ARGB32);
    result.image.fill(QColor(10, 90, 180, 123));
    result.image.setPixelColor(14, 16, QColor(220, 40, 90, 0));
    result.sourceId = identity; result.generation = 7;
    result.cursor = {14, 16};
    return result;
}
void select(capture::CaptureSession& session)
{
    session.pointerPress({10, 12}); session.pointerMove({210, 172}); session.pointerRelease({210, 172}, false);
    require(session.stage() == capture::Stage::Adjusting, "A completed source-pixel gesture enters adjustment");
}
void contracts()
{
    capture::CaptureSession session;
    for (const auto action : {capture::Action::Clipboard, capture::Action::Edit, capture::Action::Pin, capture::Action::Hex,
        capture::Action::Rgb, capture::Action::Cmyk, capture::Action::Position})
        session.setCapability(action, {capture::CapabilityState::Available, {}});
    auto invalid = image(); invalid.image.setDevicePixelRatio(2);
    require(session.load(invalid).code == capture::Failure::Code::InvalidSnapshot, "Reject an implicit-DPR source image");
    invalid = image(); invalid.desktopOrigin = QPoint(0, 0);
    require(session.load(invalid).code == capture::Failure::Code::InvalidSnapshot, "Do not invent desktop coordinates for an authorized image");
    const auto original = image(); require(!session.load(original), "Load explicit source pixels");
    select(session);
    capture::ActionRequest requested;
    unsigned requests{};
    QObject::connect(&session, &capture::CaptureSession::actionRequested, &session, [&](const auto& request) { requested = request; ++requests; });
    const auto token = session.request(capture::Action::Clipboard);
    require(token && session.stage() == capture::Stage::Submitting, "Image output has a pending token before its worker completes");
    require(!session.request(capture::Action::Clipboard), "Reject duplicate output while the crop worker is active");
    wait([&] { return requests == 1; }, "The actual image crop worker completes");
    require(requested.image == original.image.copy(QRect(10, 12, 200, 160)), "Crop preserves complete source pixels including transparent RGB");
    require(requested.sourceRect == QRect(10, 12, 200, 160) && !requested.desktopRect, "Only verified coordinate space crosses the action boundary");
    session.complete(token + 1, capture::ActionResult::Completed);
    require(session.stage() == capture::Stage::Submitting, "A stale completion cannot close a newer request");
    session.complete(token, capture::ActionResult::Failed, QStringLiteral("actual clipboard error"));
    require(session.stage() == capture::Stage::Adjusting && session.snapshot().image == original.image && session.selection().hasSelection(),
        "Output failure retains the exact source and selected region");
    require(session.failureText() == QStringLiteral("actual clipboard error"), "Preserve the actual failure detail");
    session.pointerMove({14, 16});
    const auto position = session.request(capture::Action::Position);
    require(requested.text == QStringLiteral("source(14,16)"), "Wayland source positions cannot masquerade as desktop positions");
    session.complete(position, capture::ActionResult::Cancelled);
    require(session.stage() == capture::Stage::Adjusting, "Cancelling output keeps the editable capture");
    const auto edited = session.request(capture::Action::Edit);
    const auto editCount = requests;
    wait([&] { return requests == editCount + 1; }, "Ordinary annotation prepares a real selected image");
    require(edited && requested.action == capture::Action::Edit && requested.token == edited &&
        requested.image == original.image.copy(QRect(10, 12, 200, 160)),
        "Ordinary annotation remains a distinct editor action from Ctrl-release pinning");
    session.complete(edited, capture::ActionResult::Cancelled);
    const auto cancelled = session.request(capture::Action::Pin);
    const auto before = requests;
    session.cancel(); settle();
    require(cancelled && session.stage() == capture::Stage::Closed, "Explicit capture cancellation closes the session");
    require(session.snapshot().image.isNull(), "Closing a capture releases the retained full-desktop image");
    for (int i = 0; i < 10; ++i) settle();
    require(requests == before, "A late crop worker cannot dispatch an already-cancelled action");
    require(!session.load(image(2)), "A closed capture can load a new source identity");
    session.complete(cancelled, capture::ActionResult::Failed, QStringLiteral("late"));
    require(session.stage() == capture::Stage::Selecting && session.failureText().isEmpty(), "Stale completion never changes a new source");
    session.cancel();
}
void windows()
{
    capture::CaptureSession session;
    session.setCapability(capture::Action::Clipboard, {capture::CapabilityState::Available, {}});
    session.setCapability(capture::Action::Pin, {capture::CapabilityState::Available, {}});
    ui::CaptureWindow view(session);
    unsigned closed{};
    QObject::connect(&view, &ui::CaptureWindow::closed, &view, [&] { ++closed; });
    require(!view.showSnapshot(image()), "Create a real Qt source-image capture surface");
    require(view.visible() && view.dialogParent(), "The capture owns a visible dialog and clipboard origin");
    auto* surface = view.dialogParent();
    select(session); settle();
    auto* copy = surface->findChild<QToolButton*>(QStringLiteral("capture.tool.clipboard"));
    auto* scroll = surface->findChild<QToolButton*>(QStringLiteral("capture.cap.long"));
    require(copy && copy->isEnabled() && copy->size() == QSize(44, 44), "The real toolbar retains its 44 DIP copy target");
    require(scroll && !scroll->isEnabled(), "Unavailable capabilities cannot trigger placeholder output");
    capture::ActionRequest output;
    QObject::connect(&session, &capture::CaptureSession::actionRequested, &view, [&](const auto& request) { output = request; });
    surface->setFocus();
    for (const auto key : {Qt::Key_C, Qt::Key_S}) {
        QKeyEvent pressed(QEvent::KeyPress, key, Qt::ControlModifier);
        QApplication::sendEvent(surface, &pressed);
        QKeyEvent released(QEvent::KeyRelease, key, Qt::ControlModifier);
        QApplication::sendEvent(surface, &released);
    }
    settle();
    require(!output.token && session.stage() == capture::Stage::Adjusting,
        "Selection preserves baseline Ctrl+C/S stage rules; its real copy/save toolbar remains available");
    auto* history = surface->findChild<QToolButton*>(QStringLiteral("capture.clipboard.title"));
    require(history && !history->icon().isNull() && history->iconSize() == QSize(24, 24),
        "History has an actual cross-platform icon instead of a missing Segoe glyph");
    copy->click();
    require(view.visible(), "Submitting preserves the visible Wayland clipboard origin until the real service completes");
    wait([&] { return output.token != 0; }, "The toolbar forwards an actual prepared selection");
    view.hideForAction(); require(!view.visible(), "A host can explicitly suspend the capture before a native source operation");
    session.complete(output.token, capture::ActionResult::Cancelled); settle();
    require(view.visible(), "A cancelled service restores the retained selection surface");
    view.cancel(); wait([&] { return closed == 1; }, "Close releases real widgets after their event callbacks return");
    require(!view.dialogParent() && !view.visible(), "Closed surfaces are released instead of retained offscreen");
}
void synchronousLifetime()
{
    auto* session = new capture::CaptureSession;
    require(!session->load(image()), "Prepare deletion-safe session");
    session->setCapability(capture::Action::Position, {capture::CapabilityState::Available, {}});
    QPointer<capture::CaptureSession> guard(session);
    QObject::connect(session, &capture::CaptureSession::changed, session, [session] { delete session; });
    require(session->request(capture::Action::Position) != 0 && !guard, "Synchronous state observers may destroy the session safely");
}
}
int main(int argc, char** argv)
{
    QApplication application(argc, argv); QApplication::setQuitOnLastWindowClosed(false);
    try { contracts(); windows(); synchronousLifetime(); std::cout << "Qt capture session and surfaces passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
