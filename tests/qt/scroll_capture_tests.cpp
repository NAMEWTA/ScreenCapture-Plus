#include "app/ScrollController.h"
#include "app/MediaWorkspace.h"
#include "services/desktop/SystemClipboard.h"
#include "services/desktop/FileRevealer.h"
#include "services/capture/DesktopCapture.h"

#include <QApplication>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QCursor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QTemporaryDir>
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
namespace application = HuhuCapture::Application;
namespace desktop = HuhuCapture::Desktop;
namespace media = HuhuCapture::Media;
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
    QTemporaryDir temporary; require(temporary.isValid(), "Create an isolated archive for rejected scroll requests");
    auto archive = std::make_shared<media::MediaArchiveService>(temporary.path() + QStringLiteral("/archive"));
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    application::MediaWorkspace workspace(archive, clipboard, revealer);
    QPointer<application::ScrollController> controller = new application::ScrollController(archive, workspace);
    cap::ActionRequest request; request.action = cap::Action::Scroll;
    require(!controller->begin(request) && !controller->busy(),
        "An absent image cannot admit scrolling or invent an authorized frame");
    request.image = QImage(20, 20, QImage::Format_ARGB32); request.image.fill(Qt::red);
    request.coordinates = cap::CoordinateSpace::DesktopPixels; request.sourceRect = request.image.rect();
    require(!controller->begin(request) && !controller->busy(),
        "An image without verified native bindings cannot invent physical desktop coordinates");
    controller->setExternalStopAvailable(true, QStringLiteral("available host finish action"));
    require(!controller->finish() && !controller->busy(),
        "An external stop-route capability cannot invent an active capture");
    controller->setExternalStopAvailable(false); controller->activate();
    unsigned terminals{}, outputs{};
    QObject::connect(controller, &application::ScrollController::archiveChanged, &workspace, [&] { ++outputs; });
    QObject::connect(controller, &application::ScrollController::shutdownFinished, &workspace, [&] {
        ++terminals;
        require(!controller->busy() && !controller->begin(request) && !controller->finish(),
            "Shutdown closes admission before reporting that the actual controller drained");
        delete controller.data();
    });
    controller->shutdown();
    waitFor([&] { return !controller; }, "A synchronous terminal observer may destroy the actual shared controller");
    const auto listed = archive->list();
    require(terminals == 1 && outputs == 0 && !listed.failure && listed.entries.empty() && !workspace.busy(),
        "Rejected requests create no output, placeholder view or worker and report shutdown exactly once");
    std::cout << "PASS actual controller rejects absent/unbound sources, closes admission and tolerates synchronous shutdown deletion\n";
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
        pixels.setColorSpace(QColorSpace::SRgb);
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
    const auto colorSpace = value.colorSpace();
    return {{QStringLiteral("size"), QJsonArray{value.width(), value.height()}},
        {QStringLiteral("format"), static_cast<int>(value.format())},
        {QStringLiteral("ratio"), value.devicePixelRatio()},
        {QStringLiteral("colorSpace"), QJsonObject{
            {QStringLiteral("valid"), colorSpace.isValid()},
            {QStringLiteral("description"), colorSpace.description()},
            {QStringLiteral("iccSha256"), QString::fromLatin1(QCryptographicHash::hash(
                colorSpace.iccProfile(), QCryptographicHash::Sha256).toHex())}}},
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
    cap::DesktopCapture source;
    bool initialized{}, stopped{};
    cap::Snapshot snapshot; cap::Failure sourceFailure;
    QObject::connect(&source, &cap::DesktopCapture::initialized, &source, [&](const auto& capability) {
        if (!capability.canRequest()) sourceFailure = {cap::Failure::Code::Unavailable, 0, capability.reason};
        initialized = true;
    });
    QObject::connect(&source, &cap::DesktopCapture::finished, &source, [&](quint64, const auto& value, const auto& failure) {
        snapshot = value; sourceFailure = failure;
    });
    QObject::connect(&source, &cap::DesktopCapture::shutdownFinished, &source, [&] { stopped = true; });
    source.initialize(); waitFor([&] { return initialized; }, "Initialize the actual screenshot/input backend");
    if (sourceFailure) std::cerr << "SOURCE " << sourceFailure.detail.toStdString() << '\n';
    require(!sourceFailure && source.capability().state == cap::CapabilityState::Available, "The native desktop must provide actual screenshot pixels");
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
        const auto captureToken = source.request();
        require(captureToken != 0, "Request actual native desktop pixels");
        waitFor([&] { return !snapshot.image.isNull() || sourceFailure; }, "Capture the actual visible page");
        require(!sourceFailure && snapshot.sourceId == captureToken && snapshot.generation == captureToken,
            "The captured page preserves the actual accepted request identity and verified native bindings");
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
    // This prepares the independent real page fixture. The production scroll
    // adapter itself never warps the pointer or synthesizes captured pixels.
    const auto pointerTarget = page.mapToGlobal(logical.center());
    QCursor::setPos(pointerTarget);
    waitFor([&] { return QCursor::pos() == pointerTarget; }, "The actual OS pointer reaches the selected scrolling page");
    snapshot = {}; sourceFailure = {};
    const auto cursorCapture = source.request(); require(cursorCapture != 0, "Read back the actual cursor with the desktop pixels");
    waitFor([&] { return !snapshot.image.isNull() || sourceFailure; }, "Verify the actual pointer in native source coordinates");
    require(!sourceFailure && snapshot.sourceId == cursorCapture && snapshot.coordinates == cap::CoordinateSpace::DesktopPixels &&
        snapshot.desktopOrigin && selected.contains(snapshot.cursor), "The native backend confirms the real pointer inside the selected target");
    cap::ActionRequest request;
    request.token = 1; request.action = cap::Action::Scroll; request.sourceId = snapshot.sourceId; request.generation = snapshot.generation;
    request.coordinates = snapshot.coordinates; request.sourceRect = selected; request.desktopRect = selected.translated(*snapshot.desktopOrigin);
    request.image = snapshot.image.copy(selected);
    request.screens = snapshot.screens;
    QTemporaryDir temporary; require(temporary.isValid(), "Create the actual scrolling output workspace");
    auto archive = std::make_shared<media::MediaArchiveService>(temporary.path() + QStringLiteral("/archive"));
    desktop::SystemClipboard clipboard; desktop::FileRevealer revealer;
    application::MediaWorkspace workspace(archive, clipboard, revealer);
    application::ScrollController controller(archive, workspace);
    QString failure; unsigned archived{}, busyAdmissions{};
    QObject::connect(&controller, &application::ScrollController::operationFailed, &controller, [&](const QString& value) {
        failure = value; std::cerr << "SCROLL " << value.toStdString() << '\n';
    });
    QObject::connect(&controller, &application::ScrollController::archiveChanged, &controller, [&] { ++archived; });
    QObject::connect(&controller, &application::ScrollController::busyChanged, &controller, [&](bool busy) {
        if (busy) {
            ++busyAdmissions;
            require(!controller.begin(request), "A synchronous busy observer cannot replace the active scrolling journey");
        }
    });
    require(controller.begin(request, &page), "Prepare and present the actual application-owned scrolling controller");
    QPointer<QWidget> window;
    for (auto* candidate : QApplication::topLevelWidgets())
        if (candidate->objectName() == QStringLiteral("scrollCaptureWindow")) { window = candidate; break; }
    require(window && busyAdmissions == 1, "The single production controller owns its actual preview and controls");
    auto* start = window->findChild<QToolButton*>(QStringLiteral("scrollStart"));
    auto* stop = window->findChild<QToolButton*>(QStringLiteral("scrollStop"));
    auto* save = window->findChild<QToolButton*>(QStringLiteral("scrollSave"));
    auto* dimensions = window->findChild<QLabel*>(QStringLiteral("scrollDimensions"));
    require(start && stop && save && dimensions && start->isEnabled() && !stop->isEnabled(),
        "Only the prepared real source enables the production start control");
    require(window->findChild<QLabel*>(QStringLiteral("scrollStatus"))->font().pixelSize() == 12,
        "Status typography uses explicit logical pixels on both platforms");
    for (const auto* button : {start, stop, save})
        require(button->height() == 44 && button->iconSize() == QSize(24, 24),
            "The production scrolling controls retain the shared 44-DIP buttons and 24-DIP icons");
    const auto* actions = window->findChild<QWidget*>(QStringLiteral("scrollOutputActions"));
    require(actions, "The actual scrolling output strip is present");
    const auto buttons = actions->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
    require(buttons.size() == 4 && buttons[0]->objectName() == QStringLiteral("scrollPin") &&
        buttons[1]->objectName() == QStringLiteral("scrollClose") && buttons[2]->objectName() == QStringLiteral("scrollSave") &&
        buttons[3]->objectName() == QStringLiteral("scrollCopy"), "The output toolbar keeps pin, close, save and copy in the mature order");
    QWidget* startSurface{};
    const auto global = page.mapToGlobal(logical.center());
    for (auto* candidate : QApplication::topLevelWidgets())
        if (candidate->objectName() == QStringLiteral("scrollStartSurface") && candidate->geometry().contains(global)) { startSurface = candidate; break; }
    require(startSurface, "The native region keeps the production click-to-start surface");
    const QPointF local(startSurface->mapFromGlobal(global));
    QMouseEvent move(QEvent::MouseMove, local, QPointF(global), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(startSurface, &move);
    // Only this component click starts the real pipeline. All subsequent page
    // changes must arrive as spontaneous operating-system wheel input.
    QMouseEvent release(QEvent::MouseButtonRelease, local, QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(startSurface, &release);
    const auto displayedSize = [&] {
        const auto pieces = dimensions->text().split(QChar(u'×'));
        if (pieces.size() != 2) return QSize();
        bool widthOk{}, heightOk{};
        const int width = pieces.at(0).trimmed().toInt(&widthOk);
        auto heightText = pieces.at(1).trimmed();
        if (heightText.endsWith(QStringLiteral(" px"))) heightText.chop(3);
        const int height = heightText.toInt(&heightOk);
        return widthOk && heightOk ? QSize(width, height) : QSize();
    };
    waitFor([&] { return !failure.isEmpty() || displayedSize().height() >= selected.height() + qRound(48 * page.ratio); },
        "The actual automatic worker appends independently painted page rows");
    require(failure.isEmpty() && controller.finish(), "The host finish action stops the real active native worker");
    waitFor([&] { return !failure.isEmpty() || (window && start->isEnabled() && !stop->isEnabled()); },
        "The final controls restore only after the actual source and worker drain");
    require(failure.isEmpty() && page.wheels >= 2 && page.nativeWheel,
        "Real SendInput/XTest wheel events reach the selected target and produce complete frames");
    const auto completedSize = displayedSize();
    require(completedSize.width() == selected.width() && completedSize.height() >= selected.height() + qRound(48 * page.ratio),
        "The complete scrolling output contains actual appended rows");
    auto expected = page.pixels.copy(QRect(contentOrigin, completedSize)); expected.setDevicePixelRatio(1);
    require(!controller.begin(request) && !controller.finish(),
        "A completed journey retains its result and cannot be replaced or stopped a second time");
    controller.activate();
    waitFor([&] { return window && window->isActiveWindow(); }, "Activate the actual result view for its Ctrl+S shortcut");
    key(*window, Qt::Key_S, Qt::ControlModifier);
    waitFor([&] { return window && window->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog")); },
        "Ctrl+S opens the same real destination dialog as the application media workspace");
    auto* dialog = window->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    require(workspace.busy() && !save->isEnabled(), "The real output stays pending until the dialog completes");
    static_cast<QDialog*>(dialog)->reject();
    waitFor([&] { return window && save->isEnabled() && !workspace.busy(); }, "A real cancelled Save As returns to the retained result");
    require(archived == 0 && controller.busy() && archive->list().entries.empty() && displayedSize() == completedSize,
        "Cancelling Save As publishes nothing and retains the complete result for another output");
    if (!artifacts.isEmpty()) {
        require(QDir().mkpath(artifacts) && window->grab().save(artifacts + QStringLiteral("/scroll-window.png")),
            "Save the actual shared controls for visual review");
    }
    events();
    key(*window, Qt::Key_S, Qt::ControlModifier);
    waitFor([&] { return window && window->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog")); },
        "The retained image can enter the real output flow again after cancellation");
    dialog = window->findChild<QFileDialog*>(QStringLiteral("mediaSaveAsDialog"));
    dialog->setDirectory(temporary.path()); dialog->selectFile(QStringLiteral("scroll-output.png"));
    static_cast<QDialog*>(dialog)->accept();
    waitFor([&] { return !failure.isEmpty() || (!controller.busy() && !workspace.busy()); },
        "Save As actually archives and exports the completed image before the journey closes");
    require(failure.isEmpty() && archived == 1 && !window, "Successful real output closes only its drained scrolling view");
    const auto listed = archive->list(); require(!listed.failure && listed.entries.size() == 1, "The shared archive contains exactly the completed scrolling result");
    QImage output, archivedImage;
    const auto exported = media::inspectPng(temporary.path() + QStringLiteral("/scroll-output.png"), {}, {}, &output);
    const auto inspected = media::inspectPng(listed.entries.front().path, {}, {}, &archivedImage);
    require(!exported.failure && !inspected.failure && output == expected && archivedImage == expected,
        "Every archived/exported pixel after cancellation equals the independent real displayed page");
    require(output.colorSpace() == actualFirst.colorSpace() && archivedImage.colorSpace() == actualFirst.colorSpace(),
        "The exported and archived scrolling images retain the actual native source color space");
    if (!artifacts.isEmpty())
        require(output.save(artifacts + QStringLiteral("/scroll-complete.png")), "Retain the actual stitched output pixels for review");
    bool controllerStopped{}, workspaceStopped{}, clipboardStopped{}, revealerStopped{};
    QObject::connect(&controller, &application::ScrollController::shutdownFinished, &controller, [&] { controllerStopped = true; });
    QObject::connect(&workspace, &application::MediaWorkspace::shutdownFinished, &workspace, [&] { workspaceStopped = true; });
    QObject::connect(&clipboard, &desktop::SystemClipboard::shutdownFinished, &clipboard, [&] { clipboardStopped = true; });
    QObject::connect(&revealer, &desktop::FileRevealer::shutdownFinished, &revealer, [&] { revealerStopped = true; });
    controller.shutdown(); workspace.shutdown(); clipboard.shutdown(); revealer.shutdown();
    waitFor([&] { return controllerStopped && workspaceStopped && clipboardStopped && revealerStopped; },
        "All actual application services drain before their owners are destroyed");
    require(QFile::exists(listed.entries.front().path) && output == expected,
        "Closing the journey and its services never invalidates the completed durable result");
    source.shutdown(); waitFor([&] { return stopped; }, "The native source owner drains after scrolling");
    std::cout << "PASS real native input/capture, exact archived/exported pixels, production controller/start UI, cancelled output and ordered drain\n";
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        if (app.arguments().contains(QStringLiteral("--unavailable"))) unavailableAndLifetime();
        else actualNative(app.arguments().size() > 1 ? app.arguments().at(1) : QString());
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
