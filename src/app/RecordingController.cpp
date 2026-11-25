#include "app/RecordingController.h"
#include "app/MediaWorkspace.h"
#include "platform/WindowActivation.h"
#include "recording/FramePixels.h"
#include "recording/PlatformRecording.h"
#include "recording/RecordingSession.h"
#include "services/capture/ScreenCastSource.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/RecordingWindow.h"

#include <QGuiApplication>
#include <QLayout>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <array>
#include <cmath>
#include <utility>

namespace HuhuCapture::Application {
namespace {
namespace rec = Recording;
namespace ui = UI::Qt;
rec::PixelRegion pixels(const QRect& region)
{ return {region.x(), region.y(), region.width(), region.height()}; }
bool needsPermission(const Capture::ActionRequest& selection)
{
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"), ::Qt::CaseInsensitive) ||
        qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), ::Qt::CaseInsensitive) == 0 ||
        selection.coordinates != Capture::CoordinateSpace::DesktopPixels || !selection.desktopRect;
}
bool placeControlsOutside(QWidget& window, const Capture::ActionRequest& selection)
{
    if (needsPermission(selection) || selection.sourceRect.isEmpty() ||
        selection.desktopRect->size() != selection.sourceRect.size()) return false;
    window.ensurePolished();
    if (window.layout()) window.layout()->activate();
    window.resize(window.size().expandedTo(window.sizeHint()));
    for (const auto& binding : selection.screens) {
        if (!binding.screen || binding.screen->name().compare(binding.nativeId,
#ifdef Q_OS_WIN
            ::Qt::CaseInsensitive
#else
            ::Qt::CaseSensitive
#endif
            ) != 0 || binding.logicalGeometry != QRectF(binding.screen->geometry()) ||
            binding.logicalWorkArea != QRectF(binding.screen->availableGeometry()) ||
            binding.logicalWorkArea.isEmpty() || binding.sourcePixels.isEmpty() ||
            !std::isfinite(binding.pixelsPerDipX) || binding.pixelsPerDipX <= 0 ||
            !std::isfinite(binding.pixelsPerDipY) || binding.pixelsPerDipY <= 0) continue;
        const auto selected = QRectF(binding.logicalGeometry.topLeft() +
            Capture::screenLocalFromSource(binding, selection.sourceRect.topLeft()),
            QSizeF(selection.sourceRect.width() / binding.pixelsPerDipX,
                selection.sourceRect.height() / binding.pixelsPerDipY)).adjusted(-8, -8, 8, 8);
        const auto work = binding.logicalWorkArea.adjusted(4, 4, -4, -4);
        const auto frame = QRectF(window.frameGeometry());
        if (work.contains(frame) && !selected.intersects(frame)) return true;
        const auto blocked = selected.intersected(work);
        const std::array<QRectF, 4> spaces = blocked.isEmpty() ? std::array<QRectF, 4>{work, {}, {}, {}} :
            std::array<QRectF, 4>{
                QRectF(work.left(), work.top(), work.width(), blocked.top() - work.top()),
                QRectF(work.left(), blocked.bottom(), work.width(), work.bottom() - blocked.bottom()),
                QRectF(work.left(), work.top(), blocked.left() - work.left(), work.height()),
                QRectF(blocked.right(), work.top(), work.right() - blocked.right(), work.height())};
        for (const auto& space : spaces) {
            if (space.width() < frame.width() || space.height() < frame.height()) continue;
            if (auto* native = window.windowHandle()) native->setScreen(binding.screen);
            window.move(space.topLeft().toPoint());
            const auto actual = QRectF(window.frameGeometry());
            if (work.contains(actual) && !selected.intersects(actual)) return true;
        }
    }
    return false;
}
}

struct RecordingController::State {
    RecordingController* owner;
    std::shared_ptr<Media::MediaArchiveService> archive;
    MediaWorkspace& workspace;
    ui::DesktopTranslator translator;
    std::unique_ptr<rec::RecordingSession> session;
    std::unique_ptr<Capture::ScreenCastSource> screenCast;
    std::unique_ptr<ui::CaptureWindow> selector;
    QPointer<ui::RecordingWindow> window;
    Capture::ActionRequest selection;
    rec::BackendRequest pending;
    std::optional<rec::BackendRequest> deferredStart;
    QTimer timer;
    bool journey{}, closing{}, outputPending{}, waitingPermission{}, shutdownReported{};
    std::pair<int, int> sourceSize;
    rec::FrameSourceContract sourceContract;

    State(RecordingController* value, std::shared_ptr<Media::MediaArchiveService> service, MediaWorkspace& media)
        : owner(value), archive(std::move(service)), workspace(media)
    { timer.setInterval(250); }
    bool busy() const
    { return journey || outputPending || waitingPermission || (session && session->busy()) || (screenCast && screenCast->busy()); }
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
    void fail(const QString& detail)
    {
        waitingPermission = false;
        if (window) window->setState(rec::SessionState::Failed, {}, detail);
        if (selector) selector->shutdown();
        if (screenCast && (!session || !session->busy())) screenCast->stop();
        const QPointer<RecordingController> alive(owner);
        if (!closing) Q_EMIT owner->operationFailed(detail);
        if (alive) notify();
    }
    void finishJourney()
    {
        journey = false; waitingPermission = false; deferredStart.reset();
        if (window) window->hide();
        notify();
    }
    void completed(const rec::SessionResult& result)
    {
        timer.stop();
        const auto retained = !result.archive.path.isEmpty() ? result.archive.path : result.recoveryPath;
        auto message = result.failure.detail;
        if (!result.recoveryPath.isEmpty()) message += QChar(u'\n') + result.recoveryPath;
        if (window) window->setState(result.state, result.backend.elapsed, message, result.archive.path);
        const QPointer<RecordingController> alive(owner);
        if (!result.archive.path.isEmpty()) Q_EMIT owner->archiveChanged();
        if (!alive) return;
        if (closing) { journey = false; notify(); return; }
        if (result.state == rec::SessionState::Completed) {
            if (!result.archivedEntry) { fail(QStringLiteral("The recording has no verified archive identity")); return; }
            outputPending = true; if (window) window->setOutputPending(true);
            const auto intent = result.intent == rec::StopIntent::Copy ? ui::OutputIntent::Clipboard : ui::OutputIntent::SaveAs;
            const auto token = result.token;
            const auto admitted = workspace.outputArchived(owner, window, token, intent, *result.archivedEntry,
                [q = QPointer(owner), result](ui::OutputResult output, const QString& detail) {
                    if (!q) return;
                    auto& state = *q->state_; state.outputPending = false;
                    if (state.window) state.window->setOutputPending(false);
                    if (state.closing) { state.finishJourney(); return; }
                    if (output == ui::OutputResult::Failed) {
                        if (state.window) state.window->setState(rec::SessionState::Completed, result.backend.elapsed, detail, result.archive.path);
                        const QPointer<RecordingController> alive(q);
                        Q_EMIT q->operationFailed(detail + QChar(u'\n') + result.archive.path);
                        if (alive) state.notify();
                    } else state.finishJourney();
                });
            if (!alive) return;
            if (!admitted) {
                outputPending = false; if (window) window->setOutputPending(false);
                fail(QStringLiteral("The recording is saved, but its output action could not start.\n") + retained);
            }
        } else if (result.state == rec::SessionState::Cancelled || result.state == rec::SessionState::Discarded) {
            finishJourney();
        } else {
            Q_EMIT owner->operationFailed(message);
        }
        if (alive) notify();
    }
    void startSession(rec::Backend backend, rec::BackendRequest request)
    {
        if (closing || (session && session->busy())) return;
        waitingPermission = false;
        session = std::make_unique<rec::RecordingSession>(archive, std::move(backend));
        QObject::connect(session.get(), &rec::RecordingSession::stateChanged, owner, [this](quint64, rec::SessionState state) {
            if (window) window->setState(state, session->elapsed());
            notify();
        });
        QObject::connect(session.get(), &rec::RecordingSession::prepared, owner, [this](quint64, const auto& prepared) {
            if (window) window->showPrepared(prepared);
            timer.start();
        });
        QObject::connect(session.get(), &rec::RecordingSession::finished, owner, [this](const auto& result) { completed(result); });
        QObject::connect(session.get(), &rec::RecordingSession::shutdownFinished, owner, [this] { notify(); });
        const QPointer<RecordingController> alive(owner);
        const auto started = session->start(request);
        if (!alive) return;
        if (!started) fail(QStringLiteral("The recording session could not start"));
        if (alive) notify();
    }
    rec::DesktopRecordingControls prepareControls()
    {
        if (!window) return {};
        auto native = static_cast<std::uintptr_t>(window->winId());
        const bool excluded = rec::excludeControlsFromCapture(native) == rec::ControlsExclusion::Excluded;
        window->setControlsMayAppear(!excluded);
        if (excluded || needsPermission(selection)) return {};
        const bool placed = placeControlsOutside(*window, selection);
        native = static_cast<std::uintptr_t>(window->winId());
        return {native, placed};
    }
    void authorize(rec::BackendRequest request)
    {
#ifdef Q_OS_WIN
        Q_UNUSED(request);
        fail(QStringLiteral("This selection has no physical Windows desktop coordinates"));
#else
        waitingPermission = true; pending = request;
        screenCast = std::make_unique<Capture::ScreenCastSource>();
        QObject::connect(screenCast.get(), &Capture::ScreenCastSource::failed, owner, [this](const Capture::Failure& error) {
            if (session && session->busy()) return; // the same source error reaches its worker
            if (error.code == Capture::Failure::Code::Cancelled) finishJourney(); else fail(error.detail);
        });
        QObject::connect(screenCast.get(), &Capture::ScreenCastSource::stopped, owner, [this] {
            notify();
            if (!deferredStart) return;
            // Replace the finished QObject after its terminal signal returns.
            // The GUI must never destroy an active source just to retry it.
            QTimer::singleShot(0, owner, [q = QPointer(owner)] {
                if (!q || !q->state_->journey || !q->state_->deferredStart) return;
                const auto request = *q->state_->deferredStart;
                q->state_->deferredStart.reset(); q->state_->start(request);
            });
        });
        QObject::connect(screenCast.get(), &Capture::ScreenCastSource::ready, owner, [this](const Core::FrameLease& frame) {
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
            Capture::Snapshot snapshot;
            snapshot.image = std::move(image); snapshot.sourceId = frame.metadata().sourceId;
            snapshot.generation = frame.formatGeneration(); snapshot.coordinates = Capture::CoordinateSpace::AuthorizedSourcePixels;
            const auto translatedName = translator ? translator(u"toolbar.record") : QString{};
            snapshot.sourceName = translatedName.isEmpty() || translatedName == u"toolbar.record" ?
                QStringLiteral("Screen recording") : translatedName;
            selector = std::make_unique<ui::CaptureWindow>(); selector->setTranslator(translator);
            QObject::connect(selector.get(), &ui::CaptureWindow::closed, owner, [this] {
                if (!waitingPermission) return;
                if (screenCast) screenCast->stop();
                finishJourney();
            });
            QObject::connect(selector.get(), &ui::CaptureWindow::sourceInvalidated, owner, [this](const auto& error) { fail(error.detail); });
            QObject::connect(selector->session(), &Capture::CaptureSession::actionRequested, owner, [this](const auto& selected) {
                if (!waitingPermission || selected.action != Capture::Action::Record) return;
                auto source = screenCast->takeFrameSource();
                if (!source) { fail(QStringLiteral("The authorized recording stream is no longer available")); return; }
                pending.region = pixels(selected.sourceRect);
                if (pending.format == rec::Format::Mp4) {
                    const auto aligned = rec::alignedMp4Region(pending.region, pending.maximumFrameBytes);
                    if (!aligned) { fail(QStringLiteral("The selected MP4 area is too small")); return; }
                    pending.region = *aligned;
                }
                waitingPermission = false; selector->hideForAction();
                selector->session()->complete(selected.token, Capture::ActionResult::Completed);
                startSession(rec::frameRecordingBackend(std::move(source), sourceContract), pending);
            });
            const auto error = selector->showSnapshot(std::move(snapshot), {Capture::Action::Record});
            if (error) { fail(error.detail); return; }
            selector->session()->setCapability(Capture::Action::Record, {Capture::CapabilityState::Available, {}});
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
        if (closing || outputPending || waitingPermission || (session && session->busy())) return;
        if (window) window->setState(rec::SessionState::Preparing);
        if (screenCast && screenCast->busy()) {
            deferredStart = request; notify(); return;
        }
        const auto controls = prepareControls();
        if (needsPermission(selection)) {
            authorize(request); return;
        }
        request.region = pixels(*selection.desktopRect);
        if (request.format == rec::Format::Mp4) {
            const auto aligned = rec::alignedMp4Region(request.region, request.maximumFrameBytes);
            if (!aligned) { fail(QStringLiteral("The selected MP4 area is too small")); return; }
            request.region = *aligned;
        }
        startSession(rec::desktopRecordingBackend(controls), request);
    }
};

RecordingController::RecordingController(std::shared_ptr<Media::MediaArchiveService> archive,
    MediaWorkspace& workspace, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), workspace))
{
    connect(&state_->timer, &QTimer::timeout, this, [this] {
        if (state_->session && state_->session->busy() && state_->window)
            state_->window->setState(state_->session->state(), state_->session->elapsed());
    });
}
RecordingController::~RecordingController()
{
    state_->closing = true;
    state_->workspace.cancelOutput(this, state_->session ? state_->session->token() : 0);
    state_->session.reset();
    if (state_->selector) state_->selector->shutdown();
    if (state_->screenCast && state_->screenCast->busy()) {
        auto* remaining = state_->screenCast.release();
        disconnect(remaining, nullptr, this, nullptr);
        connect(remaining, &Capture::ScreenCastSource::stopped, remaining, &QObject::deleteLater);
        remaining->stop();
    }
    delete state_->window.data();
}
void RecordingController::setTranslator(ui::DesktopTranslator translator)
{
    state_->translator = std::move(translator);
    if (state_->window) state_->window->setTranslator(state_->translator);
    if (state_->selector) state_->selector->setTranslator(state_->translator);
}
bool RecordingController::begin(const Capture::ActionRequest& selection, QWidget* parent)
{
    if (QThread::currentThread() != thread() || state_->closing || !state_->archive || selection.action != Capture::Action::Record)
        return false;
    if (state_->busy()) {
        activate();
        return false;
    }
    const auto region = selection.desktopRect.value_or(selection.sourceRect);
    if (region.isEmpty()) return false;
    if (!state_->window) {
        state_->window = new ui::RecordingWindow(); state_->window->setTranslator(state_->translator);
        connect(state_->window, &ui::RecordingWindow::startRequested, this, [this](const auto& request) { state_->start(request); });
        connect(state_->window, &ui::RecordingWindow::stopRequested, this, [this](rec::StopIntent intent) {
            if (state_->session && state_->session->busy()) state_->session->requestStop(intent);
            else if (state_->deferredStart) state_->finishJourney();
            else if (state_->waitingPermission) {
                state_->waitingPermission = false;
                if (state_->selector) state_->selector->shutdown();
                if (state_->screenCast) state_->screenCast->stop();
                state_->finishJourney();
            }
        });
        connect(state_->window, &ui::RecordingWindow::fileRequested, &state_->workspace, &MediaWorkspace::revealArchivedFile);
        connect(state_->window, &ui::RecordingWindow::viewClosed, this, [this] {
            if (!state_->outputPending && !state_->waitingPermission && (!state_->session || !state_->session->busy()))
                state_->finishJourney();
        });
    }
    state_->selection = selection; state_->journey = true;
    state_->window->setRegion(pixels(region));
    if (parent && parent->screen()) {
        const auto area = parent->screen()->availableGeometry();
        state_->window->move(area.center() - state_->window->rect().center());
    }
    state_->window->show(); state_->window->raise(); state_->prepareControls(); state_->notify(); return true;
}
void RecordingController::activate(const QString& activationToken)
{
    if (QThread::currentThread() != thread() || state_->closing || !state_->busy()) return;
    QWidget* view = state_->selector && state_->selector->visible() ? state_->selector->dialogParent() : state_->window.data();
    if (!view) return;
    view->showNormal(); view->raise(); view->winId();
    if (view == state_->window) {
        const bool excluded = rec::excludeControlsFromCapture(static_cast<std::uintptr_t>(view->winId())) ==
            rec::ControlsExclusion::Excluded;
        state_->window->setControlsMayAppear(!excluded);
    }
    if (auto* native = view->windowHandle()) {
        Platform::applyActivationToken(native, activationToken);
        native->requestActivate();
    }
}
bool RecordingController::busy() const noexcept { return state_->busy(); }
void RecordingController::shutdown()
{
    if (QThread::currentThread() != thread() || state_->closing) return;
    state_->closing = true; state_->journey = false; state_->waitingPermission = false; state_->deferredStart.reset();
    if (state_->selector) state_->selector->shutdown();
    if (state_->outputPending) {
        state_->workspace.cancelOutput(this, state_->session ? state_->session->token() : 0);
        state_->outputPending = false;
    }
    if (state_->session && state_->session->busy()) state_->session->shutdown();
    else if (state_->screenCast) state_->screenCast->stop();
    if (state_->window) state_->window->hide();
    state_->notify();
}

} // namespace HuhuCapture::Application
