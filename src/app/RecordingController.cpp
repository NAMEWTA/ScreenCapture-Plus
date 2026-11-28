#include "app/RecordingController.h"
#include "app/MediaWorkspace.h"
#include "platform/WindowActivation.h"
#include "recording/FramePixels.h"
#include "recording/PlatformRecording.h"
#include "recording/RecordingSession.h"
#include "services/capture/ScreenCastSource.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/RecordingWindow.h"

#include <QApplication>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace HuhuCapture::Application {
namespace {
namespace cap = Capture;
namespace rec = Recording;
namespace ui = UI::Qt;
rec::PixelRegion pixels(const QRect& region)
{ return {region.x(), region.y(), region.width(), region.height()}; }
bool needsPermission(const cap::ActionRequest& selection)
{
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"), Qt::CaseInsensitive) ||
        qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0 ||
        selection.coordinates != cap::CoordinateSpace::DesktopPixels || !selection.desktopRect;
}
// Four outside work-area candidates reuse this repository's PR #7 controller
// (251bc0071278c6b711c840bf5188d16870bc7349), with the same verified per-screen
// mapping. Never scale a virtual desktop or a Portal source by one global DPR.
QVector<QRect> outsideAreas(const QVector<cap::ScreenBinding>& screens, QRect sourceSelection)
{
    QVector<QRect> result;
    for (const auto& binding : screens) {
        if (!binding.screen || binding.screen->name().compare(binding.nativeId,
#ifdef Q_OS_WIN
            Qt::CaseInsensitive
#else
            Qt::CaseSensitive
#endif
            ) != 0 ||
            binding.logicalGeometry != QRectF(binding.screen->geometry()) ||
            binding.logicalWorkArea != QRectF(binding.screen->availableGeometry()) || binding.sourcePixels.isEmpty() ||
            !std::isfinite(binding.pixelsPerDipX) || binding.pixelsPerDipX <= 0 ||
            !std::isfinite(binding.pixelsPerDipY) || binding.pixelsPerDipY <= 0) continue;
        const auto work = binding.logicalWorkArea.adjusted(4, 4, -4, -4);
        const auto first = cap::screenLocalFromSource(binding, sourceSelection.topLeft()) + binding.logicalGeometry.topLeft();
        const auto end = cap::screenLocalFromSource(binding,
            QPointF(sourceSelection.x() + sourceSelection.width(), sourceSelection.y() + sourceSelection.height())) + binding.logicalGeometry.topLeft();
        const auto blocked = QRectF(first, end).adjusted(-8, -8, 8, 8).intersected(work);
        const auto spaces = blocked.isEmpty() ? std::array<QRectF, 4>{work, {}, {}, {}} : std::array<QRectF, 4>{
            QRectF(work.left(), work.top(), work.width(), blocked.top() - work.top()),
            QRectF(work.left(), blocked.bottom(), work.width(), work.bottom() - blocked.bottom()),
            QRectF(work.left(), work.top(), blocked.left() - work.left(), work.height()),
            QRectF(blocked.right(), work.top(), work.right() - blocked.right(), work.height())};
        for (const auto& area : spaces) {
            const int left = static_cast<int>(std::ceil(area.left())), top = static_cast<int>(std::ceil(area.top()));
            const QRect inward(left, top, static_cast<int>(std::floor(area.right())) - left,
                static_cast<int>(std::floor(area.bottom())) - top);
            if (!inward.isEmpty()) result.push_back(inward);
        }
    }
    return result;
}

// One transparent, input-free outline per verified screen. Only the outer
// selection edge is drawn; crossing a screen boundary adds no interior edge.
// The stroke lies outside the selected pixels, including on desktops where
// application windows cannot be excluded from capture.
class RecordingOutline final : public QWidget {
public:
    RecordingOutline(const cap::ScreenBinding& binding, QRect selection)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint |
              Qt::NoDropShadowWindowHint |
              Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus), binding_(binding), selection_(selection)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setObjectName(QStringLiteral("recordingOutline"));
        setScreen(binding.screen); setGeometry(binding.logicalGeometry.toAlignedRect());
        const auto first = cap::screenLocalFromSource(binding_, selection_.topLeft());
        const auto end = cap::screenLocalFromSource(binding_,
            QPointF(selection_.x() + selection_.width(), selection_.y() + selection_.height()));
        // A native bounding shape excludes the whole selection, even when no
        // compositing WM exists. The additional outside margin is conservative
        // for integer window/DIP rounding; capture never paints a black hole.
        const auto inner = QRectF(first, end).adjusted(-1, -1, 1, 1).toAlignedRect();
        const auto shape = (QRegion(inner.adjusted(-4, -4, 4, 4)) - QRegion(inner)) & QRegion(rect());
        border_ = !shape.isEmpty();
        if (border_) setMask(shape); // an empty QWidget mask would mean no mask
    }
    bool hasBorder() const noexcept { return border_; }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const auto first = cap::screenLocalFromSource(binding_, selection_.topLeft());
        const auto end = cap::screenLocalFromSource(binding_,
            QPointF(selection_.x() + selection_.width(), selection_.y() + selection_.height()));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(22, 119, 255), 2)); painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(first, end).adjusted(-3, -3, 3, 3));
    }
private:
    cap::ScreenBinding binding_;
    QRect selection_;
    bool border_{};
};
}

struct RecordingController::State {
    RecordingController* owner;
    std::shared_ptr<Media::MediaArchiveService> archive;
    MediaWorkspace& workspace;
    ui::DesktopTranslator translator;
    std::unique_ptr<rec::RecordingSession> session;
    std::unique_ptr<cap::ScreenCastSource> screenCast;
    std::unique_ptr<ui::CaptureWindow> selector;
    QPointer<ui::RecordingWindow> window;
    std::vector<std::unique_ptr<RecordingOutline>> outlines;
    std::vector<QPointer<QWindow>> hiddenSurfaces;
    std::shared_ptr<Core::CaptureControls> controls;
    cap::ActionRequest selection;
    rec::BackendRequest pending;
    std::optional<rec::BackendRequest> deferredStart;
    std::optional<rec::SessionResult> completedResult;
    QTimer timer, controlsTimer;
    Core::MediaClock::time_point controlsDeadline;
    bool journey{}, closing{}, outputPending{}, waitingPermission{}, shutdownReported{};
    bool waitingRefresh{}, refreshStarted{}, preparingDesktop{}, guardingControls{}, externalStopAvailable{};
    QString externalStopHint, controlsFailure;
    std::pair<int, int> sourceSize;
    rec::FrameSourceContract sourceContract;

    State(RecordingController* value, std::shared_ptr<Media::MediaArchiveService> service, MediaWorkspace& media)
        : owner(value), archive(std::move(service)), workspace(media)
    {
        timer.setInterval(250);
        controlsTimer.setInterval(20);
        QObject::connect(&controlsTimer, &QTimer::timeout, owner, [this] { admitAfterUnmap(); });
    }
    bool busy() const
    { return journey || outputPending || waitingPermission || (session && session->busy()) || (screenCast && screenCast->busy()); }
    QString text(QStringView key, const QString& fallback) const
    {
        const auto value = translator ? translator(key) : QString();
        return value.isEmpty() || value == key ? fallback : value;
    }
    void notify()
    {
        QTimer::singleShot(0, owner, [q = QPointer(owner)] {
            if (!q) return;
            const bool busy = q->busy(); Q_EMIT q->busyChanged(busy);
            if (q && q->state_->closing && !busy && !q->state_->shutdownReported) {
                q->state_->shutdownReported = true; Q_EMIT q->shutdownFinished();
            }
        });
    }
    void protect(QWindow* native)
    {
        if (!native || !native->handle()) return;
        native->installEventFilter(owner);
        const bool excluded = rec::excludeControlsFromCapture(static_cast<std::uintptr_t>(native->winId())) ==
            rec::ControlsExclusion::Excluded;
        native->setProperty("huhuCaptureExcluded", excluded);
        if (window && native == window->windowHandle()) window->setCaptureExcluded(excluded);
    }
    bool publishControls()
    {
        if (!guardingControls || !controls || !window) return false;
        auto snapshot = std::make_shared<Core::CaptureControlSnapshot>();
        const auto add = [&](QWidget& widget, bool hidden) {
            auto* native = widget.windowHandle();
            if (!native || !native->handle()) return false;
            snapshot->surfaces.push_back({static_cast<std::uintptr_t>(native->winId()),
                hidden ? Core::CaptureControlPlacement::Hidden :
                native->property("huhuCaptureExcluded").toBool() ? Core::CaptureControlPlacement::Excluded :
                    Core::CaptureControlPlacement::Outside});
            return true;
        };
        if (!add(*window, window->hiddenForCapture())) return false;
        for (const auto& outline : outlines)
            if (outline->hasBorder() && !add(*outline, !outline->isVisible())) return false;
        controls->snapshot.store(std::move(snapshot), std::memory_order_release);
        return true;
    }
    void restoreControls(bool show)
    {
        guardingControls = false; controls.reset(); controlsTimer.stop(); hiddenSurfaces.clear();
        for (const auto& outline : outlines) outline->hide();
        if (window) {
            window->setCaptureSafeAreas({});
            if (show) window->setHiddenForCapture(false);
            else window->hide();
        }
    }
    void fail(const QString& detail)
    {
        if (session && session->busy()) {
            if (controlsFailure.isEmpty()) controlsFailure = detail;
            session->shutdown();
            return;
        }
        waitingPermission = waitingRefresh = refreshStarted = preparingDesktop = false;
        deferredStart.reset(); controlsTimer.stop();
        if (selector) selector->shutdown();
        if (screenCast) screenCast->stop();
        restoreControls(!closing);
        if (window && !closing) {
            window->setState(rec::SessionState::Failed);
            window->showFailure(detail, {}, false);
        }
        const QPointer<RecordingController> alive(owner);
        if (!closing) Q_EMIT owner->operationFailed(detail);
        if (alive) notify();
    }
    void finishJourney()
    {
        journey = waitingPermission = waitingRefresh = refreshStarted = preparingDesktop = false;
        deferredStart.reset(); completedResult.reset(); timer.stop(); controlsTimer.stop();
        restoreControls(false);
        notify();
    }
    void output()
    {
        if (closing || outputPending || !completedResult || !completedResult->archivedEntry) return;
        const auto result = *completedResult;
        outputPending = true; if (window) window->setOutputPending(true);
        const auto intent = result.intent == rec::StopIntent::Copy ? ui::OutputIntent::Clipboard : ui::OutputIntent::SaveAs;
        const QPointer<RecordingController> alive(owner);
        const bool admitted = workspace.outputArchived(owner, window, result.token, intent, *result.archivedEntry,
            [q = QPointer(owner), result](ui::OutputResult status, const QString& detail) {
                if (!q) return;
                auto& state = *q->state_; state.outputPending = false;
                if (state.window) state.window->setOutputPending(false);
                if (state.closing) { state.finishJourney(); return; }
                if (status == ui::OutputResult::Failed) {
                    if (state.window) state.window->showFailure(detail, result.archive.path, true);
                    const QPointer<RecordingController> alive(q);
                    Q_EMIT q->operationFailed(detail + QChar(u'\n') + result.archive.path);
                    if (alive) state.notify();
                } else state.finishJourney();
            });
        if (!alive) return;
        if (!admitted) {
            outputPending = false;
            const auto detail = QStringLiteral("The recording is archived, but its output action could not start.");
            if (window) { window->setOutputPending(false); window->showFailure(detail, result.archive.path, true); }
            Q_EMIT owner->operationFailed(detail + QChar(u'\n') + result.archive.path);
        }
        if (alive) notify();
    }
    void completed(const rec::SessionResult& result)
    {
        timer.stop(); waitingPermission = waitingRefresh = preparingDesktop = false;
        const bool cancelled = result.state == rec::SessionState::Cancelled || result.state == rec::SessionState::Discarded;
        restoreControls(!closing && !cancelled);
        QString message = result.failure.detail;
        if (!controlsFailure.isEmpty()) message += (message.isEmpty() ? QString() : QStringLiteral("\n")) + controlsFailure;
        if (window) { window->setState(result.state); window->setElapsed(result.backend.elapsed); }
        const QPointer<RecordingController> alive(owner);
        if (!result.archive.path.isEmpty()) Q_EMIT owner->archiveChanged();
        if (!alive) return;
        if (closing) { journey = false; notify(); return; }
        if (result.state == rec::SessionState::Completed) {
            if (!result.archivedEntry) { fail(QStringLiteral("The recording has no verified archive identity")); return; }
            completedResult = result; output();
        } else if (cancelled) finishJourney();
        else {
            if (window) window->showFailure(message, result.recoveryPath, false);
            Q_EMIT owner->operationFailed(message + (result.recoveryPath.isEmpty() ? QString() : QChar(u'\n') + result.recoveryPath));
        }
        if (alive) notify();
    }
    void startSession(rec::Backend backend, rec::BackendRequest request)
    {
        if (closing || (session && session->busy())) return;
        waitingPermission = waitingRefresh = preparingDesktop = false;
        controlsTimer.stop(); hiddenSurfaces.clear();
        // The selected source belongs only to this invocation. A completed
        // QObject must not keep a first-frame lease or native reader alive.
        session = std::make_unique<rec::RecordingSession>(archive, rec::Backend{});
        QObject::connect(session.get(), &rec::RecordingSession::stateChanged, owner, [this](quint64, rec::SessionState state) {
            if (window) { window->setState(state); window->setElapsed(session->elapsed()); }
            notify();
        });
        QObject::connect(session.get(), &rec::RecordingSession::prepared, owner, [this](quint64, const auto&) { timer.start(); });
        QObject::connect(session.get(), &rec::RecordingSession::finished, owner, [this](const auto& result) { completed(result); });
        QObject::connect(session.get(), &rec::RecordingSession::shutdownFinished, owner, [this] { notify(); });
        const QPointer<RecordingController> alive(owner);
        const auto started = session->start(std::move(request), std::move(backend));
        if (!alive) return;
        if (!started) fail(QStringLiteral("The recording session could not start"));
        if (alive) notify();
    }
    bool prepareControls(bool authorized)
    {
        if (!window || !window->windowHandle()) return false;
        hiddenSurfaces.clear();
        for (auto* tooltip : QApplication::topLevelWidgets()) {
            if (tooltip->windowType() != Qt::ToolTip) continue;
            if (tooltip->windowHandle()) hiddenSurfaces.push_back(tooltip->windowHandle());
            tooltip->hide();
        }
        protect(window->windowHandle());
        const bool excluded = !authorized && window->windowHandle()->property("huhuCaptureExcluded").toBool();
        bool outside{};
        if (!excluded && !authorized) {
            auto areas = outsideAreas(selection.screens, selection.sourceRect);
            outside = !areas.isEmpty() && window->setCaptureSafeAreas(std::move(areas));
            if (outside) window->show();
        }
        if (!excluded && !outside) {
            window->setCaptureSafeAreas({});
            if (!externalStopAvailable) {
                fail(text(u"capture.stopUnavailable", QStringLiteral("A working tray, shortcut or desktop finish action is required before controls can hide.")));
                return false;
            }
            window->setHiddenForCapture(true);
            hiddenSurfaces.push_back(window->windowHandle());
        }
        controls = std::make_shared<Core::CaptureControls>(); guardingControls = true;
        if (!publishControls()) {
            fail(QStringLiteral("The recording controls have no verifiable native surfaces")); return false;
        }
        return true;
    }
    bool allSurfacesUnmapped() const
    {
        for (const auto& native : hiddenSurfaces)
            if (native && (native->isVisible() || native->isExposed())) return false;
        return !waitingRefresh || (window && window->hiddenForCapture() && !window->isVisible() &&
            window->windowHandle() && !window->windowHandle()->isExposed());
    }
    void admitAfterUnmap()
    {
        if (closing || (!preparingDesktop && !waitingRefresh) || refreshStarted) return;
        if (!allSurfacesUnmapped()) {
            if (Core::MediaClock::now() >= controlsDeadline)
                fail(QStringLiteral("The recording surfaces did not unmap before the capture deadline. No recording was started."));
            return;
        }
        controlsTimer.stop();
        if (waitingRefresh) {
            if (!externalStopAvailable || !screenCast) {
                fail(QStringLiteral("The recording stop action or authorized source is no longer available")); return;
            }
            refreshStarted = true;
            if (!screenCast->refresh()) fail(QStringLiteral("The authorized recording source could not begin its capture refresh"));
        } else startSession(rec::desktopRecordingBackend(), pending);
    }
    void authorize(rec::BackendRequest request)
    {
#ifdef Q_OS_WIN
        Q_UNUSED(request);
        fail(QStringLiteral("This selection has no physical Windows desktop coordinates"));
#else
        waitingPermission = true; pending = request;
        screenCast = std::make_unique<cap::ScreenCastSource>();
        QObject::connect(screenCast.get(), &cap::ScreenCastSource::failed, owner, [this](const cap::Failure& error) {
            if (session && session->busy()) return; // the same source error reaches its worker
            if (!waitingPermission && !waitingRefresh) return;
            if (error.code == cap::Failure::Code::Cancelled) finishJourney(); else fail(error.detail);
        });
        QObject::connect(screenCast.get(), &cap::ScreenCastSource::stopped, owner, [this] {
            notify();
            if (!deferredStart) return;
            QTimer::singleShot(0, owner, [q = QPointer(owner)] {
                if (!q || !q->state_->journey || !q->state_->deferredStart) return;
                const auto request = *q->state_->deferredStart;
                q->state_->deferredStart.reset(); q->state_->start(request);
            });
        });
        QObject::connect(screenCast.get(), &cap::ScreenCastSource::refreshed, owner, [this](const Core::FrameLease& frame) {
            if (closing || !waitingRefresh || !refreshStarted) return;
            // Only a fresh connection may change generation. The selected
            // source, format and pixel transform must retain their exact meaning.
            if (!externalStopAvailable || !allSurfacesUnmapped() || !frame ||
                rec::normalizedFrameSize(frame) != std::optional(sourceSize) || frame.format() != sourceContract.format ||
                frame.metadata().sessionId != sourceContract.preview.sessionId ||
                frame.metadata().sourceId != sourceContract.preview.sourceId ||
                frame.metadata().crop != sourceContract.preview.crop || frame.metadata().transform != sourceContract.preview.transform) {
                fail(QStringLiteral("The selected source or recording controls changed during capture refresh")); return;
            }
            sourceContract = {frame.format(), frame.metadata(), frame.formatGeneration(), sourceSize};
            // refreshed borrows the producer's retained first frame. Let all
            // direct signal handlers return before the worker can consume it.
            QTimer::singleShot(0, owner, [q = QPointer(owner)] {
                if (!q) return;
                auto& state = *q->state_;
                if (state.closing || !state.waitingRefresh || !state.refreshStarted) return;
                if (!state.externalStopAvailable || !state.allSurfacesUnmapped() || !state.screenCast) {
                    state.fail(QStringLiteral("The recording controls or stop action changed before capture admission")); return;
                }
                auto source = state.screenCast->takeFrameSource();
                if (!source) { state.fail(QStringLiteral("The refreshed recording stream is no longer available")); return; }
                state.startSession(rec::frameRecordingBackend(std::move(source), state.sourceContract), state.pending);
            });
        });
        QObject::connect(screenCast.get(), &cap::ScreenCastSource::ready, owner, [this](const Core::FrameLease& frame) {
            if (closing || !waitingPermission) return;
            const auto dimensions = rec::normalizedFrameSize(frame);
            if (!dimensions) { fail(QStringLiteral("The authorized recording source has invalid dimensions")); return; }
            sourceSize = *dimensions;
            sourceContract = {frame.format(), frame.metadata(), frame.formatGeneration(), sourceSize};
            QImage image(sourceSize.first, sourceSize.second, QImage::Format_ARGB32);
            if (image.isNull() || !rec::copyFrameBgra(frame, {0, 0, sourceSize.first, sourceSize.second},
                    {image.bits(), static_cast<std::size_t>(image.sizeInBytes())}, static_cast<std::size_t>(image.bytesPerLine()))) {
                fail(QStringLiteral("The authorized recording preview could not be prepared")); return;
            }
            cap::Snapshot snapshot;
            snapshot.image = std::move(image); snapshot.sourceId = frame.metadata().sourceId;
            snapshot.generation = frame.formatGeneration(); snapshot.coordinates = cap::CoordinateSpace::AuthorizedSourcePixels;
            snapshot.sourceName = text(u"toolbar.record", QStringLiteral("Screen recording"));
            selector = std::make_unique<ui::CaptureWindow>(); selector->setTranslator(translator);
            QObject::connect(selector.get(), &ui::CaptureWindow::closed, owner, [this] {
                if (!waitingPermission || waitingRefresh) return;
                waitingPermission = false;
                if (screenCast) screenCast->stop();
                finishJourney();
            });
            QObject::connect(selector.get(), &ui::CaptureWindow::sourceInvalidated, owner, [this](const auto& error) { fail(error.detail); });
            QObject::connect(selector->session(), &cap::CaptureSession::actionRequested, owner, [this](const auto& selected) {
                if (!waitingPermission || waitingRefresh || selected.action != cap::Action::Record) return;
                pending.region = pixels(selected.sourceRect);
                if (pending.format == rec::Format::Mp4) {
                    const auto aligned = rec::alignedMp4Region(pending.region, pending.maximumFrameBytes);
                    if (!aligned) { fail(QStringLiteral("The selected MP4 area is too small")); return; }
                    pending.region = *aligned;
                }
                if (!prepareControls(true)) return;
                if (auto* view = selector->dialogParent(); view && view->windowHandle())
                    hiddenSurfaces.push_back(view->windowHandle());
                waitingRefresh = true; refreshStarted = false; pending.controls = controls;
                selector->hideForAction();
                selector->session()->complete(selected.token, cap::ActionResult::Completed);
                controlsDeadline = Core::MediaClock::now() + std::chrono::seconds(2);
                controlsTimer.start();
                const QPointer<RecordingController> alive(owner);
                admitAfterUnmap(); if (alive) notify();
            });
            const auto error = selector->showSnapshot(std::move(snapshot), {cap::Action::Record});
            if (error) { fail(error.detail); return; }
            selector->session()->setCapability(cap::Action::Record, {cap::CapabilityState::Available, {}});
        });
        if (!window) { fail(QStringLiteral("The recording permission request has no visible parent")); return; }
        window->show(); window->winId();
        const QPointer<RecordingController> alive(owner);
        if (!screenCast->start(window->windowHandle(), request.cursor, request.framesPerSecond))
            fail(QStringLiteral("The recording permission request could not start"));
        if (alive) notify();
#endif
    }
    void start(rec::BackendRequest request)
    {
        if (closing || outputPending || waitingPermission || preparingDesktop || (session && session->busy())) return;
        if (window) window->setState(rec::SessionState::Preparing);
        if (screenCast && screenCast->busy()) { deferredStart = request; notify(); return; }
        if (needsPermission(selection)) { authorize(request); return; }
        request.region = pixels(*selection.desktopRect);
        if (request.format == rec::Format::Mp4) {
            const auto aligned = rec::alignedMp4Region(request.region, request.maximumFrameBytes);
            if (!aligned) { fail(QStringLiteral("The selected MP4 area is too small")); return; }
            request.region = *aligned;
        }
        if (!prepareControls(false)) return;
        request.controls = controls; pending = std::move(request); preparingDesktop = true;
        controlsDeadline = Core::MediaClock::now() + std::chrono::seconds(2);
        controlsTimer.start();
        const QPointer<RecordingController> alive(owner);
        admitAfterUnmap(); if (alive) notify();
    }
};

RecordingController::RecordingController(std::shared_ptr<Media::MediaArchiveService> archive,
    MediaWorkspace& workspace, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), workspace))
{
    connect(&state_->timer, &QTimer::timeout, this, [this] {
        if (state_->session && state_->session->busy() && state_->window)
            state_->window->setElapsed(state_->session->elapsed());
    });
}
RecordingController::~RecordingController()
{
    state_->closing = true; state_->controlsTimer.stop();
    state_->workspace.cancelOutput(this, state_->session ? state_->session->token() : 0);
    state_->session.reset();
    if (state_->selector) state_->selector->shutdown();
    if (state_->screenCast && state_->screenCast->busy()) {
        auto* remaining = state_->screenCast.release();
        disconnect(remaining, nullptr, this, nullptr);
        connect(remaining, &cap::ScreenCastSource::stopped, remaining, &QObject::deleteLater);
        remaining->stop();
    }
    state_->guardingControls = false; state_->controls.reset();
    state_->outlines.clear();
    delete state_->window.data();
}
void RecordingController::setTranslator(ui::DesktopTranslator translator)
{
    state_->translator = std::move(translator);
    if (state_->window) state_->window->setTranslator(state_->translator);
    if (state_->selector) state_->selector->setTranslator(state_->translator);
}
bool RecordingController::begin(const cap::ActionRequest& selection, QWidget* parent)
{
    if (QThread::currentThread() != thread() || state_->closing || !state_->archive || selection.action != cap::Action::Record)
        return false;
    if (state_->busy()) { activate(); return false; }
    const auto region = selection.desktopRect.value_or(selection.sourceRect);
    if (region.isEmpty()) return false;
    if (state_->window) { disconnect(state_->window, nullptr, this, nullptr); delete state_->window.data(); }
    state_->outlines.clear(); state_->session.reset(); state_->controlsFailure.clear(); state_->completedResult.reset();
    state_->selection = selection; state_->journey = true;
    state_->window = new ui::RecordingWindow(); state_->window->installEventFilter(this);
    state_->window->setTranslator(state_->translator); state_->window->setExternalStopHint(state_->externalStopHint);
    const cap::Capability available{cap::CapabilityState::Available, {}};
    state_->window->setCapabilities({available, available, available, available, true});
    connect(state_->window, &ui::RecordingWindow::startRequested, this, [this](const ui::RecordingOptions& options) {
        auto request = rec::recordingDefaults(options.format);
        request.systemAudio = options.format == rec::Format::Mp4 && options.systemAudio;
        request.microphone = options.format == rec::Format::Mp4 && options.microphone;
        state_->start(std::move(request));
    });
    connect(state_->window, &ui::RecordingWindow::stopRequested, this, [this](rec::StopIntent intent) { finish(intent); });
    connect(state_->window, &ui::RecordingWindow::retryOutputRequested, this, [this] { state_->output(); });
    connect(state_->window, &ui::RecordingWindow::cancelRequested, this, [this] {
        if (!state_->outputPending && !state_->waitingPermission && (!state_->session || !state_->session->busy()))
            state_->finishJourney();
    });
    connect(state_->window, &ui::RecordingWindow::capturePlacementLost, this, [this] {
        if (!state_->guardingControls || !state_->window) return;
        state_->window->setHiddenForCapture(true); state_->publishControls();
        if (!state_->externalStopAvailable)
            state_->fail(state_->text(u"capture.stopUnavailable", QStringLiteral("The recording controls cannot remain safely accessible.")));
    });
    connect(state_->window, &QObject::destroyed, this, [this] {
        if (!state_->journey || state_->closing) return;
        if (state_->session && state_->session->busy()) {
            if (state_->controls) state_->controls->snapshot.store({}, std::memory_order_release);
            state_->session->shutdown();
        } else if (state_->outputPending) {
            state_->workspace.cancelOutput(this, state_->session ? state_->session->token() : 0);
        } else finish(rec::StopIntent::Save);
    });
    if (!needsPermission(selection)) {
        for (const auto& binding : selection.screens) {
            if (!binding.screen || !binding.sourcePixels.intersects(selection.sourceRect)) continue;
            auto outline = std::make_unique<RecordingOutline>(binding, selection.sourceRect);
            if (!outline->hasBorder()) continue;
            outline->installEventFilter(this); outline->winId(); state_->protect(outline->windowHandle());
#ifdef Q_OS_WIN
            // Without modern capture exclusion, retain only the ordinary
            // outside toolbar. A full-screen outline's shaped bounds are not
            // a rectangular Win32 readback exclusion contract.
            if (outline->windowHandle()->property("huhuCaptureExcluded").toBool()) outline->show();
#else
            outline->show(); // X11 captureGuarded verifies the actual SHAPE region.
#endif
            state_->outlines.push_back(std::move(outline));
        }
    }
    if (parent && parent->screen()) {
        const auto area = parent->screen()->availableGeometry();
        state_->window->move(area.center() - state_->window->rect().center());
    }
    state_->window->winId(); state_->protect(state_->window->windowHandle());
    state_->window->show(); state_->window->raise(); state_->notify(); return true;
}
void RecordingController::activate(const QString& activationToken)
{
    if (QThread::currentThread() != thread() || state_->closing || !state_->busy()) return;
    QWidget* view = state_->selector && state_->selector->visible() ? state_->selector->dialogParent() : state_->window.data();
    if (!view || (view == state_->window && state_->window->hiddenForCapture())) return;
    view->showNormal(); view->raise(); view->winId();
    if (view == state_->window) state_->protect(view->windowHandle());
    if (auto* native = view->windowHandle()) {
        Platform::applyActivationToken(native, activationToken);
        native->requestActivate();
    }
}
void RecordingController::setExternalStopAvailable(bool available, const QString& hint)
{
    if (QThread::currentThread() != thread() || state_->closing) return;
    state_->externalStopAvailable = available; state_->externalStopHint = hint;
    if (state_->window) state_->window->setExternalStopHint(hint);
    if (!available && state_->guardingControls && state_->window && state_->window->hiddenForCapture())
        state_->fail(state_->text(u"capture.stopUnavailable", QStringLiteral("The external recording stop action is no longer available.")));
}
bool RecordingController::finish(rec::StopIntent intent)
{
    if (QThread::currentThread() != thread() || state_->closing) return false;
    if (state_->session && state_->session->busy()) return state_->session->requestStop(intent);
    if (!state_->journey || state_->outputPending) return false;
    state_->waitingPermission = state_->waitingRefresh = state_->preparingDesktop = false;
    state_->controlsTimer.stop();
    if (state_->selector) state_->selector->shutdown();
    if (state_->screenCast) state_->screenCast->stop();
    state_->finishJourney(); return true;
}
bool RecordingController::busy() const noexcept { return state_->busy(); }
void RecordingController::shutdown()
{
    if (QThread::currentThread() != thread() || state_->closing) return;
    state_->closing = true; state_->journey = state_->waitingPermission = state_->waitingRefresh = state_->preparingDesktop = false;
    state_->deferredStart.reset(); state_->controlsTimer.stop();
    if (state_->selector) state_->selector->shutdown();
    if (state_->outputPending) {
        state_->workspace.cancelOutput(this, state_->session ? state_->session->token() : 0);
        state_->outputPending = false;
    }
    if (state_->session && state_->session->busy()) state_->session->shutdown();
    else if (state_->screenCast) state_->screenCast->stop();
    if (state_->window) state_->window->hide();
    for (const auto& outline : state_->outlines) outline->hide();
    state_->notify();
}
bool RecordingController::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::PlatformSurface) {
        const auto phase = static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType();
        if (phase == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed && state_->guardingControls) {
            if (state_->controls) state_->controls->snapshot.store({}, std::memory_order_release);
            state_->fail(QStringLiteral("A recording control surface was replaced during capture; the media is retained."));
        } else if (phase == QPlatformSurfaceEvent::SurfaceCreated) {
            if (auto* native = qobject_cast<QWindow*>(watched)) state_->protect(native);
        }
    } else if (event->type() == QEvent::WinIdChange) {
        if (state_->guardingControls) {
            if (state_->controls) state_->controls->snapshot.store({}, std::memory_order_release);
            state_->fail(QStringLiteral("A recording control identifier changed during capture; the media is retained."));
        } else if (auto* widget = qobject_cast<QWidget*>(watched)) state_->protect(widget->windowHandle());
    }
    return QObject::eventFilter(watched, event);
}

} // namespace HuhuCapture::Application
