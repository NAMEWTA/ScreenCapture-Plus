// Application flow reused from this project's PR #7 ScrollController at
// 251bc0071278c6b711c840bf5188d16870bc7349; MIT, Copyright (c) 2026 NAMEWTA.
#include "app/ScrollWorkspace.h"

#include "app/CaptureWorkspace.h"
#include "app/MediaWorkspace.h"
#include "capture/CaptureSession.h"
#include "capture/ScrollCaptureSession.h"
#include "services/capture/LiveCaptureSource.h"
#include "ui/qt/ScrollWindow.h"
#include <QPointer>
#include <QTimer>
#include <utility>

namespace HuhuCapture::Application {
namespace cap = Capture;
namespace ui = UI::Qt;
struct ScrollWorkspace::State {
    ScrollWorkspace* owner;
    MediaWorkspace& media;
    CaptureWorkspace& capture;
    ui::DesktopTranslator translator;
    QPointer<cap::ScrollCaptureSession> session;
    QPointer<ui::ScrollWindow> view;
    quint64 captureToken{};
    QString externalStopHint;
    bool stopping{}, shutdownReported{}, externalStopAvailable{};

    State(ScrollWorkspace* parent, MediaWorkspace& workspace, CaptureWorkspace& source)
        : owner(parent), media(workspace), capture(source) {}
    bool busy() const { return captureToken || session || view; }
    void changed()
    { QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->busyChanged(target->busy()); }); }
    void updateCapability()
    {
        auto available = capture.capabilities().automaticScroll;
        if (capture.capabilities().backend == QStringLiteral("xdg-desktop-portal"))
            available = capture.liveSource().capability();
        if (stopping) available = {cap::CapabilityState::Unavailable, QStringLiteral("Scrolling capture is shutting down")};
        capture.setLiveCapability(cap::Action::Scroll, std::move(available));
    }
    void closed()
    {
        if (!stopping || busy() || shutdownReported) return;
        shutdownReported = true;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->shutdownFinished(); });
    }
    void finish(quint64 token)
    {
        if (!token || token != captureToken) return;
        captureToken = 0;
        if (view) {
            auto* old = view.data(); view.clear();
            QObject::disconnect(old, nullptr, owner, nullptr); old->hide(); old->deleteLater();
        }
        if (session) {
            auto* old = session.data(); session.clear();
            QObject::disconnect(old, nullptr, owner, nullptr); old->deleteLater();
        }
        const QPointer<ScrollWorkspace> alive(owner);
        capture.closeLiveAction(token); if (!alive) return;
        changed(); closed();
        QTimer::singleShot(0, owner, [target = owner, token] { Q_EMIT target->finished(token); });
    }
    void open(const cap::ActionRequest& request)
    {
        if (request.action != cap::Action::Scroll) return;
        const QPointer<ScrollWorkspace> alive(owner);
        const auto token = request.token;
        if (stopping || busy()) {
            capture.completeLiveAction(request.token, cap::ActionResult::Failed,
                QStringLiteral("Another scrolling capture is still finishing"));
            return;
        }
        try {
            auto controller = std::make_unique<cap::ScrollCaptureSession>();
            cap::Failure error;
            if (request.coordinates == cap::CoordinateSpace::DesktopPixels)
                error = controller->prepareNative(request, capture.session().snapshot(), capture.nativeSource());
            else error = controller->prepareLive(request, capture.liveSource());
            if (error) { capture.completeLiveAction(request.token, cap::ActionResult::Failed, error.detail); return; }
            auto controls = std::make_unique<ui::ScrollWindow>(*controller);
            controls->setTranslator(translator);
            controls->setExternalStopAvailable(externalStopAvailable, externalStopHint);
            error = controls->open();
            if (!alive) return;
            if (error) { capture.completeLiveAction(request.token, cap::ActionResult::Failed, error.detail); return; }
            controller->setParent(owner);
            session = controller.release(); view = controls.release(); captureToken = token;
            const QPointer<ui::ScrollWindow> guarded(view);
            const QPointer<cap::ScrollCaptureSession> origin(session);
            QObject::connect(view, &ui::ScrollWindow::captureStateChanged, owner, [this] { changed(); });
            QObject::connect(session, &cap::ScrollCaptureSession::changed, owner, [this] { changed(); });
            QObject::connect(view, &ui::ScrollWindow::outputRequested, owner,
                [this, guarded, origin](quint64 token, ui::OutputIntent intent, const QImage& image) {
                    if (!guarded || !origin || stopping) return;
                    if (intent == ui::OutputIntent::Pin) {
                        const auto* opened = media.openEditor(image, true);
                        if (guarded) guarded->completeOutput(token, opened ? ui::OutputResult::Succeeded : ui::OutputResult::Failed,
                            opened ? QString() : QStringLiteral("The complete scrolling image could not be opened"));
                        return;
                    }
                    const bool admitted = media.outputImage(origin, guarded, token, intent, image,
                        [guarded, token](ui::OutputResult result, const QString& detail) {
                            if (guarded) guarded->completeOutput(token, result, detail);
                        });
                    if (!admitted && guarded) guarded->completeOutput(token, ui::OutputResult::Failed,
                        QStringLiteral("The output service is busy; the complete scrolling image is retained"));
                });
            QObject::connect(view, &ui::ScrollWindow::outputCancelRequested, owner,
                [this, origin](quint64 token) { if (origin) media.cancelOutput(origin, token); });
            QObject::connect(view, &ui::ScrollWindow::closed, owner, [this, token = request.token] { finish(token); });
            QObject::connect(session, &cap::ScrollCaptureSession::shutdownFinished, owner,
                [this, token = request.token] { finish(token); });
            QObject::connect(session, &QObject::destroyed, owner, [this, token = request.token] { finish(token); });
            QObject::connect(session, &cap::ScrollCaptureSession::finished, owner, [this](quint64, const cap::ScrollCompletion& result) {
                if (result.failure && result.failure.code != cap::Failure::Code::Cancelled && !stopping)
                    Q_EMIT owner->operationFailed(result.failure.detail);
            });
            view->activate(); if (alive) changed();
        } catch (const std::bad_alloc&) {
            if (alive) capture.completeLiveAction(token, cap::ActionResult::Failed,
                QStringLiteral("The scrolling capture controls could not be allocated"));
        }
    }
};
ScrollWorkspace::ScrollWorkspace(MediaWorkspace& media, CaptureWorkspace& capture, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, media, capture))
{
    connect(&capture, &CaptureWorkspace::initialized, this, [this] { state_->updateCapability(); });
    connect(&capture, &CaptureWorkspace::capabilitiesChanged, this, [this] { state_->updateCapability(); });
    connect(&capture, &CaptureWorkspace::liveActionRequested, this, [this](const cap::ActionRequest& request, QWidget*) { state_->open(request); });
    connect(&capture, &CaptureWorkspace::liveActionActivationRequested, this, [this](quint64 token, const QString& activation) {
        if (token == state_->captureToken && state_->view) state_->view->activate(activation);
    });
    connect(&capture, &CaptureWorkspace::liveActionCancelled, this, [this](quint64 token) {
        if (token != state_->captureToken) return;
        if (state_->view) state_->view->close();
        else if (state_->session) state_->session->shutdown();
    });
    state_->updateCapability();
}
ScrollWorkspace::~ScrollWorkspace()
{
    state_->stopping = true;
    // The application normally awaits shutdownFinished with its dispatcher
    // alive. Abrupt deletion still joins this private consumer before returning
    // its claim to the separately owned source.
    if (state_->view) { QObject::disconnect(state_->view, nullptr, this, nullptr); delete state_->view; }
    const auto controllers = findChildren<cap::ScrollCaptureSession*>(QString(), ::Qt::FindDirectChildrenOnly);
    for (auto* controller : controllers) { QObject::disconnect(controller, nullptr, this, nullptr); delete controller; }
}
void ScrollWorkspace::setTranslator(ui::DesktopTranslator translator)
{ state_->translator = std::move(translator); if (state_->view) state_->view->setTranslator(state_->translator); }
bool ScrollWorkspace::busy() const noexcept { return state_->busy(); }
void ScrollWorkspace::setExternalStopAvailable(bool available, const QString& hint)
{
    if (state_->externalStopAvailable == available && state_->externalStopHint == hint) return;
    state_->externalStopAvailable = available;
    state_->externalStopHint = hint;
    if (state_->view) state_->view->setExternalStopAvailable(available, hint);
    state_->changed();
}
bool ScrollWorkspace::canStop() const noexcept
{ return !state_->stopping && state_->view && state_->view->canStop(); }
bool ScrollWorkspace::stopActive()
{ return canStop() && state_->view->stopCapture(); }
bool ScrollWorkspace::controlsHiddenForCapture() const noexcept
{ return state_->view && state_->view->hiddenForCapture(); }
void ScrollWorkspace::shutdown()
{
    if (state_->stopping) return;
    state_->stopping = true;
    const QPointer<ScrollWorkspace> alive(this);
    state_->updateCapability(); if (!alive) return;
    if (state_->view) state_->view->close();
    else if (state_->session) state_->session->shutdown();
    if (alive) state_->closed();
}
} // namespace HuhuCapture::Application
