#include "capture/CaptureSession.h"

#include "capture/PixelSample.h"
#include "core/PixelBuffer.h"
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <stop_token>
#include <utility>

namespace HuhuCapture::Capture {
namespace {
constexpr auto actionCount = static_cast<std::size_t>(Action::Count);
constexpr auto actionIndex(Action action) { return static_cast<std::size_t>(action); }
bool isText(Action action) { return action >= Action::Hex && action <= Action::Position; }
bool needsImage(Action action) { return !isText(action) && action != Action::History; }
SelectionPoint point(QPoint value) { return {static_cast<float>(value.x()), static_cast<float>(value.y())}; }
QRect rectangle(const SelectionBounds& value)
{
    return {static_cast<int>(value.left), static_cast<int>(value.top),
        static_cast<int>(value.right - value.left), static_cast<int>(value.bottom - value.top)};
}
Failure validate(const Snapshot& snapshot)
{
    const auto layout = Core::pixelLayout(snapshot.image.width(), snapshot.image.height(), 4);
    constexpr quint64 maximum = 512ull * 1024 * 1024;
    if (!snapshot.sourceId || !snapshot.generation || snapshot.image.isNull() || !layout || layout->bytes > maximum ||
        static_cast<quint64>(snapshot.image.sizeInBytes()) > maximum || snapshot.image.devicePixelRatio() != 1 ||
        snapshot.image.width() > (1 << 24) || snapshot.image.height() > (1 << 24))
        return {Failure::Code::InvalidSnapshot, 0, QStringLiteral("A complete bounded source image with explicit identity is required")};
    if (snapshot.coordinates == CoordinateSpace::AuthorizedSourcePixels) {
        if (snapshot.desktopOrigin || !snapshot.screens.isEmpty() || !snapshot.windows.isEmpty())
            return {Failure::Code::InvalidSnapshot, 0, QStringLiteral("An authorized source without verified desktop mapping cannot carry global coordinates")};
    } else {
        if (!snapshot.desktopOrigin || snapshot.screens.isEmpty())
            return {Failure::Code::InvalidSnapshot, 0, QStringLiteral("Desktop capture requires verified per-screen bindings")};
        const auto right = static_cast<qint64>(snapshot.desktopOrigin->x()) + snapshot.image.width();
        const auto bottom = static_cast<qint64>(snapshot.desktopOrigin->y()) + snapshot.image.height();
        if (right > (std::numeric_limits<int>::max)() || bottom > (std::numeric_limits<int>::max)())
            return {Failure::Code::InvalidSnapshot, 0, QStringLiteral("The desktop extent cannot be represented by native coordinates")};
        QVector<quint64> identities;
        for (const auto& screen : snapshot.screens) {
            if (!screen.screenId || !screen.screen || screen.nativeId.isEmpty() || screen.sourcePixels.isEmpty() ||
                !snapshot.image.rect().contains(screen.sourcePixels) || screen.logicalGeometry.isEmpty() ||
                screen.logicalWorkArea.isEmpty() || screen.logicalGeometry != QRectF(screen.screen->geometry()) ||
                !std::isfinite(screen.pixelsPerDipX) || !std::isfinite(screen.pixelsPerDipY) ||
                screen.pixelsPerDipX <= 0 || screen.pixelsPerDipY <= 0 || identities.contains(screen.screenId))
                return {Failure::Code::SourceChanged, 0, QStringLiteral("The native display and Qt screen mapping is no longer valid")};
            identities.push_back(screen.screenId);
        }
    }
    return {};
}
struct PreparedAction { ActionRequest request; Failure failure; };
}

QPointF sourceFromScreenLocal(const ScreenBinding& binding, QPointF local)
{
    return {binding.sourcePixels.x() + local.x() * binding.pixelsPerDipX,
        binding.sourcePixels.y() + local.y() * binding.pixelsPerDipY};
}
QPointF screenLocalFromSource(const ScreenBinding& binding, QPointF source)
{
    return {(source.x() - binding.sourcePixels.x()) / binding.pixelsPerDipX,
        (source.y() - binding.sourcePixels.y()) / binding.pixelsPerDipY};
}

struct CaptureSession::State {
    Snapshot snapshot;
    SelectionModel selection;
    StartOptions options;
    std::array<Capability, actionCount> capabilities;
    Stage stage{Stage::Empty}, resume{Stage::Empty};
    QPoint cursor, lastPointer;
    bool pressed{};
    quint64 sequence{}, active{};
    Action activeAction{Action::Clipboard};
    QString failure;
    std::stop_source stop;
    QThreadPool pool;
    State() { pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1); }
};

CaptureSession::CaptureSession(QObject* parent) : QObject(parent), state_(std::make_unique<State>()) {}
CaptureSession::~CaptureSession() { state_->stop.request_stop(); }

Failure CaptureSession::load(Snapshot snapshot, StartOptions options)
{
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread() || thread() != qGuiApp->thread() ||
        (state_->stage != Stage::Empty && state_->stage != Stage::Closed))
        return {Failure::Code::Busy, 0, QStringLiteral("The existing capture must finish before another image is loaded")};
    if (const auto failure = validate(snapshot)) return failure;
    if (options.enter && *options.enter != Action::Pin && *options.enter != Action::Scroll &&
        *options.enter != Action::Record && *options.enter != Action::Qr)
        return {Failure::Code::InvalidSnapshot, 0, QStringLiteral("The capture entry action is unsupported")};
    try {
        SelectionModel selection(static_cast<float>(snapshot.image.width()), static_cast<float>(snapshot.image.height()));
        std::vector<SelectionBounds> candidates;
        candidates.reserve(static_cast<std::size_t>(snapshot.windows.size()));
        for (const auto& window : snapshot.windows)
            candidates.push_back({static_cast<float>(window.x()), static_cast<float>(window.y()),
                static_cast<float>(window.x()) + static_cast<float>(window.width()),
                static_cast<float>(window.y()) + static_cast<float>(window.height())});
        selection.setWindowCandidates(std::move(candidates));
        const QPoint cursor{std::clamp(snapshot.cursor.x(), 0, snapshot.image.width() - 1),
            std::clamp(snapshot.cursor.y(), 0, snapshot.image.height() - 1)};
        if (snapshot.coordinates == CoordinateSpace::AuthorizedSourcePixels)
            selection.setSelection({0, 0, static_cast<float>(snapshot.image.width()), static_cast<float>(snapshot.image.height())});
        else selection.highlight(point(cursor));
        std::stop_source nextStop;
        state_->stop.request_stop();
        state_->stop = std::move(nextStop);
        state_->snapshot = std::move(snapshot); state_->selection = std::move(selection);
        state_->cursor = cursor; state_->options = options; state_->pressed = false; state_->active = 0;
        state_->failure.clear(); state_->stage = Stage::Selecting;
    } catch (const std::bad_alloc&) {
        return {Failure::Code::Allocation, 0, QStringLiteral("The source selection could not be initialized")};
    }
    Q_EMIT changed();
    return {};
}
const Snapshot& CaptureSession::snapshot() const noexcept { return state_->snapshot; }
const SelectionModel& CaptureSession::selection() const noexcept { return state_->selection; }
Stage CaptureSession::stage() const noexcept { return state_->stage; }
QPoint CaptureSession::cursor() const noexcept { return state_->cursor; }
bool CaptureSession::pointerPressed() const noexcept { return state_->pressed; }
quint64 CaptureSession::activeRequest() const noexcept { return state_->active; }
QString CaptureSession::failureText() const { return state_->failure; }
void CaptureSession::setCapability(Action action, Capability capability)
{
    if (actionIndex(action) >= actionCount) return;
    state_->capabilities[actionIndex(action)] = std::move(capability);
    Q_EMIT changed();
}
Capability CaptureSession::capability(Action action) const
{ return actionIndex(action) < actionCount ? state_->capabilities[actionIndex(action)] : Capability{}; }

void CaptureSession::pointerMove(QPoint source)
{
    if (state_->stage != Stage::Selecting && state_->stage != Stage::Adjusting) return;
    state_->cursor = {std::clamp(source.x(), 0, state_->snapshot.image.width() - 1),
        std::clamp(source.y(), 0, state_->snapshot.image.height() - 1)};
    if (state_->pressed && source != state_->lastPointer) {
        if (state_->stage == Stage::Selecting) state_->selection.createTo(point(source));
        else state_->selection.adjustTo(point(source));
    } else if (!state_->pressed && state_->stage == Stage::Selecting) state_->selection.highlight(point(source));
    state_->lastPointer = source;
    Q_EMIT changed();
}
void CaptureSession::pointerPress(QPoint source)
{
    if (state_->stage != Stage::Selecting && state_->stage != Stage::Adjusting) return;
    state_->lastPointer = source; state_->pressed = true; state_->failure.clear();
    state_->cursor = {std::clamp(source.x(), 0, state_->snapshot.image.width() - 1),
        std::clamp(source.y(), 0, state_->snapshot.image.height() - 1)};
    if (state_->stage == Stage::Selecting) state_->selection.beginCreation(point(source));
    else state_->selection.beginAdjustment(point(source));
    Q_EMIT changed();
}
void CaptureSession::pointerRelease(QPoint source, bool control)
{
    if (!state_->pressed) return;
    const QPointer<CaptureSession> lifetime(this);
    if (source != state_->lastPointer) pointerMove(source);
    if (!lifetime) return;
    state_->pressed = false; state_->selection.finishGesture();
    std::optional<Action> immediate;
    if (state_->stage == Stage::Selecting && state_->selection.hasSelection()) {
        state_->stage = Stage::Adjusting;
        if (state_->options.enter) immediate = state_->options.enter;
        else if (control) immediate = Action::Pin;
    }
    Q_EMIT changed();
    if (lifetime && immediate) request(*immediate);
}
void CaptureSession::pointerCaptureLost()
{
    if (!state_->pressed) return;
    state_->pressed = false; state_->selection.finishGesture();
    Q_EMIT changed();
}
void CaptureSession::nudgeCursor(QPoint delta)
{
    if (state_->stage != Stage::Selecting && state_->stage != Stage::Adjusting) return;
    const QPointer<CaptureSession> lifetime(this);
    const QPoint next{static_cast<int>(std::clamp(static_cast<qint64>(state_->cursor.x()) + delta.x(), qint64{},
        static_cast<qint64>(state_->snapshot.image.width() - 1))),
        static_cast<int>(std::clamp(static_cast<qint64>(state_->cursor.y()) + delta.y(), qint64{},
        static_cast<qint64>(state_->snapshot.image.height() - 1)))};
    pointerMove(next);
    if (lifetime) Q_EMIT cursorNudged(state_->cursor);
}

quint64 CaptureSession::request(Action action)
{
    if (state_->stage != Stage::Selecting && state_->stage != Stage::Adjusting) return 0;
    if (!capability(action).canRequest() || (needsImage(action) &&
        (state_->stage != Stage::Adjusting || !state_->selection.hasSelection()))) return 0;
    ActionRequest request;
    request.token = ++state_->sequence;
    if (!request.token) request.token = ++state_->sequence;
    request.action = action; request.coordinates = state_->snapshot.coordinates;
    request.sourceId = state_->snapshot.sourceId; request.generation = state_->snapshot.generation;
    if (needsImage(action)) {
        request.sourceRect = rectangle(state_->selection.bounds());
        if (state_->snapshot.desktopOrigin)
            request.desktopRect = request.sourceRect.translated(*state_->snapshot.desktopOrigin);
    } else if (isText(action)) {
        const auto rgb = state_->snapshot.image.pixel(state_->cursor);
        if (action == Action::Hex)
            request.text = QStringLiteral("%1%2%3").arg(qRed(rgb), 2, 16, QChar(u'0')).arg(qGreen(rgb), 2, 16, QChar(u'0')).arg(qBlue(rgb), 2, 16, QChar(u'0')).toUpper();
        else if (action == Action::Rgb)
            request.text = QStringLiteral("rgb(%1,%2,%3)").arg(qRed(rgb)).arg(qGreen(rgb)).arg(qBlue(rgb));
        else if (action == Action::Cmyk) {
            const auto [cyan, magenta, yellow, black] = cmyk(static_cast<std::uint8_t>(qRed(rgb)),
                static_cast<std::uint8_t>(qGreen(rgb)), static_cast<std::uint8_t>(qBlue(rgb)));
            request.text = QStringLiteral("cmyk(%1,%2,%3,%4)").arg(cyan).arg(magenta).arg(yellow).arg(black);
        } else if (state_->snapshot.desktopOrigin) {
            const auto desktop = state_->cursor + *state_->snapshot.desktopOrigin;
            request.text = QStringLiteral("%1,%2").arg(desktop.x()).arg(desktop.y());
        } else request.text = QStringLiteral("source(%1,%2)").arg(state_->cursor.x()).arg(state_->cursor.y());
    }
    const auto token = request.token;
    state_->active = token; state_->activeAction = action; state_->resume = state_->stage;
    state_->stage = Stage::Submitting; state_->pressed = false; state_->selection.finishGesture();
    state_->failure.clear(); state_->stop = {};
    const QPointer<CaptureSession> lifetime(this);
    Q_EMIT changed();
    if (!lifetime || state_->active != token) return token;
    if (!needsImage(action)) {
        Q_EMIT actionRequested(request);
        return token;
    }
    try {
        auto watcher = std::make_unique<QFutureWatcher<PreparedAction>>(this);
        connect(watcher.get(), &QFutureWatcher<PreparedAction>::finished, this, [this, watcher = watcher.get(), token] {
            watcher->deleteLater();
            if (state_->active != token) return;
            PreparedAction result;
            try {
                result = watcher->result();
            } catch (const std::exception&) {
                complete(token, ActionResult::Failed, QStringLiteral("The selected image could not be prepared"));
                return;
            }
            if (result.failure) {
                complete(token, result.failure.code == Failure::Code::Cancelled ? ActionResult::Cancelled : ActionResult::Failed,
                    result.failure.detail);
                return;
            }
            Q_EMIT actionRequested(result.request);
        });
        watcher->setFuture(QtConcurrent::run(&state_->pool,
            [request = std::move(request), image = state_->snapshot.image, stop = state_->stop.get_token()]() mutable {
                PreparedAction result;
                if (stop.stop_requested()) { result.failure.code = Failure::Code::Cancelled; return result; }
                result.request = std::move(request);
                result.request.image = image.copy(result.request.sourceRect);
                if (result.request.image.isNull())
                    result.failure = {Failure::Code::Allocation, 0, QStringLiteral("The selected image could not be prepared")};
                else if (stop.stop_requested()) { result.request.image = {}; result.failure.code = Failure::Code::Cancelled; }
                return result;
            }));
        watcher.release();
    } catch (const std::exception&) {
        complete(token, ActionResult::Failed, QStringLiteral("The selected image could not be prepared"));
    }
    return token;
}

void CaptureSession::complete(quint64 token, ActionResult result, QString detail)
{
    if (!token || state_->active != token) return;
    state_->active = 0;
    state_->failure = result == ActionResult::Failed ? std::move(detail) : QString();
    const bool close = result == ActionResult::Completed ||
        (state_->activeAction == Action::Qr && result == ActionResult::Cancelled);
    state_->stage = close ? Stage::Closed : state_->resume;
    const QPointer<CaptureSession> lifetime(this);
    Q_EMIT changed();
    if (lifetime && close) Q_EMIT closed();
}
void CaptureSession::cancel()
{
    if (state_->stage == Stage::Closed) return;
    const auto token = std::exchange(state_->active, 0);
    state_->stop.request_stop(); state_->pressed = false; state_->selection.finishGesture();
    state_->stage = Stage::Closed;
    const QPointer<CaptureSession> lifetime(this);
    if (token) Q_EMIT actionCancelled(token);
    if (!lifetime) return;
    Q_EMIT changed();
    if (lifetime) Q_EMIT closed();
}

} // namespace HuhuCapture::Capture
