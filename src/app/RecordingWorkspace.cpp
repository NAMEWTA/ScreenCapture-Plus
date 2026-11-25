#include "app/RecordingWorkspace.h"

#include "app/CaptureWorkspace.h"
#include "app/MediaWorkspace.h"
#include "capture/CaptureSession.h"
#include "recording/RecordingSession.h"
#include "services/desktop/CaptureExclusion.h"
#include "services/desktop/WindowActivation.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace HuhuCapture::Application {
namespace {
namespace cap = Capture;
namespace rec = Recording;
namespace ui = UI::Qt;

// One transparent, input-free outline per verified screen. Only the outer
// selection edge is drawn; crossing a screen boundary adds no interior edge.
// The stroke lies outside the selected pixels, including on desktops where
// application windows cannot be excluded from capture.
class RecordingOutline final : public QWidget {
public:
    RecordingOutline(const cap::ScreenBinding& binding, QRect selection)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
              Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus), binding_(binding), selection_(selection)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setObjectName(QStringLiteral("recordingOutline"));
        setScreen(binding.screen); setGeometry(binding.logicalGeometry.toAlignedRect());
    }
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
};
}

struct RecordingWorkspace::State {
    RecordingWorkspace* owner;
    MediaWorkspace& media;
    CaptureWorkspace& capture;
    rec::RecordingSession session;
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
    bool hasBackend{}, stopping{}, sessionStopped{}, shutdownReported{}, cancelled{};

    State(RecordingWorkspace* q, std::shared_ptr<Media::MediaArchiveService> archive,
        MediaWorkspace& mediaWorkspace, CaptureWorkspace& captureWorkspace, rec::Backend backend,
        ui::RecordingCapabilities available)
        : owner(q), media(mediaWorkspace), capture(captureWorkspace), session(std::move(archive), backend),
          capabilities(std::move(available)), hasBackend(bool(backend))
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
        if (hasBackend && !stopping) {
            if (capabilities.mp4.canRequest()) available = capabilities.mp4;
            else available = capabilities.gif;
        }
        capture.setLiveCapability(cap::Action::Record, std::move(available));
    }
    void protect(QWindow* window)
    {
        if (!window) return;
        window->installEventFilter(owner);
        // The adapter preserves unsupported capability. It never substitutes
        // a black rectangle or claims Linux has a universal exclusion API.
        const auto result = Desktop::excludeFromCapture(window);
        if (this->window && window == this->window->windowHandle())
            this->window->setCaptureExcluded(result.applied);
    }
    void hideOutlines()
    { for (const auto& outline : outlines) outline->hide(); }
    void finishJourney()
    {
        const auto completed = std::exchange(captureToken, 0);
        recordingToken = 0; archived.reset(); sourceFailure.clear();
        timer.stop(); outlines.clear();
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
        if (!hasBackend || !capability.canRequest() ||
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
        const auto token = session.start(request);
        if (!token) { showFailure(QStringLiteral("The recording session could not accept this request")); return; }
        recordingToken = token; changed();
    }
    void open(const cap::ActionRequest& request)
    {
        if (request.action != cap::Action::Record) return;
        const auto& snapshot = capture.session().snapshot();
        if (stopping || captureToken || session.busy() || !hasBackend ||
            !request.token || capture.session().activeRequest() != request.token ||
            request.sourceId != snapshot.sourceId || request.generation != snapshot.generation ||
            !capture.sourceIsCurrent(request.sourceId, request.generation)) {
            capture.completeLiveAction(request.token, cap::ActionResult::Failed,
                QStringLiteral("The recording source is busy or has changed")); return;
        }
        // A Portal screenshot and a later ScreenCast authorization are distinct
        // sources. Only a provider holding the actual stream may establish their
        // mapping; a source-local bitmap rectangle is never a desktop rectangle.
        if (request.coordinates != cap::CoordinateSpace::DesktopPixels || !request.desktopRect) {
            capture.completeLiveAction(request.token, cap::ActionResult::Failed,
                QStringLiteral("This recording backend requires a verified desktop source")); return;
        }
        captureToken = request.token; sourceId = request.sourceId; generation = request.generation;
        region = *request.desktopRect; cancelled = false; archived.reset(); sourceFailure.clear();
        window = new ui::RecordingWindow;
        window->setTranslator(translator); window->setCapabilities(capabilities);
        const cap::ScreenBinding* placement{}; qint64 largest{};
        for (const auto& binding : snapshot.screens) {
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
            if (session.requestStop(choice)) { intent = choice; hideOutlines(); changed(); }
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
        for (const auto& outline : outlines) { outline->show(); protect(outline->windowHandle()); }
        window->show(); protect(window->windowHandle());
        activation.request(window->windowHandle()); changed();
    }
};

RecordingWorkspace::RecordingWorkspace(std::shared_ptr<Media::MediaArchiveService> archive,
    MediaWorkspace& media, CaptureWorkspace& capture, rec::Backend backend,
    ui::RecordingCapabilities capabilities, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), media, capture,
          std::move(backend), std::move(capabilities)))
{
    connect(&capture, &CaptureWorkspace::liveActionRequested, this,
        [this](const cap::ActionRequest& request, QWidget*) { state_->open(request); });
    connect(&capture, &CaptureWorkspace::liveActionActivationRequested, this, [this](quint64 token, const QString& activationToken) {
        if (token == state_->captureToken && state_->window)
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
        state_->timer.stop(); state_->hideOutlines();
        if (state_->window) state_->window->setElapsed(result.backend.elapsed);
        const QPointer<RecordingWorkspace> alive(this);
        if (!result.archive.path.isEmpty()) state_->media.notifyArchiveChanged();
        if (!alive) return;
        if (state_->stopping) return; // shutdownFinished owns the final drain.
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
    if (state_->window) {
        auto* window = state_->window.data(); disconnect(window, nullptr, this, nullptr); delete window;
    }
}
void RecordingWorkspace::setTranslator(ui::DesktopTranslator translator)
{ state_->translator = std::move(translator); if (state_->window) state_->window->setTranslator(state_->translator); }
bool RecordingWorkspace::busy() const noexcept { return state_->busy(); }
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
    if (event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
        const QPointer<QWindow> window(qobject_cast<QWindow*>(watched));
        QTimer::singleShot(0, this, [this, window] { if (window) state_->protect(window); });
    }
    return QObject::eventFilter(watched, event);
}

} // namespace HuhuCapture::Application
