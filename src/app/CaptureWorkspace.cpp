#include "app/CaptureWorkspace.h"

#include "app/MediaWorkspace.h"
#include "capture/CaptureSession.h"
#include "services/desktop/SystemClipboard.h"
#include "services/recognition/QrService.h"
#include "ui/qt/CaptureWindow.h"
#include "ui/qt/DesktopTheme.h"
#include "ui/qt/EditorWindow.h"
#include "ui/qt/HistoryWindow.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <exception>
#include <optional>
#include <stop_token>
#include <utility>

namespace HuhuCapture::Application {
namespace {
namespace cap = Capture;
namespace ui = UI::Qt;
cap::ActionResult actionResult(ui::OutputResult result)
{
    switch (result) {
    case ui::OutputResult::Succeeded: return cap::ActionResult::Completed;
    case ui::OutputResult::Cancelled: return cap::ActionResult::Cancelled;
    case ui::OutputResult::Failed: return cap::ActionResult::Failed;
    }
    return cap::ActionResult::Failed;
}
cap::StartOptions options(Core::CaptureEntry entry)
{
    switch (entry) {
    case Core::CaptureEntry::Pin: return {cap::Action::Pin};
    case Core::CaptureEntry::Scroll: return {cap::Action::Scroll};
    case Core::CaptureEntry::Recording: return {cap::Action::Record};
    case Core::CaptureEntry::Qr: return {cap::Action::Qr};
    case Core::CaptureEntry::Capture: return {};
    }
    return {};
}
QWindow* nativeWindow(QWidget* origin)
{ return origin ? origin->window()->windowHandle() : nullptr; }
}

struct CaptureWorkspace::State {
    struct Prepared {
        Core::QrError qrError{Core::QrError::None};
        Recognition::QrResult qr;
        QString qrDisplay;
        Desktop::ClipboardPreparation clipboard;
    };
    struct Job {
        quint64 token{};
        std::stop_source stop;
        QPointer<QFutureWatcher<Prepared>> watcher;
    };
    struct TextPublication {
        quint64 serviceToken{}, captureToken{};
        std::function<void(const Desktop::ClipboardResult&)> completed;
    };
    CaptureWorkspace* owner;
    MediaWorkspace& media;
    Desktop::SystemClipboard& clipboard;
    cap::ScreenCaptureService source;
    cap::CaptureSession session;
    ui::CaptureWindow view;
    Desktop::WindowActivation activation;
    ui::DesktopTranslator translator;
    QThreadPool pool;
    std::shared_ptr<Job> job;
    std::optional<TextPublication> publication;
    QPointer<QDialog> qrDialog;
    Core::CaptureEntry entry{Core::CaptureEntry::Capture};
    QString activationToken;
    bool initializing{}, ready{}, pending{}, stopping{}, sourceStopped{}, shutdownReported{}, journeyActive{};
    quint64 captureToken{};
    quint64 liveActionToken{};

    State(CaptureWorkspace* parent, MediaWorkspace& workspace, Desktop::SystemClipboard& systemClipboard)
        : owner(parent), media(workspace), clipboard(systemClipboard), view(session)
    {
        pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1);
        const cap::Capability available{cap::CapabilityState::Available, {}};
        for (const auto action : {cap::Action::Edit, cap::Action::Pin, cap::Action::Qr,
                 cap::Action::History, cap::Action::SaveAs, cap::Action::Clipboard,
                 cap::Action::Hex, cap::Action::Rgb, cap::Action::Cmyk, cap::Action::Position})
            session.setCapability(action, available);
    }
    QString text(QStringView key) const
    { return translator ? translator(key) : key.toString(); }
    bool busy() const
    {
        return pending || captureToken || job || publication || qrDialog ||
            (session.stage() != cap::Stage::Empty && session.stage() != cap::Stage::Closed);
    }
    void changed()
    {
        // Queue notifications so callers cannot delete a partially admitted job.
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->busyChanged(target->busy()); });
    }
    void reportFailure(QString detail)
    {
        QTimer::singleShot(0, owner, [target = owner, detail = std::move(detail)] {
            Q_EMIT target->operationFailed(detail);
        });
    }
    void finishJourney()
    {
        if (!std::exchange(journeyActive, false) || stopping) return;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->captureFinished(); });
    }
    void finishShutdown()
    {
        if (!stopping || !sourceStopped || job || publication || qrDialog || shutdownReported) return;
        shutdownReported = true;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->shutdownFinished(); });
    }
    void complete(quint64 token, cap::ActionResult result, const QString& detail = {})
    {
        if (!token || session.activeRequest() != token) return;
        if (liveActionToken == token) liveActionToken = 0;
        const QPointer<CaptureWorkspace> alive(owner);
        session.complete(token, result, detail);
        if (!alive) return;
        if (!stopping && session.stage() != cap::Stage::Closed) view.setPresentationVisible(true);
        if (alive) changed();
    }
    void begin()
    {
        if (stopping || !ready || !pending) return;
        pending = false;
        if (!source.capabilities().screenshot.canRequest()) {
            const auto detail = source.capabilities().screenshot.reason;
            changed();
            reportFailure(detail);
            finishJourney();
            return;
        }
        captureToken = source.request();
        changed();
        if (!captureToken) {
            reportFailure(QStringLiteral("The screen capture service could not accept this request"));
            finishJourney();
        }
    }
    bool submit(quint64 token, std::function<Prepared(std::stop_token)> worker,
        std::function<void(Prepared)> completed)
    {
        if (stopping || job || session.activeRequest() != token) return false;
        auto current = std::make_shared<Job>(); current->token = token;
        auto watcher = std::make_unique<QFutureWatcher<Prepared>>(owner);
        current->watcher = watcher.get();
        QObject::connect(watcher.get(), &QFutureWatcher<Prepared>::finished, owner,
            [this, current, completed = std::move(completed)]() mutable {
                auto* watcher = current->watcher.data();
                if (watcher) watcher->deleteLater();
                if (job != current) return;
                job.reset();
                if (stopping || current->stop.stop_requested() || session.activeRequest() != current->token) {
                    changed(); finishShutdown(); return;
                }
                const QPointer<CaptureWorkspace> alive(owner);
                try {
                    auto result = watcher->result();
                    completed(std::move(result));
                } catch (const std::exception&) {
                    if (alive) complete(current->token, cap::ActionResult::Failed,
                        QStringLiteral("The selected content could not be prepared"));
                }
            });
        watcher->setFuture(QtConcurrent::run(&pool, [worker = std::move(worker), stop = current->stop.get_token()]() mutable {
            return worker(stop);
        }));
        job = std::move(current); watcher.release(); changed();
        return true;
    }
    void publish(quint64 token, Desktop::ClipboardPreparation prepared, QWindow* origin,
        std::function<void(const Desktop::ClipboardResult&)> completed)
    {
        if (prepared.failure) { completed({prepared.failure, {}}); return; }
        const auto receipt = clipboard.publish(std::move(prepared.content), origin);
        if (!receipt) {
            completed({{Desktop::Error::Busy, 0, QStringLiteral("The system clipboard could not accept this request")}, {}});
            return;
        }
        publication = TextPublication{receipt, token, std::move(completed)}; changed();
    }
    void textOutput(quint64 token, const QString& value, QWidget* origin,
        std::function<void(const Desktop::ClipboardResult&)> completed)
    {
        const QPointer<QWidget> parent(origin);
        auto callback = std::make_shared<std::function<void(const Desktop::ClipboardResult&)>>(std::move(completed));
        if (!submit(token, [value](std::stop_token stop) {
                Prepared result; result.clipboard = Desktop::prepareText(value, {}, stop); return result;
            }, [this, token, parent, callback](Prepared result) {
                publish(token, std::move(result.clipboard), nativeWindow(parent), *callback);
            }))
            (*callback)({{Desktop::Error::Busy, 0, QStringLiteral("Another capture preparation is still finishing")}, {}});
    }
    void showQr(quint64 token, Recognition::QrResult result, const QString& display)
    {
        const QPointer<CaptureWorkspace> alive(owner);
        view.setPresentationVisible(false);
        if (!alive) return;
        auto* dialog = new QDialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("captureQrResult"));
        dialog->setWindowTitle(text(u"about.sysTip"));
        ui::DesktopTheme::apply(dialog);
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(20, 16, 20, 16); layout->setSpacing(12);
        const bool detected = !result.detection.codes.empty();
        if (detected) {
            auto* content = new QPlainTextEdit(dialog); content->setObjectName(QStringLiteral("captureQrText"));
            content->setReadOnly(true); content->setPlainText(display); content->setMinimumSize(360, 200);
            layout->addWidget(content);
        }
        auto* message = ui::DesktopTheme::label(dialog, 12, true); message->setWordWrap(true);
        message->setText(detected ? text(u"cap.qrcodeCopy") : text(u"cap.qrcodeEmpty"));
        if (result.decoded.unreadable)
            message->setText(text(u"captureResult.rawPayload"));
        layout->addWidget(message);
        auto* failure = ui::DesktopTheme::label(dialog, 12); failure->setWordWrap(true); failure->setProperty("error", true);
        failure->hide(); layout->addWidget(failure);
        auto* buttons = new QDialogButtonBox(detected ? QDialogButtonBox::Ok | QDialogButtonBox::Cancel : QDialogButtonBox::Ok, dialog);
        layout->addWidget(buttons);
        qrDialog = dialog;
        const QPointer<QDialog> guarded(dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(buttons->button(QDialogButtonBox::Ok), &QPushButton::clicked, owner,
            [this, token, guarded, buttons, failure, display, detected] {
                if (!guarded || session.activeRequest() != token || publication || job) return;
                if (!detected) { guarded->accept(); return; }
                buttons->button(QDialogButtonBox::Ok)->setEnabled(false); failure->hide();
                // Keep this real focused window visible through the asynchronous
                // Wayland receipt; closing it on accepted() would lose context.
                textOutput(token, display, guarded, [guarded, buttons, failure](const auto& result) {
                    if (!guarded) return;
                    if (!result.failure) { guarded->accept(); return; }
                    failure->setText(result.failure.detail); failure->show();
                    buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
                });
            });
        auto finished = std::make_shared<bool>(false);
        const auto finishDialog = [this, token, finished](int status) {
            if (std::exchange(*finished, true)) return;
            const QPointer<CaptureWorkspace> alive(owner);
            qrDialog.clear();
            if (job && job->token == token) job->stop.request_stop();
            if (publication && publication->captureToken == token) clipboard.cancel(publication->serviceToken);
            if (!alive) return;
            complete(token, status == QDialog::Accepted ? cap::ActionResult::Completed : cap::ActionResult::Cancelled);
            if (alive) finishShutdown();
        };
        QObject::connect(dialog, &QDialog::finished, owner, finishDialog);
        QObject::connect(dialog, &QObject::destroyed, owner, [finishDialog] { finishDialog(QDialog::Rejected); });
        dialog->open();
        if (alive) changed();
    }
    void action(const cap::ActionRequest& request, QWidget* origin)
    {
        if (stopping || request.token != session.activeRequest()) return;
        switch (request.action) {
        case cap::Action::Edit:
        case cap::Action::Pin: {
            const QPointer<CaptureWorkspace> alive(owner);
            const auto* editor = media.openEditor(request.image, request.action == cap::Action::Pin);
            if (alive) complete(request.token, editor ? cap::ActionResult::Completed : cap::ActionResult::Failed,
                editor ? QString() : QStringLiteral("The selected image could not be opened in the editor"));
            return;
        }
        case cap::Action::History: {
            const QPointer<CaptureWorkspace> alive(owner);
            const auto* history = media.openHistory();
            if (alive) complete(request.token, history ? cap::ActionResult::Completed : cap::ActionResult::Failed);
            return;
        }
        case cap::Action::SaveAs:
        case cap::Action::Clipboard: {
            const QPointer<CaptureWorkspace> alive(owner);
            const bool admitted = media.outputImage(&session, origin ? origin->window() : nullptr, request.token,
                request.action == cap::Action::SaveAs ? ui::OutputIntent::SaveAs : ui::OutputIntent::Clipboard,
                request.image, [this, token = request.token](ui::OutputResult result, const QString& detail) {
                    complete(token, actionResult(result), detail);
                });
            if (alive && !admitted)
                complete(request.token, cap::ActionResult::Failed, QStringLiteral("The image output service is busy"));
            return;
        }
        case cap::Action::Qr:
            if (!submit(request.token, [image = request.image](std::stop_token stop) {
                    Prepared result; result.qrError = Recognition::recognizeImage(image, result.qr, {}, stop);
                    if (result.qrError != Core::QrError::None) return result;
                    result.qrDisplay = result.qr.text;
                    if (result.qr.decoded.unreadable) {
                        // Keep all decoded text and original bytes. Conversion
                        // runs with recognition, outside the presentation thread.
                        result.qrDisplay.clear();
                        for (std::size_t index = 0; index < result.qr.detection.codes.size(); ++index) {
                            if (stop.stop_requested()) { result.qrError = Core::QrError::Cancelled; return result; }
                            if (!result.qrDisplay.isEmpty()) result.qrDisplay += QStringLiteral("\n\n");
                            const auto& decoded = result.qr.decoded.codes[index];
                            if (decoded.readable())
                                result.qrDisplay += QString::fromUtf16(decoded.text.data(), static_cast<qsizetype>(decoded.text.size()));
                            else {
                                const auto& code = result.qr.detection.codes[index];
                                result.qrDisplay += QStringLiteral("[QR %1; ECI %2; raw hex]\n").arg(index + 1).arg(code.eci);
                                result.qrDisplay += QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(code.payload.data()),
                                    static_cast<qsizetype>(code.payload.size())).toHex(' '));
                            }
                        }
                    }
                    return result;
                }, [this, token = request.token](Prepared result) {
                    if (result.qrError != Core::QrError::None)
                        complete(token, result.qrError == Core::QrError::Cancelled ? cap::ActionResult::Cancelled : cap::ActionResult::Failed,
                            QStringLiteral("QR recognition did not complete within the image and memory limits"));
                    else showQr(token, std::move(result.qr), result.qrDisplay);
                }))
                complete(request.token, cap::ActionResult::Failed, QStringLiteral("QR recognition is still finishing"));
            return;
        case cap::Action::Hex: case cap::Action::Rgb: case cap::Action::Cmyk: case cap::Action::Position:
            textOutput(request.token, request.text, origin, [this, token = request.token](const auto& result) {
                complete(token, !result.failure ? cap::ActionResult::Completed :
                    result.failure.code == Desktop::Error::Cancelled ? cap::ActionResult::Cancelled : cap::ActionResult::Failed,
                    result.failure.detail);
            });
            return;
        case cap::Action::Scroll: case cap::Action::Record:
        {
            const QPointer<CaptureWorkspace> alive(owner);
            liveActionToken = request.token;
            view.setPresentationVisible(false);
            if (alive) Q_EMIT owner->liveActionRequested(request, origin);
            return;
        }
        case cap::Action::Count: return;
        }
    }
    void cancel(quint64 token)
    {
        const QPointer<CaptureWorkspace> alive(owner);
        if (liveActionToken == token) liveActionToken = 0;
        if (job && job->token == token) job->stop.request_stop();
        if (publication && publication->captureToken == token) clipboard.cancel(publication->serviceToken);
        if (!alive) return;
        media.cancelOutput(&session, token);
        if (!alive) return;
        if (qrDialog) qrDialog->reject();
        if (alive) Q_EMIT owner->liveActionCancelled(token);
    }
};

CaptureWorkspace::CaptureWorkspace(MediaWorkspace& media, Desktop::SystemClipboard& clipboard, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, media, clipboard))
{
    connect(&state_->source, &cap::ScreenCaptureService::initialized, this, [this](const auto&, const auto&) {
        state_->ready = true; state_->initializing = false;
        const QPointer<CaptureWorkspace> alive(this);
        Q_EMIT initialized();
        if (alive) state_->begin();
    });
    connect(&state_->source, &cap::ScreenCaptureService::capabilitiesChanged, this, [this] { Q_EMIT capabilitiesChanged(); });
    connect(&state_->source, &cap::ScreenCaptureService::sourceInvalidated, this, &CaptureWorkspace::sourceInvalidated);
    connect(&state_->source, &cap::ScreenCaptureService::finished, this, [this](quint64 token, const auto& snapshot, const auto& failure) {
        if (token != state_->captureToken) return;
        state_->captureToken = 0;
        if (state_->stopping) { state_->finishShutdown(); return; }
        const QPointer<CaptureWorkspace> alive(this);
        auto error = failure;
        if (!error) error = state_->session.load(snapshot, options(state_->entry));
        if (!alive) return;
        if (!error) error = state_->view.open();
        if (!alive) return;
        if (!error && !state_->activationToken.isEmpty()) state_->view.activate(std::exchange(state_->activationToken, {}));
        if (!alive) return;
        if (error) {
            state_->session.cancel();
            if (!alive) return;
            state_->finishJourney();
            if (error.code != cap::Failure::Code::Cancelled) state_->reportFailure(error.detail);
        }
        state_->changed();
    });
    connect(&state_->source, &cap::ScreenCaptureService::shutdownFinished, this, [this] {
        state_->sourceStopped = true; state_->captureToken = 0; state_->finishShutdown();
    });
    connect(&state_->session, &cap::CaptureSession::cursorNudged, this, [this](QPoint point) {
        const auto& snapshot = state_->session.snapshot();
        if (snapshot.coordinates == cap::CoordinateSpace::DesktopPixels)
            state_->source.moveCursor(snapshot.sourceId, snapshot.generation, point);
    });
    connect(&state_->source, &cap::ScreenCaptureService::cursorMoved, this,
        [this](quint64 sourceId, quint64 generation, QPoint requested, QPoint actual, const cap::Failure& failure) {
            const auto& snapshot = state_->session.snapshot();
            if (state_->stopping || snapshot.sourceId != sourceId || snapshot.generation != generation ||
                state_->session.cursor() != requested || !state_->source.isCurrent(sourceId, generation)) return;
            if (failure) { state_->reportFailure(failure.detail); return; }
            // A real desktop can clamp an arrow nudge at a monitor edge/gap.
            // Never replace a newer physical pointer event with an older warp.
            if (actual != requested) state_->session.pointerMove(actual);
        });
    connect(&state_->session, &cap::CaptureSession::actionCancelled, this, [this](quint64 token) { state_->cancel(token); });
    connect(&state_->session, &cap::CaptureSession::closed, this, [this] {
        const QPointer<CaptureWorkspace> alive(this);
        state_->view.close();
        if (alive) { state_->changed(); state_->finishJourney(); }
    });
    connect(&state_->view, &ui::CaptureWindow::actionRequested, this,
        [this](const auto& request, QWidget* origin) { state_->action(request, origin); });
    connect(&clipboard, &Desktop::SystemClipboard::finished, this, [this](quint64 token, const auto& result) {
        if (!state_->publication || state_->publication->serviceToken != token) return;
        auto pending = std::move(*state_->publication); state_->publication.reset();
        const QPointer<CaptureWorkspace> alive(this);
        if (!state_->stopping && state_->session.activeRequest() == pending.captureToken) pending.completed(result);
        if (alive) { state_->changed(); state_->finishShutdown(); }
    });
}
CaptureWorkspace::~CaptureWorkspace()
{ shutdown(); state_->pool.waitForDone(); if (state_->qrDialog) delete state_->qrDialog; }
void CaptureWorkspace::setTranslator(ui::DesktopTranslator translator)
{ state_->translator = std::move(translator); state_->view.setTranslator(state_->translator); }
void CaptureWorkspace::initialize()
{
    if (state_->ready || state_->initializing || state_->stopping) return;
    state_->initializing = true; state_->source.initialize();
}
cap::CaptureCapabilities CaptureWorkspace::capabilities() const { return state_->source.capabilities(); }
bool CaptureWorkspace::sourceIsCurrent(quint64 sourceId, quint64 generation) const
{ return state_->source.isCurrent(sourceId, generation); }
bool CaptureWorkspace::start(Core::CaptureEntry entry, const QString& activationToken)
{
    if (QThread::currentThread() != thread() || state_->stopping) return false;
    if (state_->busy()) {
        if (entry != state_->entry || state_->pending || state_->captureToken || state_->job || state_->publication) return false;
        if (state_->liveActionToken) {
            Q_EMIT liveActionActivationRequested(state_->liveActionToken, activationToken);
            return true;
        }
        if (state_->qrDialog) state_->activation.request(nativeWindow(state_->qrDialog), activationToken);
        else state_->view.activate(activationToken);
        return true;
    }
    state_->entry = entry; state_->pending = true; state_->journeyActive = true;
    state_->activationToken = activationToken;
    if (!state_->ready) initialize();
    else state_->begin();
    return true;
}
void CaptureWorkspace::setLiveCapability(cap::Action action, cap::Capability capability)
{
    if (action == cap::Action::Record || action == cap::Action::Scroll)
        state_->session.setCapability(action, std::move(capability));
}
void CaptureWorkspace::completeLiveAction(quint64 token, cap::ActionResult result, const QString& detail)
{ state_->complete(token, result, detail); }
void CaptureWorkspace::closeLiveAction(quint64 token)
{
    if (token && token == state_->liveActionToken && token == state_->session.activeRequest()) state_->session.cancel();
}
cap::CaptureSession& CaptureWorkspace::session() noexcept { return state_->session; }
ui::CaptureWindow& CaptureWorkspace::window() noexcept { return state_->view; }
bool CaptureWorkspace::busy() const noexcept { return state_->busy(); }
void CaptureWorkspace::shutdown()
{
    if (state_->stopping) return;
    state_->stopping = true; state_->pending = false;
    const QPointer<CaptureWorkspace> alive(this);
    if (state_->job) state_->job->stop.request_stop();
    state_->session.cancel();
    if (!alive) return;
    if (state_->qrDialog) state_->qrDialog->reject();
    if (!alive) return;
    if (state_->publication) state_->clipboard.cancel(state_->publication->serviceToken);
    if (!alive) return;
    state_->view.close();
    if (!alive) return;
    state_->source.shutdown();
    if (alive) state_->finishShutdown();
}
} // namespace HuhuCapture::Application
