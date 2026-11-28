#include "ui/qt/CaptureWindow.h"
#include "ui/qt/EditorWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QThread>
#include <QToolButton>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace HuhuCapture;
namespace ui = HuhuCapture::UI::Qt;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void events() { QCoreApplication::sendPostedEvents(); QApplication::processEvents(); }
void waitFor(const std::function<bool()>& predicate)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < 8000) { events(); QThread::msleep(2); }
    require(predicate(), "the real shared worker/UI handoff completes");
}
Capture::Snapshot source(quint64 generation = 1)
{
    Capture::Snapshot result; result.sourceId = 93; result.generation = generation;
    result.image = QImage(640, 400, QImage::Format_ARGB32);
    for (int y = 0; y < result.image.height(); ++y) for (int x = 0; x < result.image.width(); ++x)
        result.image.setPixel(x, y, qRgb(x % 251, y % 251, (x * 3 + y * 5) % 251));
    result.cursor = {120, 110};
    return result;
}
QWidget* imageView(ui::CaptureWindow& window, const QImage& image)
{
    auto* canvas = window.dialogParent();
    require(canvas && canvas->objectName() == QStringLiteral("captureSurface"),
        "the final capture presentation owns a real QWidget source surface");
    // The ordinary source window fits an image inside a 12-DIP margin and does
    // not enlarge it. Select a 1:1 fixture size so input is independently given
    // as source pixels plus that margin, without another capture implementation.
    canvas->resize(image.size() + QSize(24, 24));
    canvas->activateWindow(); canvas->setFocus(Qt::ActiveWindowFocusReason); events();
    require(canvas->size() == image.size() + QSize(24, 24) && canvas->isVisible(),
        "the source fixture has the actual one-to-one image viewport");
    return canvas;
}
void mouse(QWidget& canvas, QEvent::Type type, QPointF sourcePoint, Qt::MouseButton button, Qt::MouseButtons held)
{
    const auto local = sourcePoint + QPointF(12, 12);
    const QPointF global = QPointF(canvas.mapToGlobal(local.toPoint())) + local - QPointF(local.toPoint());
    QMouseEvent event(type, local, local, global, button, held, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
void drag(QWidget& canvas, QPointF from, QPointF to)
{
    mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    events();
}
void key(QWidget& target, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, value, modifiers); QApplication::sendEvent(&target, &event); events();
}
void save(const QImage& image, const QString& directory, const QString& name)
{ if (!directory.isEmpty()) require(image.save(QDir(directory).filePath(name)), "save actual Qt capture rendering evidence"); }
void enable(Capture::CaptureSession& session, Capture::Action action)
{ session.setCapability(action, {Capture::CapabilityState::Available, {}}); }

void realSelectionAndEditor(const QString& directory)
{
    Capture::CaptureSession session; const auto pixels = source();
    enable(session, Capture::Action::Edit); enable(session, Capture::Action::Clipboard); enable(session, Capture::Action::SaveAs);
    ui::CaptureWindow window(session); require(!window.showSnapshot(pixels), "open an actual immutable image in the final shared QWidget capture view");
    QPointer<QWidget> canvas(imageView(window, pixels.image));
    require(session.snapshot().coordinates == Capture::CoordinateSpace::AuthorizedSourcePixels &&
        session.snapshot().screens.isEmpty(), "an unmapped image stays in source coordinates");
    const auto labels = canvas->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
    require(labels.size() == 1 && labels.front()->isVisible() &&
        labels.front()->text().contains(QStringLiteral("source pixels")),
        "authorized-source coordinates are explicitly explained in the interface");
    save(canvas->grab().toImage(), directory, QStringLiteral("capture-selecting.png"));
    drag(*canvas, {60, 70}, {380, 280});
    require(session.stage() == Capture::Stage::Adjusting && session.selection().bounds().left == 60 &&
        session.selection().bounds().right == 380, "real Qt pointer events use the shared source selection");
    // A press beyond the entire rectangle adjusts the existing edge, instead
    // of restarting a selection or requiring a tiny editor-style handle.
    drag(*canvas, {420, 160}, {460, 160});
    require(session.selection().bounds().left == 60 && session.selection().bounds().right == 460,
        "the original outside nine-grid adjustment works through real widget input");
    auto* mark = canvas->findChild<QToolButton*>(QStringLiteral("capture.cap.mark"));
    auto* scroll = canvas->findChild<QToolButton*>(QStringLiteral("capture.cap.long"));
    require(mark && mark->isEnabled() && mark->width() == 44 && mark->height() == 44 && mark->iconSize() == QSize(24, 24),
        "the shared capture toolbar keeps the original 44/24 logical-pixel controls");
    require(scroll && !scroll->isEnabled(), "an unconnected live service cannot present a working scrolling action");
    save(canvas->grab().toImage(), directory, QStringLiteral("capture-adjusting.png"));
    std::unique_ptr<ui::EditorWindow> editor;
    QObject::connect(&session, &Capture::CaptureSession::actionRequested, &window,
        [&](const Capture::ActionRequest& request) {
            require(window.dialogParent() == canvas && request.action == Capture::Action::Edit,
                "mark dispatches the direct editor intent from the final session and real view");
            editor = std::make_unique<ui::EditorWindow>();
            require(!editor->loadImage(request.image), "the actual shared editor accepts the selected image");
            editor->setCloseOnSuccessfulOutput(true); editor->show();
            require(editor->canvas()->originalImage() == pixels.image.copy(60, 70, 400, 210), "direct annotation keeps every actual cropped pixel");
            for (const auto tool : ui::editorTools) {
                editor->canvas()->setTool(tool); require(editor->canvas()->tool() == tool, "all nine existing editor tools are available immediately");
            }
            session.complete(request.token, Capture::ActionResult::Completed);
        });
    mark->click(); waitFor([&] { return session.stage() == Capture::Stage::Closed; });
    require(editor && editor->isVisible() && (!canvas || !canvas->isVisible()),
        "capture closes only after the real shared editor has accepted the image");
    // The finished event stack may still own the old toolbar. Observe its real
    // deferred destruction instead of comparing addresses an allocator may reuse.
    waitFor([&] { return canvas.isNull(); });
    require(!window.showSnapshot(source(2)), "the same application-owned capture presentation can open a new source generation");
    auto* next = imageView(window, pixels.image);
    require(next && session.snapshot().generation == 2 && next->isVisible(), "a later capture binds a fresh real view and source generation");
    window.cancel(); editor->close();
    std::cout << "PASS real Qt selection, outside adjustment, 44/24 toolbar, direct nine-tool editor and reusable capture presentation\n";
}

void retryKeysAndWrapping(const QString& directory)
{
    Capture::CaptureSession session;
    for (const auto action : {Capture::Action::Clipboard, Capture::Action::SaveAs, Capture::Action::Hex, Capture::Action::Position}) enable(session, action);
    ui::CaptureWindow window(session); require(!window.showSnapshot(source()), "open an independent shortcut/failed-output image");
    QPointer<QWidget> canvas(imageView(window, session.snapshot().image));
    std::vector<Capture::ActionRequest> requests;
    QObject::connect(&session, &Capture::CaptureSession::actionRequested, &window,
        [&](const Capture::ActionRequest& request) { requests.push_back(request); });
    key(*canvas, Qt::Key_Right); require(session.cursor() == QPoint(121, 110), "arrow nudges exactly one source pixel");
    key(*canvas, Qt::Key_P, Qt::ControlModifier);
    require(requests.size() == 1 && requests.back().text == QStringLiteral("source(121,110)"), "image-source position cannot masquerade as a desktop position");
    session.complete(requests.back().token, Capture::ActionResult::Failed, QStringLiteral("The desktop did not accept the copy"));
    require(session.stage() == Capture::Stage::Selecting, "failed text publication retains image sampling");
    drag(*canvas, {50, 50}, {500, 310});
    key(*canvas, Qt::Key_C, Qt::ControlModifier);
    require(requests.size() == 1, "the capture adjustment keeps original Ctrl+C stage semantics");
    key(*canvas, Qt::Key_Return); waitFor([&] { return requests.size() == 2; });
    require(requests.back().action == Capture::Action::Clipboard, "Enter adjustment copies through the same real output request");
    session.complete(requests.back().token, Capture::ActionResult::Failed, QStringLiteral("Copy was rejected. The selected image is still available."));
    events(); require(session.stage() == Capture::Stage::Adjusting && !session.failureText().isEmpty(), "a failed host output preserves the exact editable selection");
    canvas->resize(320, 330); events();
    auto* toolbar = canvas->findChild<QWidget*>(QStringLiteral("captureToolbar"));
    require(toolbar && toolbar->isVisible() && toolbar->height() >= 90 && toolbar->geometry().left() >= 0 &&
        toolbar->geometry().right() < canvas->width(), "narrow image views wrap complete controls within the real available width");
    for (auto* button : toolbar->findChildren<QToolButton*>()) require(button->size() == QSize(44, 44), "wrapping retains every complete click target");
    save(canvas->grab().toImage(), directory, QStringLiteral("capture-narrow-failure.png"));
    const auto oldBounds = session.selection().bounds();
    auto* saveButton = canvas->findChild<QToolButton*>(QStringLiteral("capture.tool.save"));
    saveButton->click(); waitFor([&] { return requests.size() == 3; });
    session.complete(requests.back().token, Capture::ActionResult::Cancelled);
    const auto& retained = session.selection().bounds();
    require(session.stage() == Capture::Stage::Adjusting && retained.left == oldBounds.left && retained.top == oldBounds.top &&
        retained.right == oldBounds.right && retained.bottom == oldBounds.bottom, "cancelled Save As retains selection and source");
    key(*canvas, Qt::Key_Escape); require(session.stage() == Capture::Stage::Closed, "Escape closes through the shared session");
    std::cout << "PASS actual shortcut dispatch, failed/cancelled output retention and narrow-toolbar geometry\n";
}

void hostLifetime()
{
    auto* session = new Capture::CaptureSession;
    enable(*session, Capture::Action::Edit);
    auto* window = new ui::CaptureWindow(*session); QPointer<ui::CaptureWindow> alive(window);
    require(!window->showSnapshot(source()), "open real lifetime capture view");
    QPointer<QWidget> canvas(imageView(*window, session->snapshot().image));
    drag(*canvas, {80, 80}, {300, 250});
    QObject::connect(session, &Capture::CaptureSession::actionRequested, window, [window, session](const Capture::ActionRequest&) {
        delete window; delete session;
    });
    canvas->findChild<QToolButton*>(QStringLiteral("capture.cap.mark"))->click();
    waitFor([&] { return alive.isNull(); });
    require(canvas.isNull(), "destroying the host also destroys its actual capture widget");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::cout << "PASS actual shared-editor handoff may destroy its capture owner\n";
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); QApplication::setStyle(QStringLiteral("Fusion"));
    try {
        const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
        if (!directory.isEmpty()) require(QDir().mkpath(directory), "create real rendering artifact folder");
        realSelectionAndEditor(directory); retryKeysAndWrapping(directory); hostLifetime(); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
