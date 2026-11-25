#include "app/CaptureWorkspace.h"

#include "app/MediaWorkspace.h"
#include "capture/CaptureSession.h"
#include "services/capture/LiveCaptureSource.h"
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
    cap::LiveCaptureSource live;
    cap::CaptureSession session;
    ui::CaptureWindow view;
    Desktop::WindowActivation activation;
    ui::DesktopTranslator translator;
    QThreadPool pool;
    std::shared_ptr<Job> job;
    std::optional<TextPublication> publication;
    QPointer<QDialog> qrDialog;
    QPointer<QDialog> authorization;
    QTimer authorizationDeadline;
    cap::Capability recordingCapability, scrollingCapability;
    cap::Action streamAction{cap::Action::Record};
    Core::CaptureEntry entry{Core::CaptureEntry::Capture};
    QString activationToken;
    bool initializing{}, ready{}, liveReady{}, pending{}, stopping{}, sourceStopped{}, liveStopped{}, shutdownReported{}, journeyActive{};
    bool preparingLive{}, switchingSource{};
    quint64 captureToken{};
    quint64 liveActionToken{};
    quint64 streamToken{}, authorizationRequest{}, streamSourceId{}, streamGeneration{};

    State(CaptureWorkspace* parent, MediaWorkspace& workspace, Desktop::SystemClipboard& systemClipboard)
        : owner(parent), media(workspace), clipboard(systemClipboard), view(session)
    {
        pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1);
        authorizationDeadline.setSingleShot(true); authorizationDeadline.setInterval(3000);
        QObject::connect(&authorizationDeadline, &QTimer::timeout, owner, [this] {
            if (preparingLive && !streamToken)
                authorizationFailed({cap::Failure::Code::Unavailable, 0,
                    QStringLiteral("The screen-sharing parent window could not be exposed")});
        });
        const cap::Capability available{cap::CapabilityState::Available, {}};
        for (const auto action : {cap::Action::Edit, cap::Action::Pin, cap::Action::Qr,
                 cap::Action::History, cap::Action::SaveAs, cap::Action::Clipboard,
                 cap::Action::Hex, cap::Action::Rgb, cap::Action::Cmyk, cap::Action::Position})
            session.setCapability(action, available);
    }
    QString text(QStringView key) const
    { return translator ? translator(key) : key.toString(); }
    QString text(QStringView key, const char* fallback) const
    {
        const auto value = text(key);
        return value.isEmpty() || value == key ? QString::fromUtf8(fallback) : value;
    }
    bool needsStream() const
    { return source.capabilities().backend == QStringLiteral("xdg-desktop-portal"); }
    void updateLiveCapabilities()
    {
        const QPointer<CaptureWorkspace> alive(owner);
        for (const auto action : {cap::Action::Record, cap::Action::Scroll}) {
            auto available = action == cap::Action::Record ? recordingCapability : scrollingCapability;
            if (available.canRequest() && needsStream()) {
                if (!liveReady) available = {cap::CapabilityState::Busy, QStringLiteral("Screen-sharing capabilities are being checked")};
                else if (!live.capability().canRequest()) available = live.capability();
                else available.state = cap::CapabilityState::RequiresPermission;
            }
            session.setCapability(action, available);
            if (!alive) return;
        }
    }
    bool busy() const
    {
        return pending || captureToken || streamToken || preparingLive || job || publication || qrDialog ||
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
        if (preparingLive || streamToken || liveActionToken ||
            (session.stage() != cap::Stage::Empty && session.stage() != cap::Stage::Closed)) return;
        if (!std::exchange(journeyActive, false) || stopping) return;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->captureFinished(); });
    }
    void finishShutdown()
    {
        if (!stopping || !sourceStopped || !liveStopped || job || publication || qrDialog || shutdownReported) return;
        shutdownReported = true;
        QTimer::singleShot(0, owner, [target = owner] { Q_EMIT target->shutdownFinished(); });
    }
    void complete(quint64 token, cap::ActionResult result, const QString& detail = {})
    {
        if (!token || session.activeRequest() != token) return;
        if (liveActionToken == token) { liveActionToken = 0; if (streamToken) live.stop(streamToken); }
        const QPointer<CaptureWorkspace> alive(owner);
        session.complete(token, result, detail);
        if (!alive) return;
        if (!stopping && session.stage() != cap::Stage::Closed) view.setPresentationVisible(true);
        if (alive) changed();
    }
    void begin()
    {
        if (stopping || !ready || !pending) return;
        const auto action = options(entry).enter;
        if (needsStream() && action && (*action == cap::Action::Record || *action == cap::Action::Scroll)) {
            if (!liveReady) return;
            pending = false;
            authorize(*action, 0);
            return;
        }
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
    void refreshAuthorization()
    {
        if (!authorization) return;
        authorization->setWindowTitle(text(streamAction == cap::Action::Scroll ? u"cap.long" : u"cap.video"));
        if (auto* message = authorization->findChild<QLabel*>(QStringLiteral("captureLiveAuthorizationMessage")))
            message->setText(text(u"cap.authorizeLive", "Choose one screen or window to share, then select the region again in the newly authorized image."));
        if (auto* status = authorization->findChild<QLabel*>(QStringLiteral("captureLiveAuthorizationStatus")))
            status->setText(text(u"cap.authorizingLive", "Waiting for screen-sharing permission…"));
        if (auto* close = authorization->findChild<QPushButton*>(QStringLiteral("captureLiveAuthorizationCancel")))
            close->setText(text(u"tool.close"));
    }
    void cleanupAuthorization()
    {
        authorizationDeadline.stop();
        if (!authorization) return;
        auto* dialog = authorization.data(); authorization.clear();
        QObject::disconnect(dialog, nullptr, owner, nullptr);
        dialog->hide(); dialog->deleteLater();
    }
    void authorizationFailed(const cap::Failure& error)
    {
        authorizationDeadline.stop(); preparingLive = false;
        const auto request = std::exchange(authorizationRequest, 0);
        if (authorization) authorization->hide();
        const QPointer<CaptureWorkspace> alive(owner);
        if (streamToken) live.cancel(streamToken);
        else cleanupAuthorization();
        if (!alive) return;
        if (request && session.activeRequest() == request)
            complete(request, error.code == cap::Failure::Code::Cancelled ? cap::ActionResult::Cancelled : cap::ActionResult::Failed, error.detail);
        else if (!stopping && error.code != cap::Failure::Code::Cancelled) reportFailure(error.detail);
        if (alive) { changed(); finishJourney(); finishShutdown(); }
    }
    void requestAuthorizedSource()
    {
        if (stopping || !preparingLive || streamToken || !authorization || !authorization->isVisible()) return;
        auto* native = authorization->windowHandle();
        if (!native || !native->isExposed()) return;
        authorizationDeadline.stop();
        streamToken = live.request(native);
        if (!streamToken) {
            authorizationFailed({cap::Failure::Code::Unavailable, 0,
                QStringLiteral("The screen-sharing source could not accept this request")});
            return;
        }
        changed();
    }
    void authorize(cap::Action action, quint64 request)
    {
        if (stopping) return;
        if (preparingLive || streamToken) {
            if (request) complete(request, cap::ActionResult::Failed,
                QStringLiteral("The previous screen-sharing source is still finishing"));
            return;
        }
        if (!session.capability(action).canRequest()) {
            const auto detail = session.capability(action).reason;
            if (request) complete(request, cap::ActionResult::Failed, detail);
            else { reportFailure(detail); finishJourney(); }
            return;
        }
        // PR #7's visible authorization parent/reselection journey is retained,
        // while this workspace uses one shared live owner for both consumers.
        // The prior Screenshot is never a coordinate contract for ScreenCast.
        streamAction = action; authorizationRequest = request; preparingLive = true;
        auto* dialog = new QDialog;
        dialog->setObjectName(QStringLiteral("captureLiveAuthorization"));
        ui::DesktopTheme::apply(dialog); dialog->setModal(false);
        auto* layout = new QVBoxLayout(dialog); layout->setContentsMargins(20, 16, 20, 16); layout->setSpacing(12);
        auto* message = ui::DesktopTheme::label(dialog); message->setWordWrap(true);
        message->setObjectName(QStringLiteral("captureLiveAuthorizationMessage")); layout->addWidget(message);
        auto* status = ui::DesktopTheme::label(dialog, 12, true); status->setWordWrap(true);
        status->setObjectName(QStringLiteral("captureLiveAuthorizationStatus")); layout->addWidget(status);
        auto* cancel = ui::DesktopTheme::button(dialog, "captureLiveAuthorizationCancel"); layout->addWidget(cancel);
        authorization = dialog; refreshAuthorization(); dialog->resize(420, dialog->sizeHint().height());
        QObject::connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
        QObject::connect(dialog, &QDialog::rejected, owner, [this] {
            if (preparingLive) authorizationFailed({cap::Failure::Code::Cancelled, 0, {}});
        });
        QObject::connect(dialog, &QObject::destroyed, owner, [this] {
            if (preparingLive) authorizationFailed({cap::Failure::Code::Cancelled, 0, {}});
        });
        const QPointer<CaptureWorkspace> alive(owner);
        view.setPresentationVisible(false); if (!alive) return;
        dialog->show(); if (!alive || !authorization) return;
        if (auto* native = dialog->windowHandle()) {
            native->installEventFilter(owner);
            activation.request(native, std::exchange(activationToken, {}));
        }
        authorizationDeadline.start();
        QTimer::singleShot(0, owner, [this] { requestAuthorizedSource(); });
        changed();
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
            if (request.coordinates == cap::CoordinateSpace::AuthorizedSourcePixels &&
                !live.isCurrent(request.sourceId, request.generation)) {
                authorize(request.action, request.token);
                return;
            }
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
        if (switchingSource) return;
        const QPointer<CaptureWorkspace> alive(owner);
        const bool ownedLiveAction = liveActionToken == token;
        if (ownedLiveAction) liveActionToken = 0;
        if (authorizationRequest == token) {
            authorizationRequest = 0; preparingLive = false; authorizationDeadline.stop();
            if (authorization) authorization->hide();
        }
        if (streamToken) {
            if (ownedLiveAction) live.stop(streamToken);
            else live.cancel(streamToken);
        } else if (!preparingLive) cleanupAuthorization();
        if (!alive) return;
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
        state_->updateLiveCapabilities(); if (!alive) return;
        Q_EMIT initialized();
        if (alive) state_->begin();
    });
    connect(&state_->source, &cap::ScreenCaptureService::capabilitiesChanged, this, [this] {
        const QPointer<CaptureWorkspace> alive(this);
        state_->updateLiveCapabilities();
        if (alive) Q_EMIT capabilitiesChanged();
    });
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
    connect(&state_->live, &cap::LiveCaptureSource::initialized, this, [this](const auto&, const auto&) {
        state_->liveReady = true;
        const QPointer<CaptureWorkspace> alive(this);
        state_->updateLiveCapabilities(); if (!alive) return;
        Q_EMIT capabilitiesChanged(); if (!alive) return;
        state_->begin();
    });
    connect(&state_->live, &cap::LiveCaptureSource::capabilityChanged, this, [this] {
        const QPointer<CaptureWorkspace> alive(this);
        state_->updateLiveCapabilities();
        if (alive) Q_EMIT capabilitiesChanged();
    });
    connect(&state_->live, &cap::LiveCaptureSource::prepared, this,
        [this](quint64 token, const cap::Snapshot& snapshot, const cap::Failure& failure) {
            if (token != state_->streamToken) return;
            if (!state_->preparingLive || state_->stopping) { state_->live.cancel(token); return; }
            if (failure) { state_->authorizationFailed(failure); return; }
            if (!state_->live.isCurrent(snapshot.sourceId, snapshot.generation)) {
                state_->authorizationFailed({cap::Failure::Code::SourceChanged, 0,
                    QStringLiteral("The newly authorized source is no longer current")});
                return;
            }
            const QPointer<CaptureWorkspace> alive(this);
            state_->preparingLive = false; state_->authorizationRequest = 0;
            state_->streamSourceId = snapshot.sourceId; state_->streamGeneration = snapshot.generation;
            state_->switchingSource = true;
            state_->session.cancel(); if (!alive) return;
            state_->view.close(); if (!alive) return;
            state_->switchingSource = false;
            if (state_->authorization) state_->authorization->hide();
            auto error = state_->session.load(snapshot, {state_->streamAction});
            if (!alive) return;
            if (!error) error = state_->view.open();
            if (!alive) return;
            if (error) {
                state_->live.cancel(token);
                state_->session.cancel(); if (!alive) return;
                if (error.code != cap::Failure::Code::Cancelled) state_->reportFailure(error.detail);
            } else state_->view.activate();
            if (alive) state_->changed();
        });
    connect(&state_->live, &cap::LiveCaptureSource::sourceClosed, this,
        [this](quint64 sourceId, quint64 generation, const cap::Failure& failure) {
            if (sourceId != state_->streamSourceId || generation != state_->streamGeneration) return;
            const QPointer<CaptureWorkspace> alive(this);
            // A normal stopped producer has no failure. Record/scroll retain
            // their output while their worker finalizes and releases its claim.
            if (failure) Q_EMIT sourceInvalidated(sourceId, generation, failure);
            if (!alive || state_->liveActionToken || state_->switchingSource) return;
            // The owned preview remains a valid frozen image, like an ordinary
            // screenshot after display invalidation. Editing/QR/image output
            // may continue; a new live action must authorize and select again.
            if (failure && failure.code != cap::Failure::Code::Cancelled && !state_->stopping)
                state_->reportFailure(failure.detail);
        });
    connect(&state_->live, &cap::LiveCaptureSource::stopped, this, [this](quint64 token, const auto&) {
        if (token != state_->streamToken) return;
        state_->streamToken = 0; state_->streamSourceId = 0; state_->streamGeneration = 0;
        state_->cleanupAuthorization();
        state_->changed(); state_->finishJourney(); state_->finishShutdown();
    });
    connect(&state_->live, &cap::LiveCaptureSource::shutdownFinished, this, [this] {
        state_->liveStopped = true; state_->finishShutdown();
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
        if (state_->switchingSource) return;
        const QPointer<CaptureWorkspace> alive(this);
        state_->view.close();
        if (!alive) return;
        if (state_->streamToken && !state_->liveActionToken) state_->live.stop(state_->streamToken);
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
{
    shutdown(); state_->pool.waitForDone();
    if (state_->qrDialog) delete state_->qrDialog;
    if (state_->authorization) delete state_->authorization;
}
void CaptureWorkspace::setTranslator(ui::DesktopTranslator translator)
{
    state_->translator = std::move(translator);
    state_->view.setTranslator(state_->translator); state_->refreshAuthorization();
}
void CaptureWorkspace::initialize()
{
    if (state_->ready || state_->initializing || state_->stopping) return;
    state_->initializing = true;
    const QPointer<CaptureWorkspace> alive(this);
    state_->source.initialize();
    if (alive) state_->live.initialize();
}
cap::CaptureCapabilities CaptureWorkspace::capabilities() const { return state_->source.capabilities(); }
cap::LiveCaptureSource& CaptureWorkspace::liveSource() noexcept { return state_->live; }
cap::ScreenCaptureService& CaptureWorkspace::nativeSource() noexcept { return state_->source; }
bool CaptureWorkspace::sourceIsCurrent(quint64 sourceId, quint64 generation) const
{ return state_->live.isCurrent(sourceId, generation) || state_->source.isCurrent(sourceId, generation); }
bool CaptureWorkspace::start(Core::CaptureEntry entry, const QString& activationToken)
{
    if (QThread::currentThread() != thread() || state_->stopping) return false;
    if (state_->busy()) {
        if (entry != state_->entry || state_->pending || state_->captureToken || state_->job || state_->publication) return false;
        if (state_->preparingLive && state_->authorization) {
            state_->activation.request(nativeWindow(state_->authorization), activationToken);
            return true;
        }
        if (state_->liveActionToken) {
            Q_EMIT liveActionActivationRequested(state_->liveActionToken, activationToken);
            return true;
        }
        if (state_->qrDialog) state_->activation.request(nativeWindow(state_->qrDialog), activationToken);
        else if (state_->session.stage() != cap::Stage::Closed && state_->session.stage() != cap::Stage::Empty)
            state_->view.activate(activationToken);
        else return false;
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
    if (action != cap::Action::Record && action != cap::Action::Scroll) return;
    if (action == cap::Action::Record) state_->recordingCapability = std::move(capability);
    else state_->scrollingCapability = std::move(capability);
    state_->updateLiveCapabilities();
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
    state_->stopping = true; state_->pending = false; state_->preparingLive = false;
    state_->authorizationDeadline.stop();
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
    if (!alive) return;
    if (state_->authorization) state_->authorization->hide();
    state_->live.shutdown();
    if (alive) state_->finishShutdown();
}
bool CaptureWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (state_->authorization && watched == state_->authorization->windowHandle() && event->type() == QEvent::Expose)
        QTimer::singleShot(0, this, [this] { state_->requestAuthorizedSource(); });
    return QObject::eventFilter(watched, event);
}
} // namespace HuhuCapture::Application
