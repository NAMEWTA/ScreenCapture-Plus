#include "capture/ScrollCaptureSession.h"
#include "services/capture/LiveCaptureSource.h"
#include "services/capture/ScreenCaptureService.h"
#include "ui/qt/ScrollWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dwmapi.h>
#endif

namespace cap = HuhuCapture::Capture;
namespace ui = HuhuCapture::UI::Qt;
namespace core = HuhuCapture::Core;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void events()
{
    QCoreApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void waitFor(const std::function<bool()>& ready, const char* message)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!ready() && elapsed.elapsed() < 15000) { events(); QThread::msleep(1); }
    require(ready(), message);
}
void key(QWidget& widget, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, value, modifiers);
    QApplication::sendEvent(&widget, &event);
}

void unavailableAndLifetime()
{
    require(QGuiApplication::platformName() == QStringLiteral("offscreen") ||
        QGuiApplication::platformName() == QStringLiteral("minimal"), "The negative test uses an actual unsupported QPA");
    cap::ScreenCaptureService native;
    cap::LiveCaptureSource live;
    cap::ScrollCaptureSession session;
    cap::ActionRequest request;
    request.action = cap::Action::Scroll;
    cap::Snapshot snapshot;
    core::LiveSourceConsumer absent;
    require(session.prepareNative(request, snapshot, native).code == cap::Failure::Code::SourceChanged,
        "An absent native frame cannot admit scrolling or invent desktop coordinates");
    require(session.prepareLive(request, live, absent).code == cap::Failure::Code::SourceChanged &&
        !absent.firstFrame && !absent.frames, "An absent authorized first frame cannot become a live consumer");
    require(session.start() == 0 && !session.hasCompleteImage() && session.image().isNull(),
        "No placeholder image or worker is produced for a rejected source");

    auto* window = new ui::ScrollWindow(session);
    QPointer<ui::ScrollWindow> guarded(window);
    unsigned outputs{}, terminals{};
    bool shutdown{};
    QObject::connect(window, &ui::ScrollWindow::outputRequested, &session, [&](quint64, ui::OutputIntent, const QImage&) { ++outputs; });
    QObject::connect(&session, &cap::ScrollCaptureSession::shutdownFinished, &session, [&] { shutdown = true; });
    QObject::connect(window, &ui::ScrollWindow::closed, &session, [&] { ++terminals; delete window; });
    require(window->open().code == cap::Failure::Code::Busy, "A source-less view rejects presentation instead of displaying success");
    for (const auto* button : window->findChildren<QToolButton*>()) {
        if (button->objectName() == QStringLiteral("tool.pin") || button->objectName() == QStringLiteral("tool.save") ||
            button->objectName() == QStringLiteral("tool.clipboard"))
            require(!button->isEnabled(), "Output controls require a real complete captured image");
    }
    require(window->findChild<QLabel*>(QStringLiteral("scrollStatus"))->font().pixelSize() == 12,
        "Status typography uses the same explicit logical-pixel size on both platforms");
    key(*window, Qt::Key_C, Qt::ControlModifier); key(*window, Qt::Key_S, Qt::ControlModifier);
    key(*window, Qt::Key_Return);
    require(outputs == 0, "Keyboard shortcuts never publish an absent scrolling result");
    key(*window, Qt::Key_Escape);
    require(!shutdown && guarded, "Closing waits for the session's queued drain notification");
    waitFor([&] { return !guarded; }, "A synchronous closed observer may destroy the actual shared widget");
    require(shutdown && terminals == 1 && session.phase() == cap::ScrollPhase::Closed && !session.busy(),
        "The view closes exactly once after the real session has drained");
    require(session.start() == 0 && session.prepareNative(request, snapshot, native).code == cap::Failure::Code::Busy,
        "Shutdown permanently closes admission on that session");
    std::cout << "PASS unavailable source, real widgets/shortcuts, exact shutdown ordering and reentrant deletion\n";
}

// Generated fixture is painted by a real native QWidget and scrolled only by
// the production Win32 SendInput/XTest adapter. It is not a fake frame provider.
// Physical row/column colors make every retained output pixel independently
// comparable to the displayed page without duplicating the stitch algorithm.
class Page final : public QWidget {
public:
    QImage pixels;
    int offset{}, wheels{}, paints{}, contentPaints{};
    bool nativeWheel{};
    qreal ratio{1};
    Page() { setWindowTitle(QStringLiteral("huhucapture real scrolling page")); resize(320, 240); }
    void initializePixels()
    {
        ratio = devicePixelRatioF();
        pixels = QImage(qRound(width() * ratio), qRound(1600 * ratio), QImage::Format_ARGB32);
        for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x)
            pixels.setPixel(x, y, qRgb((y * 37 + x * 31) % 251, (y * 43 + x * 53) % 251, (y * 71 + x * 19) % 251));
        pixels.setDevicePixelRatio(ratio); repaint();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.drawImage(QPointF(0, -offset), pixels);
        ++paints; if (!pixels.isNull()) ++contentPaints;
    }
    void wheelEvent(QWheelEvent* event) override
    {
        if (event->angleDelta().y() < 0) offset += 24;
        else if (event->angleDelta().y() > 0) offset = std::max(0, offset - 24);
        nativeWheel = nativeWheel || event->spontaneous(); ++wheels; update(); event->accept();
    }
};

QJsonArray point(QPointF value) { return {value.x(), value.y()}; }
QJsonArray rectangle(QRectF value) { return {value.x(), value.y(), value.width(), value.height()}; }
QJsonObject imageInfo(const QImage& value)
{
    return {{QStringLiteral("size"), QJsonArray{value.width(), value.height()}},
        {QStringLiteral("format"), static_cast<int>(value.format())},
        {QStringLiteral("ratio"), value.devicePixelRatio()},
        {QStringLiteral("dotsPerMeter"), QJsonArray{value.dotsPerMeterX(), value.dotsPerMeterY()}}};
}
#ifdef Q_OS_WIN
QString handle(HWND value) { return QString::number(reinterpret_cast<quintptr>(value), 16); }
QJsonArray nativeRectangle(const RECT& value)
{ return {static_cast<int>(value.left), static_cast<int>(value.top), static_cast<int>(value.right - value.left), static_cast<int>(value.bottom - value.top)}; }
QJsonObject nativeWindow(HWND value)
{
    DWORD process{}; const auto thread = GetWindowThreadProcessId(value, &process);
    QJsonObject result{{QStringLiteral("handle"), handle(value)},
        {QStringLiteral("process"), static_cast<qint64>(process)},
        {QStringLiteral("thread"), static_cast<qint64>(thread)},
        {QStringLiteral("visible"), bool(IsWindowVisible(value))},
        {QStringLiteral("minimized"), bool(IsIconic(value))},
        {QStringLiteral("dpi"), static_cast<int>(GetDpiForWindow(value))}};
    RECT bounds{};
    if (GetWindowRect(value, &bounds)) result.insert(QStringLiteral("window"), nativeRectangle(bounds));
    if (GetClientRect(value, &bounds)) result.insert(QStringLiteral("client"), nativeRectangle(bounds));
    POINT origin{};
    if (ClientToScreen(value, &origin)) result.insert(QStringLiteral("clientOrigin"), QJsonArray{static_cast<int>(origin.x), static_cast<int>(origin.y)});
    wchar_t className[256]{};
    if (GetClassNameW(value, className, static_cast<int>(std::size(className))))
        result.insert(QStringLiteral("class"), QString::fromWCharArray(className));
    DWORD cloaked{};
    if (SUCCEEDED(DwmGetWindowAttribute(value, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        result.insert(QStringLiteral("cloaked"), static_cast<qint64>(cloaked));
    if (SUCCEEDED(DwmGetWindowAttribute(value, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
        result.insert(QStringLiteral("extendedFrame"), nativeRectangle(bounds));
    return result;
}
#endif
QJsonObject presentationState(Page& page, std::optional<QRect> selectedDesktop = {})
{
    const auto* window = page.windowHandle();
    QJsonObject result{{QStringLiteral("paints"), page.paints}, {QStringLiteral("contentPaints"), page.contentPaints},
        {QStringLiteral("offset"), page.offset}, {QStringLiteral("ratio"), page.ratio},
        {QStringLiteral("visible"), page.isVisible()}, {QStringLiteral("exposed"), window && window->isExposed()},
        {QStringLiteral("qtActive"), page.isActiveWindow()},
        {QStringLiteral("qtFocusWindowMatches"), window == QGuiApplication::focusWindow()},
        {QStringLiteral("widgetGeometry"), rectangle(page.geometry())},
        {QStringLiteral("frameGeometry"), rectangle(page.frameGeometry())},
        {QStringLiteral("globalOrigin"), point(page.mapToGlobal(QPoint()))},
        {QStringLiteral("fixture"), imageInfo(page.pixels)}};
#ifdef Q_OS_WIN
    result.insert(QStringLiteral("nativePage"), nativeWindow(reinterpret_cast<HWND>(page.winId())));
    result.insert(QStringLiteral("foreground"), nativeWindow(GetForegroundWindow()));
    result.insert(QStringLiteral("threadFocus"), handle(GetFocus()));
    DWM_TIMING_INFO timing{}; timing.cbSize = sizeof(timing);
    const auto status = DwmGetCompositionTimingInfo(nullptr, &timing);
    result.insert(QStringLiteral("dwmTimingStatus"), static_cast<qint64>(status));
    if (SUCCEEDED(status)) {
        result.insert(QStringLiteral("dwmFrame"), QString::number(timing.cFrame));
        result.insert(QStringLiteral("dwmVBlank"), QString::number(timing.qpcVBlank));
    }
    if (selectedDesktop) {
        QJsonArray owners;
        for (const auto sample : {selectedDesktop->topLeft(), selectedDesktop->center(), selectedDesktop->bottomRight()}) {
            const POINT actual{sample.x(), sample.y()};
            owners.append(QJsonObject{{QStringLiteral("point"), point(sample)},
                {QStringLiteral("window"), nativeWindow(WindowFromPoint(actual))}});
        }
        result.insert(QStringLiteral("selectedPointOwners"), owners);
    }
#else
    Q_UNUSED(selectedDesktop);
#endif
    return result;
}
QJsonObject pixelComparison(const QImage& actual, const QImage& expected)
{
    qint64 mismatches{}; QPoint first(-1, -1); QRect bounds;
    if (actual.size() == expected.size()) {
        for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x)
            if (actual.pixel(x, y) != expected.pixel(x, y)) {
                if (!mismatches) { first = {x, y}; bounds = {first, QSize(1, 1)}; }
                else bounds = bounds.united({QPoint(x, y), QSize(1, 1)});
                ++mismatches;
            }
    } else mismatches = -1;
    QJsonObject result{{QStringLiteral("equal"), actual == expected},
        {QStringLiteral("mismatchedPixels"), mismatches}, {QStringLiteral("firstMismatch"), point(first)},
        {QStringLiteral("mismatchBounds"), rectangle(bounds)},
        {QStringLiteral("actual"), imageInfo(actual)}, {QStringLiteral("expected"), imageInfo(expected)}};
    if (first.x() >= 0) {
        result.insert(QStringLiteral("actualFirstRgba"), QString::number(actual.pixel(first), 16));
        result.insert(QStringLiteral("expectedFirstRgba"), QString::number(expected.pixel(first), 16));
    }
    return result;
}
void saveReadback(const QString& directory, const QString& prefix, const cap::Snapshot& snapshot,
    const QImage& actual, const QImage& expected)
{
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "Create the actual native readback evidence directory");
    require(snapshot.image.save(QDir(directory).filePath(prefix + QStringLiteral("-desktop.png"))) &&
        actual.save(QDir(directory).filePath(prefix + QStringLiteral("-actual.png"))) &&
        expected.save(QDir(directory).filePath(prefix + QStringLiteral("-expected.png"))),
        "Preserve actual desktop, selected pixels and independent expectation before any pixel assertion");
}
void saveEvidence(const QString& directory, const QJsonObject& evidence)
{
    std::cout << "PRESENTATION " << QJsonDocument(evidence).toJson(QJsonDocument::Compact).toStdString() << '\n';
    if (directory.isEmpty()) return;
    QFile output(QDir(directory).filePath(QStringLiteral("scroll-initial-presentation.json")));
    const auto bytes = QJsonDocument(evidence).toJson(QJsonDocument::Indented);
    require(output.open(QIODevice::WriteOnly | QIODevice::Truncate) && output.write(bytes) == bytes.size() && output.flush(),
        "Retain real presentation coordinates and native ownership evidence even on a mismatch");
}
void actualNative(const QString& artifacts)
{
    require(QGuiApplication::platformName() == QStringLiteral("xcb") || QGuiApplication::platformName() == QStringLiteral("windows"),
        "The positive probe requires an actual X11 or Windows desktop");
    Page page;
    page.move(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(70, 70));
    page.show(); page.raise(); page.activateWindow();
    waitFor([&] { return page.windowHandle() && page.windowHandle()->isExposed(); }, "Expose the real scrolling page");
    page.initializePixels();
    waitFor([&] { return page.contentPaints != 0; }, "Paint actual fixture content, excluding the initial empty-widget paint");
    cap::ScreenCaptureService source;
    bool initialized{}, stopped{}, moved{};
    cap::Snapshot snapshot; cap::Failure sourceFailure;
    QObject::connect(&source, &cap::ScreenCaptureService::initialized, &source, [&](const auto&, const auto& failure) {
        sourceFailure = failure; initialized = true;
    });
    QObject::connect(&source, &cap::ScreenCaptureService::finished, &source, [&](quint64, const auto& value, const auto& failure) {
        snapshot = value; sourceFailure = failure;
    });
    QObject::connect(&source, &cap::ScreenCaptureService::shutdownFinished, &source, [&] { stopped = true; });
    source.initialize(); waitFor([&] { return initialized; }, "Initialize the actual screenshot/input backend");
    if (sourceFailure) std::cerr << "SOURCE " << sourceFailure.detail.toStdString() << '\n';
    require(!sourceFailure && source.capabilities().automaticScroll.canRequest(), "The actual desktop must advertise real automatic input");
    const QRect logical(20, 20, 220, 180);
    QPoint contentOrigin; QRect selected; QImage actualFirst, expectedFirst;
    QJsonObject firstEvidence, latestEvidence; QJsonArray attempts;
    unsigned count{}; QElapsedTimer presentation; presentation.start();
    // Reuse screen_capture_tests' real-readback presentation condition. Qt's
    // paint/expose notification is not a compositor presentation receipt. A
    // fixed delay or an arbitrary number of discarded frames cannot fix that.
    do {
        const auto before = presentationState(page);
        snapshot = {}; sourceFailure = {};
        require(source.request() != 0, "Request actual native desktop pixels");
        waitFor([&] { return !snapshot.image.isNull() || sourceFailure; }, "Capture the actual visible page");
        require(!sourceFailure && source.isCurrent(snapshot.sourceId, snapshot.generation), "The captured page keeps a verified current identity");
        require(snapshot.coordinates == cap::CoordinateSpace::DesktopPixels && snapshot.desktopOrigin,
            "The native fixture has a verified physical desktop origin");
        const cap::ScreenBinding* binding{};
        for (const auto& candidate : snapshot.screens) if (candidate.screen == page.windowHandle()->screen()) { binding = &candidate; break; }
        require(binding != nullptr, "Map through the page's actual QScreen binding");
        require(std::abs(binding->pixelsPerDipX - page.ratio) < .01 && std::abs(binding->pixelsPerDipY - page.ratio) < .01,
            "The native fixture uses the verified physical-pixel scale of its own screen");
        const auto pageOrigin = cap::sourceFromScreenLocal(*binding, QPointF(page.mapToGlobal(QPoint())) - binding->logicalGeometry.topLeft());
        const QPoint first(qRound(pageOrigin.x() + logical.x() * page.ratio), qRound(pageOrigin.y() + logical.y() * page.ratio));
        selected = {first, QSize(qRound(logical.width() * page.ratio), qRound(logical.height() * page.ratio))};
        contentOrigin = {first.x() - qRound(pageOrigin.x()), first.y() - qRound(pageOrigin.y())};
        expectedFirst = page.pixels.copy(QRect(contentOrigin, selected.size())); expectedFirst.setDevicePixelRatio(1);
        actualFirst = snapshot.image.copy(selected).convertToFormat(QImage::Format_ARGB32);
        const auto compared = pixelComparison(actualFirst, expectedFirst);
        const auto after = presentationState(page, selected.translated(*snapshot.desktopOrigin));
        latestEvidence = {{QStringLiteral("attempt"), static_cast<int>(++count)},
            {QStringLiteral("elapsedMs"), presentation.elapsed()}, {QStringLiteral("beforeRequest"), before},
            {QStringLiteral("afterReadback"), after}, {QStringLiteral("comparison"), compared},
            {QStringLiteral("sourceId"), QString::number(snapshot.sourceId)}, {QStringLiteral("generation"), QString::number(snapshot.generation)},
            {QStringLiteral("desktopOrigin"), point(*snapshot.desktopOrigin)}, {QStringLiteral("pageSourceOrigin"), point(pageOrigin)},
            {QStringLiteral("selectedSourcePixels"), rectangle(selected)}, {QStringLiteral("fixtureOriginPixels"), point(contentOrigin)},
            {QStringLiteral("bindingLogicalGeometry"), rectangle(binding->logicalGeometry)},
            {QStringLiteral("bindingSourcePixels"), rectangle(binding->sourcePixels)},
            {QStringLiteral("bindingScale"), QJsonArray{binding->pixelsPerDipX, binding->pixelsPerDipY}}};
        const QJsonObject summary{{QStringLiteral("attempt"), static_cast<int>(count)}, {QStringLiteral("elapsedMs"), presentation.elapsed()},
            {QStringLiteral("comparison"), compared}, {QStringLiteral("foreground"), after.value(QStringLiteral("foreground"))}};
        if (attempts.size() < 64) attempts.append(summary);
        else attempts.replace(attempts.size() - 1, summary); // bounded diagnostic history, with the actual last attempt
        if (count == 1) {
            firstEvidence = latestEvidence;
            saveReadback(artifacts, QStringLiteral("scroll-initial-first"), snapshot, actualFirst, expectedFirst);
        }
        if (actualFirst == expectedFirst) break;
        events();
    } while (presentation.elapsed() < 5000);
    saveReadback(artifacts, QStringLiteral("scroll-initial-final"), snapshot, actualFirst, expectedFirst);
    saveEvidence(artifacts, {{QStringLiteral("qpa"), QGuiApplication::platformName()},
        {QStringLiteral("attemptCount"), static_cast<int>(count)}, {QStringLiteral("retainedAttempts"), attempts},
        {QStringLiteral("first"), firstEvidence}, {QStringLiteral("last"), latestEvidence}});
    require(actualFirst == expectedFirst,
        "The native capture equals pixels actually painted by the independent page fixture");
    QObject::connect(&source, &cap::ScreenCaptureService::cursorMoved, &source,
        [&](quint64, quint64, QPoint requested, QPoint actual, const cap::Failure& error) {
            require(!error && requested == actual, "Place the real pointer inside the selected scrolling target"); moved = true;
        });
    source.moveCursor(snapshot.sourceId, snapshot.generation, selected.center());
    waitFor([&] { return moved; }, "The OS confirms the actual pointer position");
    cap::ActionRequest request;
    request.token = 1; request.action = cap::Action::Scroll; request.sourceId = snapshot.sourceId; request.generation = snapshot.generation;
    request.coordinates = snapshot.coordinates; request.sourceRect = selected; request.desktopRect = selected.translated(*snapshot.desktopOrigin);
    request.image = snapshot.image.copy(selected);
    cap::ScrollCaptureSession session;
    require(!session.prepareNative(request, snapshot, source), "Prepare the actual source through the production session");
    ui::ScrollWindow window(session);
    require(!window.open(), "Open the same production scrolling controls and start surfaces");
    window.activate();
    unsigned outputs{}, completed{}; bool closed{};
    QImage output;
    QObject::connect(&session, &cap::ScrollCaptureSession::previewChanged, &session, [&](const cap::ScrollProgress& progress) {
        if (progress.imageSize.height() >= selected.height() + qRound(48 * page.ratio)) session.stop();
    });
    QObject::connect(&session, &cap::ScrollCaptureSession::finished, &session, [&](quint64, const cap::ScrollCompletion& result) {
        if (result.failure) std::cerr << "SCROLL " << result.failure.detail.toStdString() << '\n';
        require(session.prepareNative(request, snapshot, source).code == cap::Failure::Code::Busy,
            "A synchronous terminal observer cannot replace a result while its callbacks are still being delivered");
        ++completed;
    });
    QObject::connect(&window, &ui::ScrollWindow::outputRequested, &window,
        [&](quint64 token, ui::OutputIntent intent, const QImage& image) {
            ++outputs; output = image;
            require(intent == ui::OutputIntent::SaveAs, "Ctrl+S preserves the mature Save As intent");
            window.completeOutput(token, ui::OutputResult::Cancelled);
        });
    QObject::connect(&window, &ui::ScrollWindow::closed, &window, [&] { closed = true; });
    QWidget* startSurface{};
    const auto global = page.mapToGlobal(logical.center());
    for (auto* candidate : QApplication::topLevelWidgets())
        if (candidate->objectName() == QStringLiteral("scrollStartSurface") && candidate->geometry().contains(global)) { startSurface = candidate; break; }
    require(startSurface, "The native region has the shared real start surface");
    const QPointF local(startSurface->mapFromGlobal(global));
    QMouseEvent move(QEvent::MouseMove, local, QPointF(global), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(startSurface, &move);
    // A component click enters the real native pipeline. Only subsequent OS
    // wheel events can change the page; their spontaneous flag is asserted.
    QMouseEvent release(QEvent::MouseButtonRelease, local, QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(startSurface, &release);
    waitFor([&] { return completed != 0; }, "Real automatic scrolling appends and stops its actual native worker");
    require(!session.completion().failure && page.wheels >= 2 && page.nativeWheel,
        "Real SendInput/XTest wheel events reach the target and produce complete frames");
    require(session.hasCompleteImage() && session.image().height() >= selected.height() + qRound(48 * page.ratio),
        "The complete scrolling output contains actual appended rows");
    auto expected = page.pixels.copy(QRect(contentOrigin, session.image().size())); expected.setDevicePixelRatio(1);
    require(session.image() == expected, "Every accumulated output pixel equals the independent real displayed page");
    key(window, Qt::Key_S, Qt::ControlModifier);
    require(outputs == 1 && output == expected && session.image() == expected && !closed,
        "Cancelled Save As retains the exact complete image for another output");
    if (!artifacts.isEmpty()) {
        QDir().mkpath(artifacts);
        require(window.grab().save(artifacts + QStringLiteral("/scroll-window.png")), "Save the actual shared controls for visual review");
        require(output.save(artifacts + QStringLiteral("/scroll-complete.png")), "Save the actual stitched result for review");
    }
    window.close(); waitFor([&] { return closed; }, "Closing waits for the actual session shutdown");
    require(!session.busy() && session.image() == expected, "Shutdown never invalidates a retained complete output image");
    source.shutdown(); waitFor([&] { return stopped; }, "The native source owner drains after scrolling");
    std::cout << "PASS real native input/capture, exact page pixels, shared start/control UI, cancelled output and ordered drain\n";
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try {
        if (app.arguments().contains(QStringLiteral("--unavailable"))) unavailableAndLifetime();
        else actualNative(app.arguments().size() > 1 ? app.arguments().at(1) : QString());
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
