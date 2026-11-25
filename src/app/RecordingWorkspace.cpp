#include "app/RecordingWorkspace.h"

#include "app/CaptureWorkspace.h"
#include "app/MediaWorkspace.h"
#include "capture/CaptureSession.h"
#include "recording/RecordingSession.h"
#include "services/capture/LiveCaptureSource.h"
#include "services/desktop/CaptureExclusion.h"
#include "services/desktop/WindowActivation.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace HuhuCapture::Application {
namespace {
namespace cap = Capture;
namespace rec = Recording;
namespace ui = UI::Qt;

// Four outside work-area candidates reuse this repository's PR #7 controller
// (251bc0071278c6b711c840bf5188d16870bc7349), with the same verified per-screen
// mapping. Never scale a virtual desktop or a Portal source by one global DPR.
QVector<QRect> outsideAreas(const QVector<cap::ScreenBinding>& screens, QRect sourceSelection)
{
    QVector<QRect> result;
    for (const auto& binding : screens) {
        if (!binding.screen || binding.screen->name().compare(binding.nativeId, Qt::CaseInsensitive) != 0 ||
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

struct RecordingWorkspace::State {
    RecordingWorkspace* owner;
    MediaWorkspace& media;
    CaptureWorkspace& capture;
    rec::RecordingSession session;
    rec::LiveBackendFactory liveBackend;
    ui::RecordingCapabilities capabilities;
    ui::DesktopTranslator translator;
    Desktop::WindowActivation activation;
    QPointer<ui::RecordingWindow> window;
    std::vector<std::unique_ptr<RecordingOutline>> outlines;
    QTimer timer;
    std::optional<Media::Entry> archived;
    rec::StopIntent intent{rec::StopIntent::Save};
    QRect region;
    quint64 captureToken{}, recordingToken{}, sourceId{}, generation{}, outputToken{}, nextOutputToken{1};
    QString sourceFailure;
    std::stop_source producerStop{std::nostopstate};
    std::shared_ptr<Core::CaptureControls> controls;
    QVector<cap::ScreenBinding> screens;
    QRect sourceRegion;
    bool hasBackend{}, fromLive{}, consumerClaimed{}, stopping{}, sessionStopped{}, shutdownReported{}, cancelled{};
    bool externalStopAvailable{};
    bool guardingControls{};

    State(RecordingWorkspace* q, std::shared_ptr<Media::MediaArchiveService> archive,
        MediaWorkspace& mediaWorkspace, CaptureWorkspace& captureWorkspace, rec::Backend backend,
        ui::RecordingCapabilities available, rec::LiveBackendFactory liveFactory)
        : owner(q), media(mediaWorkspace), capture(captureWorkspace), session(std::move(archive), backend),
          liveBackend(std::move(liveFactory)), capabilities(std::move(available)), hasBackend(bool(backend))
    {
        timer.setInterval(250);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] {
            if (window) window->setElapsed(session.elapsed());
        });
    }
    bool busy() const noexcept { return captureToken || session.busy() || outputToken || window; }
    void changed()
    { QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->busyChanged(target->busy()); }); }
    void updateCapability()
    {
        cap::Capability available{cap::CapabilityState::Unavailable, QStringLiteral("A recording backend is unavailable")};
        const auto liveCapability = capture.liveSource().capability();
        if (!stopping && (hasBackend || (liveBackend && liveCapability.canRequest()))) {
            if (capabilities.mp4.canRequest()) available = capabilities.mp4;
            else available = capabilities.gif;
            if (!hasBackend && available.canRequest()) available = liveCapability;
        }
        capture.setLiveCapability(cap::Action::Record, std::move(available));
    }
    void releaseConsumer()
    {
        if (!std::exchange(consumerClaimed, false)) return;
        // This runs after the session's worker has returned and released the
        // provider, including failures before its encoder was ever invoked.
        producerStop.request_stop();
        capture.liveSource().consumerFinished(sourceId, generation);
        producerStop = std::stop_source{std::nostopstate};
    }
    void protect(QWindow* window)
    {
        if (!window) return;
        window->installEventFilter(owner);
        // The adapter preserves unsupported capability. It never substitutes
        // a black rectangle or claims Linux has a universal exclusion API.
        const auto result = Desktop::excludeFromCapture(window);
        window->setProperty("huhuCaptureExcluded", result.applied);
        if (this->window && window == this->window->windowHandle())
            this->window->setCaptureExcluded(result.applied);
    }
    bool publishControls()
    {
        if (!guardingControls || !controls || !window || !window->windowHandle()) return false;
        auto snapshot = std::make_shared<Core::CaptureControlSnapshot>();
        const auto add = [&](QWidget& widget, bool hidden) {
            auto* native = widget.windowHandle();
            if (!native || !native->handle()) return false;
            snapshot->surfaces.push_back({static_cast<std::uintptr_t>(native->winId()), hidden ? Core::CaptureControlPlacement::Hidden :
                native->property("huhuCaptureExcluded").toBool() ? Core::CaptureControlPlacement::Excluded : Core::CaptureControlPlacement::Outside});
            return true;
        };
        if (!add(*window, window->hiddenForCapture())) return false;
        for (const auto& outline : outlines)
            if (outline->hasBorder() && !add(*outline, !outline->isVisible())) return false;
        controls->snapshot.store(std::move(snapshot), std::memory_order_release);
        return true;
    }
    bool prepareControls()
    {
        if (!window || !window->windowHandle()) return false;
        // A tooltip is a separate native top-level, rather than a child of
        // the toolbar's exclusion/shape. Unmap only this process's actual
        // tooltips before publishing the capture surface contract.
        for (auto* tooltip : QApplication::topLevelWidgets())
            if (tooltip->windowType() == Qt::ToolTip) tooltip->hide();
        protect(window->windowHandle());
        const bool excluded = window->windowHandle()->property("huhuCaptureExcluded").toBool();
        bool outside{};
        if (!excluded && !fromLive && capabilities.guardedOutsideControls) {
            auto areas = outsideAreas(screens, sourceRegion);
            outside = !areas.isEmpty() && window->setCaptureSafeAreas(std::move(areas));
            if (outside) window->show();
        }
        if (!excluded && !outside) {
            window->setCaptureSafeAreas({});
            if (!externalStopAvailable) {
                showFailure(QStringLiteral("No recording control fits outside the selected pixels, and no tray or global stop action is currently available."));
                return false;
            }
            window->setHiddenForCapture(true);
        }
        controls = std::make_shared<Core::CaptureControls>(); guardingControls = true;
        if (!publishControls()) {
            guardingControls = false; controls.reset();
            window->setHiddenForCapture(false);
            showFailure(QStringLiteral("The recording controls have no verifiable native surfaces")); return false;
        }
        return true;
    }
    void restoreControls()
    {
        guardingControls = false; controls.reset();
        if (window) { window->setCaptureSafeAreas({}); window->setHiddenForCapture(false); }
    }
    void hideOutlines()
    { for (const auto& outline : outlines) outline->hide(); }
    void finishJourney()
    {
        const auto completed = std::exchange(captureToken, 0);
        recordingToken = 0; archived.reset(); sourceFailure.clear();
        timer.stop(); outlines.clear();
        guardingControls = false; controls.reset(); screens.clear(); sourceRegion = {};
        if (window) {
            auto* old = window.data(); window.clear();
            QObject::disconnect(old, nullptr, owner, nullptr);
            old->hide(); old->deleteLater();
        }
        const QPointer<RecordingWorkspace> alive(owner);
        if (completed) capture.closeLiveAction(completed);
        if (!alive) return;
        changed();
        if (completed) QTimer::singleShot(0, owner, [target = owner, completed] { Q_EMIT target->finished(completed); });
    }
    void finishShutdown()
    {
        if (!stopping || !sessionStopped || outputToken || shutdownReported) return;
        const QPointer<RecordingWorkspace> alive(owner);
        finishJourney();
        if (!alive) return;
        shutdownReported = true;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->shutdownFinished(); });
    }
    void showFailure(const QString& detail, const QString& retainedPath = {}, bool retry = false)
    {
        hideOutlines(); timer.stop();
        restoreControls();
        if (window) window->showFailure(detail, retainedPath, retry);
        else {
            QTimer::singleShot(0, owner, [target = owner, detail, retainedPath] {
                Q_EMIT target->operationFailed(detail + (retainedPath.isEmpty() ? QString() : QChar(u'\n') + retainedPath));
            });
            finishJourney();
        }
    }
    void output()
    {
        if (stopping || outputToken || !archived || !window || !nextOutputToken) return;
        const auto token = nextOutputToken++;
        outputToken = token; window->setOutputPending(true);
        const QPointer<RecordingWorkspace> alive(owner);
        const bool accepted = media.outputArchived(owner, window, token,
            intent == rec::StopIntent::Copy ? ui::OutputIntent::Clipboard : ui::OutputIntent::SaveAs,
            *archived, [this, token](ui::OutputResult result, const QString& detail) {
                if (outputToken != token) return;
                outputToken = 0;
                if (stopping) { finishShutdown(); return; }
                const QPointer<RecordingWorkspace> alive(owner);
                if (result == ui::OutputResult::Failed) showFailure(detail, archived ? archived->path : QString(), true);
                else finishJourney(); // Cancelling Save As preserves the durable recording.
                if (alive) changed();
            });
        if (!alive) return;
        if (!accepted) {
            outputToken = 0;
            showFailure(QStringLiteral("The media output service could not accept this recording"), archived->path, true);
        }
        changed();
    }
    void start(const ui::RecordingOptions& options)
    {
        if (stopping || !captureToken || session.busy() || recordingToken || !window) return;
        if (!capture.sourceIsCurrent(sourceId, generation)) {
            showFailure(QStringLiteral("The selected desktop changed before recording started. Capture a new region.")); return;
        }
        const auto capability = options.format == rec::Format::Mp4 ? capabilities.mp4 : capabilities.gif;
        if ((!fromLive && !hasBackend) || (fromLive && !liveBackend) || !capability.canRequest() ||
            (options.systemAudio && !capabilities.systemAudio.canRequest()) ||
            (options.microphone && !capabilities.microphone.canRequest())) {
            showFailure(QStringLiteral("The selected recording format or audio source is unavailable")); return;
        }
        rec::BackendRequest request;
        request.format = options.format;
        request.region = {region.x(), region.y(), region.width(), region.height()};
        request.framesPerSecond = options.format == rec::Format::Gif ? 16 : 30;
        request.maximumDuration = options.format == rec::Format::Gif ? std::chrono::minutes(6) : std::chrono::minutes(120);
        request.systemAudio = options.format == rec::Format::Mp4 && options.systemAudio;
        request.microphone = options.format == rec::Format::Mp4 && options.microphone;
        if (!prepareControls()) return;
        request.controls = controls;
        quint64 token{};
        const QPointer<RecordingWorkspace> alive(owner);
        if (fromLive) {
            request.coordinates = rec::CoordinateSpace::AuthorizedSourcePixels;
            request.sourceId = sourceId; request.generation = generation;
            auto consumer = capture.liveSource().claim(sourceId, generation,
                {static_cast<std::uint32_t>(region.x()), static_cast<std::uint32_t>(region.y()),
                 static_cast<std::uint32_t>(region.width()), static_cast<std::uint32_t>(region.height())});
            if (!consumer) {
                showFailure(QStringLiteral("The authorized recording source is no longer available. Select a new source.")); return;
            }
            producerStop = consumer->stop; consumerClaimed = true;
            try {
                token = session.start(request, liveBackend(std::move(*consumer)));
            } catch (...) {
                consumer.reset();
                if (alive) releaseConsumer();
                if (alive) showFailure(QStringLiteral("The recording source could not be admitted within the available resources"));
                return;
            }
            consumer.reset();
            if (alive && !token) releaseConsumer();
        } else token = session.start(request);
        if (!alive) return;
        if (!token) { showFailure(QStringLiteral("The recording session could not accept this request")); return; }
        recordingToken = token; changed();
    }
    void open(const cap::ActionRequest& request)
    {
        if (request.action != cap::Action::Record) return;
        const auto& snapshot = capture.session().snapshot();
        if (stopping || captureToken || session.busy() ||
            !request.token || capture.session().activeRequest() != request.token ||
            request.sourceId != snapshot.sourceId || request.generation != snapshot.generation ||
            !capture.sourceIsCurrent(request.sourceId, request.generation)) {
            capture.completeLiveAction(request.token, cap::ActionResult::Failed,
                QStringLiteral("The recording source is busy or has changed")); return;
        }
        // Source-local recording is admitted only after this same authorized
        // stream's first frame has been selected by the shared capture session.
        // An earlier Portal screenshot cannot establish this pixel domain.
        const bool authorized = request.coordinates == cap::CoordinateSpace::AuthorizedSourcePixels;
        if ((authorized && (!liveBackend || !capture.liveSource().isCurrent(request.sourceId, request.generation))) ||
            (!authorized && (!hasBackend || !request.desktopRect))) {
            capture.completeLiveAction(request.token, cap::ActionResult::Failed,
                QStringLiteral("The recording request has no matching live or native desktop source")); return;
        }
        captureToken = request.token; sourceId = request.sourceId; generation = request.generation;
        fromLive = authorized; region = authorized ? request.sourceRect : *request.desktopRect;
        sourceRegion = request.sourceRect; screens = snapshot.screens;
        cancelled = false; archived.reset(); sourceFailure.clear();
        window = new ui::RecordingWindow;
        window->installEventFilter(owner);
        window->setTranslator(translator); window->setCapabilities(capabilities);
        const cap::ScreenBinding* placement{}; qint64 largest{};
        for (const auto& binding : snapshot.screens) {
            if (fromLive) break; // No inferred global outline for a Portal source.
            if (!binding.screen) continue;
            const auto intersection = request.sourceRect.intersected(binding.sourcePixels);
            if (intersection.isEmpty()) continue;
            outlines.push_back(std::make_unique<RecordingOutline>(binding, request.sourceRect));
            const auto area = static_cast<qint64>(intersection.width()) * intersection.height();
            if (area > largest) { largest = area; placement = &binding; }
        }
        if (placement) {
            const auto intersection = request.sourceRect.intersected(placement->sourcePixels);
            const auto first = cap::screenLocalFromSource(*placement, intersection.topLeft()) + placement->logicalGeometry.topLeft();
            const auto end = cap::screenLocalFromSource(*placement,
                QPointF(intersection.x() + intersection.width(), intersection.y() + intersection.height())) + placement->logicalGeometry.topLeft();
            window->setPlacement(placement->screen, QRectF(first, end), placement->logicalWorkArea);
        }
        QObject::connect(window, &ui::RecordingWindow::startRequested, owner, [this](const auto& selected) { start(selected); });
        QObject::connect(window, &ui::RecordingWindow::stopRequested, owner, [this](rec::StopIntent choice) {
            owner->stopActive(choice);
        });
        QObject::connect(window, &ui::RecordingWindow::capturePlacementLost, owner, [this] {
            if (!window || !guardingControls) return;
            // The widget already unmapped itself before reporting that its
            // new size cannot fit. Keep it hidden until native capture ends.
            window->setHiddenForCapture(true);
            publishControls();
            if (!externalStopAvailable) owner->stopActive(rec::StopIntent::Save);
            changed();
        });
        QObject::connect(window, &ui::RecordingWindow::retryOutputRequested, owner, [this] { output(); });
        QObject::connect(window, &ui::RecordingWindow::cancelRequested, owner, [this] {
            if (!session.busy() && !outputToken) finishJourney();
        });
        QObject::connect(window, &QObject::destroyed, owner, [this] {
            if (!captureToken || stopping) return;
            // Unexpected owner loss requests safe finalization/recovery, not an
            // implicit discard. It cannot destroy a running native encoder.
            if (session.busy()) session.requestStop(rec::StopIntent::Save);
            else if (outputToken) media.cancelOutput(owner, outputToken);
            else finishJourney();
        });
        for (const auto& outline : outlines) if (outline->hasBorder()) {
            outline->installEventFilter(owner); outline->show(); protect(outline->windowHandle());
        }
        window->show(); protect(window->windowHandle());
        activation.request(window->windowHandle()); changed();
    }
};

RecordingWorkspace::RecordingWorkspace(std::shared_ptr<Media::MediaArchiveService> archive,
    MediaWorkspace& media, CaptureWorkspace& capture, rec::Backend backend,
    ui::RecordingCapabilities capabilities, rec::LiveBackendFactory liveBackend, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), media, capture,
          std::move(backend), std::move(capabilities), std::move(liveBackend)))
{
    connect(&capture.liveSource(), &cap::LiveCaptureSource::initialized, this,
        [this](const auto&, const auto&) { state_->updateCapability(); });
    connect(&capture.liveSource(), &cap::LiveCaptureSource::capabilityChanged, this,
        [this](const auto&) { state_->updateCapability(); });
    connect(&capture, &CaptureWorkspace::liveActionRequested, this,
        [this](const cap::ActionRequest& request, QWidget*) { state_->open(request); });
    connect(&capture, &CaptureWorkspace::liveActionActivationRequested, this, [this](quint64 token, const QString& activationToken) {
        if (token == state_->captureToken && state_->window && !state_->window->hiddenForCapture())
            state_->activation.request(state_->window->windowHandle(), activationToken);
    });
    connect(&capture, &CaptureWorkspace::liveActionCancelled, this, [this](quint64 token) {
        if (state_->stopping || token != state_->captureToken) return;
        state_->cancelled = true;
        if (state_->session.busy()) state_->session.requestStop(rec::StopIntent::Discard);
        else if (state_->outputToken) state_->media.cancelOutput(this, state_->outputToken);
        else state_->finishJourney();
    });
    connect(&capture, &CaptureWorkspace::sourceInvalidated, this, [this](quint64 source, quint64 generation, const auto& failure) {
        if (state_->stopping || !state_->captureToken || source != state_->sourceId || generation != state_->generation) return;
        const auto phase = state_->session.state();
        if (state_->recordingToken && phase != rec::SessionState::Preparing &&
            phase != rec::SessionState::Recording && phase != rec::SessionState::Stopping) return;
        state_->sourceFailure = failure.detail.isEmpty() ? QStringLiteral("The recording source changed") : failure.detail;
        state_->hideOutlines();
        if (state_->session.busy()) state_->session.requestStop(rec::StopIntent::Save);
        else if (!state_->outputToken) state_->showFailure(state_->sourceFailure, state_->archived ? state_->archived->path : QString(), bool(state_->archived));
    });
    connect(&state_->session, &rec::RecordingSession::stateChanged, this, [this](quint64, rec::SessionState state) {
        if (state_->window) state_->window->setState(state);
        if (state == rec::SessionState::Finalizing || state == rec::SessionState::Archiving) {
            state_->timer.stop(); state_->hideOutlines();
            // Every backend reports Finalizing only after its actual source
            // stopped; it is now safe to restore a hidden output/status view.
            if (!state_->stopping && !state_->cancelled) state_->restoreControls();
            if (state_->window) state_->window->setElapsed(state_->session.elapsed());
        }
        state_->changed();
    });
    connect(&state_->session, &rec::RecordingSession::prepared, this, [this](quint64 token, const auto&) {
        if (state_->stopping || token != state_->recordingToken) return;
        state_->timer.start();
        if (state_->window) state_->window->setElapsed(state_->session.elapsed());
    });
    connect(&state_->session, &rec::RecordingSession::finished, this, [this](const rec::SessionResult& result) {
        if (result.token != state_->recordingToken) return;
        state_->releaseConsumer();
        state_->timer.stop(); state_->hideOutlines();
        if (state_->window) state_->window->setElapsed(result.backend.elapsed);
        const QPointer<RecordingWorkspace> alive(this);
        if (!result.archive.path.isEmpty()) state_->media.notifyArchiveChanged();
        if (!alive) return;
        if (state_->stopping) {
            // shutdownFinished still owns the final drain, but closing the
            // application cannot swallow a failed writer/finalizer or the
            // location of an unpublished recording retained for recovery.
            const auto retained = result.archive.path.isEmpty() ? result.recoveryPath : result.archive.path;
            if (result.backend.media.firstError || result.backend.media.finalizationError ||
                (!retained.isEmpty() && result.state == rec::SessionState::RecoverableFailure) ||
                (result.failure && result.failure.code != Media::Error::Cancelled)) {
                auto detail = result.failure.detail;
                if (detail.isEmpty()) detail = QStringLiteral("The recording could not finish cleanly while the application was closing.");
                if (!retained.isEmpty()) detail += QChar(u'\n') + retained;
                Q_EMIT operationFailed(detail);
            }
            return;
        }
        state_->intent = result.intent; state_->archived = result.archivedEntry;
        if (state_->cancelled) { state_->finishJourney(); return; }
        if (result.state == rec::SessionState::Completed && state_->archived) {
            if (!state_->sourceFailure.isEmpty()) state_->showFailure(state_->sourceFailure, state_->archived->path, true);
            else if (!state_->window) state_->finishJourney();
            else state_->output();
        } else if (result.state == rec::SessionState::Cancelled || result.state == rec::SessionState::Discarded)
            state_->finishJourney();
        else state_->showFailure(result.failure.detail,
            result.archive.path.isEmpty() ? result.recoveryPath : result.archive.path, false);
        if (alive) state_->changed();
    });
    connect(&state_->session, &rec::RecordingSession::shutdownFinished, this, [this] {
        state_->sessionStopped = true; state_->finishShutdown();
    });
    state_->updateCapability();
}
RecordingWorkspace::~RecordingWorkspace()
{
    shutdown();
    // State's control snapshot is declared after the outlines and would be
    // destroyed first. Native surface destruction still dispatches our event
    // filter until QObject's base destructor removes it. Tear down these owned
    // windows while the complete State exists, including abnormal owner loss
    // before the asynchronous session has reported shutdownFinished.
    state_->guardingControls = false;
    const auto detach = [this](QWidget* widget) {
        if (!widget) return;
        widget->removeEventFilter(this);
        if (auto* native = widget->windowHandle()) native->removeEventFilter(this);
    };
    for (const auto& outline : state_->outlines) detach(outline.get());
    state_->outlines.clear();
    if (state_->window) {
        auto* window = state_->window.data(); detach(window);
        disconnect(window, nullptr, this, nullptr); delete window;
    }
}
void RecordingWorkspace::setTranslator(ui::DesktopTranslator translator)
{ state_->translator = std::move(translator); if (state_->window) state_->window->setTranslator(state_->translator); }
bool RecordingWorkspace::busy() const noexcept { return state_->busy(); }
void RecordingWorkspace::setExternalStopAvailable(bool available)
{
    if (state_->externalStopAvailable == available) return;
    state_->externalStopAvailable = available;
    if (!available && controlsHiddenForCapture() && state_->session.busy())
        stopActive(rec::StopIntent::Save);
    state_->changed();
}
bool RecordingWorkspace::canStop() const noexcept
{
    const auto phase = state_->session.state();
    return !state_->stopping && state_->session.busy() &&
        (phase == rec::SessionState::Preparing || phase == rec::SessionState::Recording);
}
bool RecordingWorkspace::stopActive(rec::StopIntent intent)
{
    if (!canStop() || !state_->session.requestStop(intent)) return false;
    state_->intent = intent; state_->hideOutlines(); state_->changed(); return true;
}
bool RecordingWorkspace::controlsHiddenForCapture() const noexcept
{ return state_->window && state_->window->hiddenForCapture(); }
void RecordingWorkspace::shutdown()
{
    if (state_->stopping) return;
    state_->stopping = true; state_->timer.stop(); state_->hideOutlines();
    const QPointer<RecordingWorkspace> alive(this);
    state_->updateCapability();
    if (!alive) return;
    if (state_->outputToken) state_->media.cancelOutput(this, state_->outputToken);
    if (!alive) return;
    state_->session.shutdown();
    if (alive) state_->finishShutdown();
}
bool RecordingWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::PlatformSurface) {
        const auto phase = static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType();
        if (phase == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed && state_->guardingControls) {
            if (state_->controls) state_->controls->snapshot.store({}, std::memory_order_release);
            stopActive(rec::StopIntent::Save);
        } else if (phase == QPlatformSurfaceEvent::SurfaceCreated) {
            // Set the actual Windows affinity before this native surface can
            // be mapped. A queued setter leaves an unprotected visible frame.
            if (auto* window = qobject_cast<QWindow*>(watched)) state_->protect(window);
        }
    } else if (event->type() == QEvent::WinIdChange && state_->guardingControls) {
        if (state_->controls) state_->controls->snapshot.store({}, std::memory_order_release);
        stopActive(rec::StopIntent::Save);
    }
    return QObject::eventFilter(watched, event);
}

} // namespace HuhuCapture::Application
