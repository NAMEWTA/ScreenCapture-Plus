#include "recording/RecordingSession.h"

#include <QMetaObject>
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace HuhuCapture::Recording {
namespace {
Media::Failure backendFailure(const Core::RecordingResult& result)
{
    if (!result.firstError)
        return {Media::Error::InvalidMedia, QStringLiteral("The recording did not finalize complete media")};
    return {Media::Error::Write, QStringLiteral("Recording failed at stage %1 (native code %2)")
        .arg(static_cast<int>(result.firstError->stage)).arg(result.firstError->code)};
}
}

struct RecordingSession::State {
    struct Delivery {
        // The mutex only protects admission of a queued call versus QObject
        // destruction. It is never held while executing an owner-thread slot.
        std::mutex mutex;
        RecordingSession* owner{};
    };
    struct Work {
        quint64 token{};
        BackendRequest request;
        Backend backend;
        std::shared_ptr<Media::MediaArchiveService> archive;
        std::shared_ptr<Delivery> delivery;
        std::stop_source captureStop;
        std::stop_source processingStop;
        std::atomic<int> intent{-1};
        std::atomic<bool> abandoned{};
    };

    RecordingSession* owner;
    std::shared_ptr<Media::MediaArchiveService> archive;
    Backend backend;
    std::shared_ptr<Delivery> delivery;
    std::shared_ptr<Work> active;
    std::optional<PreparedRecording> prepared;
    std::optional<SessionResult> result;
    Core::MediaTime stoppedElapsed{};
    SessionState phase{SessionState::Idle};
    quint64 nextToken{1}, currentToken{};
    bool closing{};

    State(RecordingSession* session, std::shared_ptr<Media::MediaArchiveService> service, Backend capture)
        : owner(session), archive(std::move(service)), backend(std::move(capture)), delivery(std::make_shared<Delivery>())
    { delivery->owner = owner; }

    static void post(const std::shared_ptr<Delivery>& delivery, std::function<void(RecordingSession&)> callback)
    {
        std::lock_guard guard(delivery->mutex);
        if (!delivery->owner) return;
        const std::weak_ptr<Delivery> weak = delivery;
        QMetaObject::invokeMethod(delivery->owner, [weak, callback = std::move(callback)] {
            const auto alive = weak.lock();
            if (!alive) return;
            RecordingSession* owner{};
            { std::lock_guard guard(alive->mutex); owner = alive->owner; }
            if (owner) callback(*owner);
        }, Qt::QueuedConnection);
    }

    static StopIntent intent(const Work& work)
    {
        const auto selected = work.intent.load();
        return selected < 0 ? StopIntent::Save : static_cast<StopIntent>(selected);
    }

    static SessionResult execute(const std::shared_ptr<Work>& work)
    {
        SessionResult result; result.token = work->token;
        Media::StagingFile staging;
        const auto closeIntent = [&] {
            int unset = -1;
            work->intent.compare_exchange_strong(unset, static_cast<int>(StopIntent::Save));
            result.intent = intent(*work);
        };
        const auto retain = [&] { if (staging) result.recoveryPath = staging.retainForRecovery(); };
        const auto finishFailure = [&](Media::Failure failure) {
            closeIntent();
            result.failure = std::move(failure);
            retain();
            result.state = result.backend.media.framesWritten ? SessionState::RecoverableFailure : SessionState::Failed;
        };
        try {
            if (work->captureStop.stop_requested()) {
                closeIntent(); result.state = SessionState::Cancelled;
                result.backend.stopRequested = true; return result;
            }
            auto created = work->archive->createStaging();
            if (created.failure) { finishFailure(std::move(created.failure)); return result; }
            staging = std::move(created.file);
            std::error_code error;
            auto writer = staging.openWriter(error);
            if (!writer) { finishFailure({Media::Error::Write, QString::fromStdString(error.message())}); return result; }
            const BackendEvents events{
                [work](const PreparedRecording& prepared) {
                    post(work->delivery, [work, prepared](RecordingSession& owner) {
                        auto& state = *owner.state_;
                        if (state.active != work) return;
                        state.prepared = prepared;
                        if (state.closing || work->captureStop.stop_requested()) return;
                        state.phase = SessionState::Recording;
                        const QPointer<RecordingSession> alive(&owner);
                        Q_EMIT owner.prepared(work->token, prepared);
                        if (alive && state.active == work && state.phase == SessionState::Recording)
                            Q_EMIT owner.stateChanged(work->token, SessionState::Recording);
                    });
                },
                [work] {
                    const auto finishedAt = Core::MediaClock::now();
                    post(work->delivery, [work, finishedAt](RecordingSession& owner) {
                        auto& state = *owner.state_;
                        if (state.active != work) return;
                        if (state.prepared) state.stoppedElapsed = std::chrono::duration_cast<Core::MediaTime>(
                            finishedAt - state.prepared->started);
                        if (intent(*work) == StopIntent::Discard || state.closing) return;
                        state.phase = SessionState::Finalizing;
                        Q_EMIT owner.stateChanged(work->token, SessionState::Finalizing);
                    });
                }};
            result.backend = work->backend(work->request, std::move(writer), events, work->captureStop.get_token());
            // Close intent admission before deciding cleanup/publication. A UI
            // stop racing an automatic duration limit either wins this atomic
            // choice or is rejected; it cannot change an already chosen save.
            closeIntent();
            if (work->abandoned || work->processingStop.stop_requested()) {
                if (!result.backend.media.firstError && !result.backend.media.framesWritten) {
                    const auto discarded = staging.discard();
                    if (discarded) finishFailure(discarded);
                    else result.state = SessionState::Cancelled;
                } else finishFailure(result.backend.media.firstError ? backendFailure(result.backend.media) :
                    Media::Failure{Media::Error::Cancelled, QStringLiteral("The recording was retained during shutdown")});
                return result;
            }
            if (result.intent == StopIntent::Discard) {
                const auto discarded = staging.discard();
                if (discarded) finishFailure(discarded);
                else if (result.backend.media.firstError) {
                    result.failure = backendFailure(result.backend.media); result.state = SessionState::Failed;
                } else result.state = result.backend.media.framesWritten ? SessionState::Discarded : SessionState::Cancelled;
                return result;
            }
            if (result.backend.media.completion(result.backend.stopRequested) == Core::RecordingCompletion::Cancelled) {
                const auto discarded = staging.discard();
                if (discarded) finishFailure(discarded);
                else result.state = SessionState::Cancelled;
                return result;
            }
            if (!result.backend.media.canArchive()) { finishFailure(backendFailure(result.backend.media)); return result; }
            post(work->delivery, [work](RecordingSession& owner) {
                auto& state = *owner.state_;
                if (state.active != work || state.closing) return;
                state.phase = SessionState::Archiving;
                Q_EMIT owner.stateChanged(work->token, SessionState::Archiving);
            });
            result.archive = work->archive->commitStaged(staging,
                work->request.format == Format::Mp4 ? Media::Kind::Mp4 : Media::Kind::Gif,
                work->processingStop.get_token());
            if (!result.archive.durable()) {
                result.failure = result.archive.failure;
                if (!result.failure) result.failure = {Media::Error::Commit, QStringLiteral("Recording archive durability was not confirmed")};
                // A published path, including unknown durability, is not a
                // temporary file. Preserve it in result.archive on every error.
                if (result.archive.path.isEmpty()) retain();
                result.state = SessionState::RecoverableFailure;
            } else result.state = SessionState::Completed;
        } catch (const std::bad_alloc&) {
            finishFailure({Media::Error::Budget, QStringLiteral("The recording exceeded available memory")});
        } catch (...) {
            finishFailure({Media::Error::Write, QStringLiteral("The recording worker failed before completing its operation")});
        }
        return result;
    }

    void complete(const std::shared_ptr<Work>& work, SessionResult result)
    {
        if (active != work) return;
        active.reset(); phase = result.state;
        prepared = result.backend.prepared;
        stoppedElapsed = result.backend.elapsed;
        this->result = result;
        const QPointer<RecordingSession> alive(owner);
        Q_EMIT owner->stateChanged(result.token, result.state);
        if (!alive) return;
        Q_EMIT owner->finished(result);
        if (alive && closing && !active) Q_EMIT owner->shutdownFinished();
    }
};

RecordingSession::RecordingSession(std::shared_ptr<Media::MediaArchiveService> archive, Backend backend, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(archive), std::move(backend)))
{
    qRegisterMetaType<SessionState>();
    qRegisterMetaType<PreparedRecording>();
    qRegisterMetaType<SessionResult>();
}
RecordingSession::~RecordingSession()
{
    // A queued worker may outlive this QObject, but owns only values, the archive
    // service and its staging token. It never posts through a dangling QObject.
    { std::lock_guard guard(state_->delivery->mutex); state_->delivery->owner = nullptr; }
    if (state_->active) {
        state_->active->abandoned = true;
        state_->active->processingStop.request_stop();
        state_->active->captureStop.request_stop();
    }
}
quint64 RecordingSession::start(BackendRequest request)
{
    if (QThread::currentThread() != thread() || state_->closing || state_->active ||
        !state_->archive || !state_->backend || !state_->nextToken) return 0;
    std::shared_ptr<State::Work> work;
    QFutureWatcher<SessionResult>* watcher{};
    try {
        work = std::make_shared<State::Work>();
        work->token = state_->nextToken++; work->request = request;
        work->backend = state_->backend; work->archive = state_->archive; work->delivery = state_->delivery;
        watcher = new QFutureWatcher<SessionResult>(this);
        connect(watcher, &QFutureWatcher<SessionResult>::finished, this, [this, work, watcher] {
            auto future = watcher->future(); watcher->deleteLater();
            SessionResult result;
            try { result = future.takeResult(); }
            catch (...) {
                result.token = work->token; result.intent = State::intent(*work);
                result.failure = {Media::Error::Write, QStringLiteral("The recording worker did not return its completion result")};
            }
            state_->complete(work, std::move(result));
        });
        state_->active = work; state_->currentToken = work->token; state_->prepared.reset(); state_->result.reset();
        state_->stoppedElapsed = {}; state_->phase = SessionState::Preparing;
        // Reuse the project's QtConcurrent/QFutureWatcher completion pattern.
        // Admission is one active job per application session. The global
        // executor owns its lifetime, so QObject destruction cannot GUI-join a
        // native finalizer. Normal exit waits for shutdownFinished.
        watcher->setFuture(QtConcurrent::run([work] { return State::execute(work); }));
    } catch (...) {
        if (work) {
            work->abandoned = true; work->processingStop.request_stop(); work->captureStop.request_stop();
            if (state_->active == work) { state_->active.reset(); state_->phase = SessionState::Idle; }
        }
        delete watcher;
        return 0;
    }
    const auto token = work->token;
    Q_EMIT stateChanged(token, SessionState::Preparing);
    return token;
}
bool RecordingSession::requestStop(StopIntent intent)
{
    if (QThread::currentThread() != thread() || !state_->active || state_->closing) return false;
    const auto work = state_->active;
    int unset = -1;
    if (!work->intent.compare_exchange_strong(unset, static_cast<int>(intent))) return false;
    state_->phase = SessionState::Stopping;
    work->captureStop.request_stop();
    Q_EMIT stateChanged(work->token, SessionState::Stopping);
    return true;
}
void RecordingSession::shutdown()
{
    if (QThread::currentThread() != thread() || state_->closing) return;
    state_->closing = true;
    if (!state_->active) { Q_EMIT shutdownFinished(); return; }
    const auto work = state_->active;
    work->processingStop.request_stop(); work->captureStop.request_stop();
    state_->phase = SessionState::Stopping;
    Q_EMIT stateChanged(work->token, SessionState::Stopping);
}
bool RecordingSession::busy() const noexcept { return bool(state_->active); }
SessionState RecordingSession::state() const noexcept { return state_->phase; }
quint64 RecordingSession::token() const noexcept { return state_->currentToken; }
Core::MediaTime RecordingSession::elapsed() const noexcept
{
    if (state_->active && state_->prepared &&
        (state_->phase == SessionState::Recording || state_->phase == SessionState::Stopping))
        return std::chrono::duration_cast<Core::MediaTime>(Core::MediaClock::now() - state_->prepared->started);
    return state_->stoppedElapsed;
}
std::optional<SessionResult> RecordingSession::lastResult() const { return state_->result; }

} // namespace HuhuCapture::Recording
