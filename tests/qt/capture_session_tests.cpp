#include "capture/CaptureSession.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace HuhuCapture::Capture;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < 8000) {
        QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(); QThread::msleep(2);
    }
    require(condition(), "the actual image preparation completes on its worker");
}
Snapshot imageSource(quint64 generation = 1)
{
    Snapshot snapshot; snapshot.sourceId = 17; snapshot.generation = generation;
    snapshot.image = QImage(32, 24, QImage::Format_ARGB32);
    for (int y = 0; y < snapshot.image.height(); ++y) for (int x = 0; x < snapshot.image.width(); ++x)
        snapshot.image.setPixel(x, y, qRgba(x * 7, y * 9, x + y, 80 + x));
    snapshot.image.setPixel(4, 3, qRgba(255, 128, 0, 145));
    return snapshot;
}
void available(CaptureSession& session, Action action)
{ session.setCapability(action, {CapabilityState::Available, {}}); }
void select(CaptureSession& session)
{
    session.pointerPress({5, 4}); session.pointerMove({20, 18}); session.pointerRelease({20, 18}, false);
    require(session.stage() == Stage::Adjusting && !session.pointerPressed(), "a complete drag enters adjustment once");
}

void actualPixelOutput()
{
    CaptureSession session;
    const auto source = imageSource();
    require(!session.load(source), "load the actual owned source image");
    select(session);
    require(session.request(Action::Clipboard) == 0, "a disconnected output cannot pretend to work");
    available(session, Action::Clipboard); available(session, Action::SaveAs);
    std::vector<ActionRequest> requests;
    QObject::connect(&session, &CaptureSession::actionRequested, &session, [&](const ActionRequest& value) { requests.push_back(value); });
    const auto token = session.request(Action::Clipboard);
    require(token && session.stage() == Stage::Submitting && session.request(Action::Clipboard) == 0,
        "one bounded preparation/output is active and rejects duplicate actions");
    waitFor([&] { return requests.size() == 1; });
    const auto& output = requests.front();
    require(output.token == token && output.sourceId == 17 && output.generation == 1 &&
        output.sourceRect == QRect(5, 4, 15, 14) && !output.desktopRect && output.image.size() == QSize(15, 14),
        "the real crop keeps exact source identity, coordinates and dimensions");
    for (int y = 0; y < output.image.height(); ++y) for (int x = 0; x < output.image.width(); ++x)
        require(output.image.pixel(x, y) == source.image.pixel(x + 5, y + 4), "the prepared image keeps every source pixel and alpha byte");
    session.complete(token + 1, ActionResult::Completed);
    require(session.stage() == Stage::Submitting, "a stale host completion cannot close a different request");
    session.complete(token, ActionResult::Failed, QStringLiteral("Actual output failed"));
    require(session.stage() == Stage::Adjusting && !session.failureText().isEmpty(), "actual failure retains the selection and retry state");
    const auto save = session.request(Action::SaveAs);
    waitFor([&] { return requests.size() == 2; });
    session.complete(save, ActionResult::Cancelled);
    require(session.stage() == Stage::Adjusting && session.failureText().isEmpty(), "cancelled Save As retains the complete selected image");
    const auto copied = session.request(Action::Clipboard);
    waitFor([&] { return requests.size() == 3; });
    session.complete(copied, ActionResult::Completed);
    require(session.stage() == Stage::Closed && session.activeRequest() == 0, "only actual host success completes capture");
    std::cout << "PASS real worker crop, source alpha, connected output, retries and stale completion isolation\n";
}

void entryAndCancellation()
{
    CaptureSession session;
    available(session, Action::Qr); available(session, Action::Pin); available(session, Action::Clipboard);
    std::vector<ActionRequest> requests;
    QObject::connect(&session, &CaptureSession::actionRequested, &session, [&](const ActionRequest& value) { requests.push_back(value); });
    require(!session.load(imageSource(), {Action::Qr}), "load an explicit QR capture entry");
    session.pointerPress({5, 4}); session.pointerMove({20, 18}); session.pointerRelease({20, 18}, true);
    waitFor([&] { return requests.size() == 1; });
    require(requests.back().action == Action::Qr, "explicit CLI entry takes precedence over Ctrl pinning");
    session.complete(requests.back().token, ActionResult::Cancelled);
    require(session.stage() == Stage::Closed, "dismissing the QR result preserves the original capture close behavior");
    require(!session.load(imageSource(2)), "load the next independent capture generation");
    session.pointerPress({5, 4}); session.pointerMove({20, 18}); session.pointerRelease({20, 18}, true);
    waitFor([&] { return requests.size() == 2; });
    require(requests.back().action == Action::Pin, "Ctrl release without CLI entry directly requests the shared editor");
    session.complete(requests.back().token, ActionResult::Failed, QStringLiteral("Editor unavailable"));
    const auto cancelled = session.request(Action::Clipboard);
    session.cancel();
    require(!session.load(imageSource(3)), "replace a closed session without consuming old worker results");
    select(session);
    const auto next = session.request(Action::Clipboard);
    require(next && next != cancelled, "request tokens remain unique across source generations");
    waitFor([&] { return requests.size() == 3; });
    require(requests.back().token == next && requests.back().generation == 3,
        "the single worker queue discards cancelled old-generation crops before the new result");
    session.complete(next, ActionResult::Completed);
    std::cout << "PASS explicit entry precedence, Ctrl pinning and cancelled-generation worker ownership\n";
}

void colorAndCoordinates()
{
    CaptureSession session;
    require(!session.load(imageSource()), "load authorized image color sampling");
    session.pointerMove({4, 3});
    std::vector<ActionRequest> requests;
    QObject::connect(&session, &CaptureSession::actionRequested, &session, [&](const ActionRequest& value) { requests.push_back(value); });
    const std::pair<Action, QString> expected[]{
        {Action::Hex, QStringLiteral("FF8000")}, {Action::Rgb, QStringLiteral("rgb(255,128,0)")},
        {Action::Cmyk, QStringLiteral("cmyk(0,50,100,0)")}, {Action::Position, QStringLiteral("source(4,3)")}};
    for (const auto& item : expected) {
        available(session, item.first);
        const auto token = session.request(item.first);
        require(token && requests.back().text == item.second && requests.back().image.isNull(),
            "color and source-position text preserves exact original formatting with explicit source-local coordinates");
        session.complete(token, ActionResult::Failed, QStringLiteral("Copy not accepted"));
        require(session.stage() == Stage::Selecting, "failed color copy retains the original source and magnifier");
    }
    session.nudgeCursor({1, -1});
    require(session.cursor() == QPoint(5, 2), "arrows move exactly one original-image pixel");
    session.nudgeCursor({-100, -100});
    require(session.cursor() == QPoint(), "image cursor motion stops at the authorized source edge");

    // Unit data for explicit per-screen conversion, not physical mixed-DPI evidence.
    ScreenBinding left; left.sourcePixels = QRect(0, 0, 1920, 1080);
    left.logicalGeometry = QRectF(-1536, 0, 1536, 864); left.pixelsPerDipX = left.pixelsPerDipY = 1.25;
    ScreenBinding right; right.sourcePixels = QRect(1920, 0, 2560, 1440);
    right.logicalGeometry = QRectF(0, 0, 1280, 720); right.pixelsPerDipX = right.pixelsPerDipY = 2;
    require(sourceFromScreenLocal(left, {8, 12}) == QPointF(10, 15) &&
        sourceFromScreenLocal(right, {8, 12}) == QPointF(1936, 24) &&
        screenLocalFromSource(right, {1936, 24}) == QPointF(8, 12), "mixed scale conversions use the explicit source screen");
    require(sourceFromScreenLocal(left, {1600, 0}) == QPointF(2000, 0),
        "a captured drag outside its view still uses that view's native-to-DIP transform");
    std::cout << "PASS frozen-image colors, source-local coordinate identity and explicit per-screen unit mapping\n";
}

void authorizedImageClick()
{
    CaptureSession session;
    require(!session.load(imageSource()), "load the already-authorized complete image");
    session.pointerPress({8, 6});
    session.pointerMove({8, 6}); // A redundant platform move must not erase the highlight.
    session.pointerRelease({8, 6}, false);
    const auto bounds = session.selection().bounds();
    require(session.stage() == Stage::Adjusting && bounds.left == 0 && bounds.top == 0 && bounds.right == 32 && bounds.bottom == 24,
        "one click accepts the complete authorized source without pretending to enumerate desktop windows");
    std::cout << "PASS authorized-source click acceptance without desktop-window inference\n";
}

void directEditorEntry()
{
    CaptureSession session;
    require(!session.load(imageSource()), "load the complete source for direct annotation");
    select(session);
    available(session, Action::Edit);
    std::vector<ActionRequest> requests;
    QObject::connect(&session, &CaptureSession::actionRequested, &session, [&](const ActionRequest& value) { requests.push_back(value); });
    const auto token = session.request(Action::Edit);
    waitFor([&] { return !requests.empty(); });
    require(token && requests.front().action == Action::Edit && requests.front().sourceRect == QRect(5, 4, 15, 14) &&
        requests.front().image == session.snapshot().image.copy(5, 4, 15, 14),
        "direct annotation receives the exact crop with its own editor intent, independent of Ctrl pinning");
    session.complete(token, ActionResult::Completed);
    require(session.stage() == Stage::Closed, "capture completes after the real shared editor accepts its image");
    std::cout << "PASS direct shared-editor handoff distinct from explicit pinning\n";
}

void recordingSelectionWithoutImage()
{
    CaptureSession session;
    // This is a selection/handoff contract test, not a physical capture test.
    // Use the real QScreen geometry for a valid Qt binding; no native backend
    // or recording success is asserted by this owned image fixture.
    auto* screen = QGuiApplication::primaryScreen();
    require(screen && screen->geometry().width() > 20 && screen->geometry().height() > 18,
        "the component environment provides a real Qt screen binding");
    Snapshot snapshot;
    snapshot.sourceId = 29; snapshot.generation = 7;
    snapshot.coordinates = CoordinateSpace::DesktopPixels;
    snapshot.desktopOrigin = screen->geometry().topLeft();
    snapshot.image = QImage(screen->geometry().size(), QImage::Format_ARGB32);
    snapshot.image.fill(qRgb(29, 61, 113));
    ScreenBinding binding;
    binding.screenId = 1; binding.screen = screen;
    binding.nativeId = QStringLiteral("component-screen-binding");
    binding.sourcePixels = snapshot.image.rect();
    binding.logicalGeometry = screen->geometry();
    binding.logicalWorkArea = screen->availableGeometry();
    binding.pixelsPerDipX = binding.pixelsPerDipY = 1;
    snapshot.screens.push_back(binding);
    require(!session.load(snapshot), "load an explicit desktop selection contract");
    available(session, Action::Record);
    require(session.request(Action::Record) == 0,
        "recording still requires a complete selected region before admission");
    select(session);
    std::vector<ActionRequest> requests;
    QObject::connect(&session, &CaptureSession::actionRequested, &session,
        [&](const ActionRequest& value) { requests.push_back(value); });
    const auto sourceBytes = session.snapshot().image.cacheKey();
    const auto token = session.request(Action::Record);
    require(token && requests.size() == 1 && requests.front().token == token &&
        requests.front().sourceId == 29 && requests.front().generation == 7 &&
        requests.front().coordinates == CoordinateSpace::DesktopPixels &&
        requests.front().sourceRect == QRect(5, 4, 15, 14) &&
        requests.front().desktopRect == QRect(5, 4, 15, 14).translated(*snapshot.desktopOrigin) &&
        requests.front().image.isNull() && session.snapshot().image.cacheKey() == sourceBytes,
        "recording receives the exact source and desktop region without allocating a cropped image");
    session.complete(token, ActionResult::Failed, QStringLiteral("The real recorder was unavailable"));
    require(session.stage() == Stage::Adjusting && session.selection().hasSelection(),
        "a recorder rejection retains the existing image and selection for retry");
    std::cout << "PASS recording selection identity and desktop coordinates without an unused image copy\n";
}

void reentrantHost()
{
    auto* session = new CaptureSession;
    QPointer<CaptureSession> lifetime(session);
    require(!session->load(imageSource()), "load a real source for host lifetime validation");
    available(*session, Action::History);
    QObject::connect(session, &CaptureSession::changed, session, [session] {
        if (session->stage() == Stage::Submitting) delete session;
    });
    require(session->request(Action::History) != 0 && !lifetime,
        "a synchronous state observer may destroy capture without a second request dispatch");
    session = new CaptureSession; lifetime = session;
    require(!session->load(imageSource()), "reload another independent lifetime");
    available(*session, Action::History);
    QObject::connect(session, &CaptureSession::actionRequested, session, [session](const ActionRequest&) { delete session; });
    require(session->request(Action::History) != 0 && !lifetime,
        "a synchronous host may take over and destroy the capture origin");
    std::cout << "PASS capture state and host action synchronous lifetime handoff\n";
}
}
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try { actualPixelOutput(); entryAndCancellation(); colorAndCoordinates(); authorizedImageClick(); directEditorEntry(); recordingSelectionWithoutImage(); reentrantHost(); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
