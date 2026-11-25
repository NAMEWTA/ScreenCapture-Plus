#include "services/capture/ScreenCastSource.h"

#ifdef HUHUCAPTURE_WITH_PORTAL
#include "platform/linux/portal/PortalCapture.h"
#include "platform/linux/pipewire/PipeWireCapture.h"
#endif
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace HuhuCapture::Capture {
namespace {
struct Mailbox {
    std::mutex mutex;
    std::condition_variable_any available;
    Core::FrameLease latest;
    std::optional<Failure> failure;
    std::stop_source stop;
    bool producerFinished{}, userStopped{};
};
struct ProducerFinished {
    std::shared_ptr<Mailbox> mailbox;
    ~ProducerFinished()
    {
        { std::lock_guard lock(mailbox->mutex); mailbox->producerFinished = true; }
        mailbox->available.notify_all();
    }
};
class StreamReader final : public Core::FrameSource {
public:
    explicit StreamReader(std::shared_ptr<Mailbox> state) : state_(std::move(state)) {}
    ~StreamReader() override
    {
        { std::lock_guard lock(state_->mutex); state_->userStopped = true; }
        state_->stop.request_stop(); state_->available.notify_all();
    }
    Core::FrameSourceKind kind() const noexcept override { return Core::FrameSourceKind::PipeWire; }
    void stopAndWait() noexcept override
    {
        { std::lock_guard lock(state_->mutex); state_->userStopped = true; }
        state_->stop.request_stop(); state_->available.notify_all();
        std::unique_lock lock(state_->mutex);
        state_->available.wait(lock, [this] { return state_->producerFinished; });
    }
    Core::FrameRead read(Core::MediaClock::time_point deadline, std::stop_token stop) override
    {
        std::unique_lock lock(state_->mutex);
        state_->available.wait_until(lock, stop, deadline, [this] {
            return state_->latest || state_->failure || state_->producerFinished || state_->userStopped;
        });
        if (state_->failure) return {Core::FrameReadStatus::Failed, {}, state_->failure->nativeCode, state_->failure->detail.toStdString()};
        if (stop.stop_requested() || state_->userStopped) return {Core::FrameReadStatus::Stopped, {}, 0, {}};
        if (state_->latest) return {Core::FrameReadStatus::Frame, std::exchange(state_->latest, {}), 0, {}};
        if (state_->producerFinished) return {Core::FrameReadStatus::Failed, {}, 0, "The authorized capture stream ended"};
        return {Core::FrameReadStatus::Idle, {}, 0, {}};
    }
private:
    std::shared_ptr<Mailbox> state_;
};
#ifdef HUHUCAPTURE_WITH_PORTAL
Failure convert(const Platform::Linux::PortalFailure& failure)
{
    using Code = Platform::Linux::PortalFailure::Code;
    const auto code = failure.code == Code::Cancelled || failure.code == Code::ParentDestroyed ? Failure::Code::Cancelled :
        failure.code == Code::Denied ? Failure::Code::Denied : Failure::Code::Platform;
    return {code, failure.nativeCode, failure.message};
}
#endif
}

struct ScreenCastSource::State {
    ScreenCastSource* owner{};
    std::shared_ptr<Mailbox> mailbox;
    QPointer<QWindow> parent;
    QThreadPool pool;
    bool active{}, stopping{}, initializing{}, producer{}, previewReady{}, readerTaken{}, cursor{}, failureReported{};
    quint32 frameRate{};
    quint64 request{}, generation{};
#ifdef HUHUCAPTURE_WITH_PORTAL
    Platform::Linux::PortalCapture* portal{};
#endif
    explicit State(ScreenCastSource* value) : owner(value)
    { pool.setMaxThreadCount(1); pool.setExpiryTimeout(-1); }
    void finish()
    {
        if (!active) return;
        active = false; initializing = producer = false; request = 0;
#ifdef HUHUCAPTURE_WITH_PORTAL
        if (portal) { QObject::disconnect(portal, nullptr, owner, nullptr); portal->closeScreenCast(); portal->deleteLater(); portal = nullptr; }
#endif
        Q_EMIT owner->stopped();
    }
    void fail(Failure failure)
    {
        if (!active) return;
        const bool first = !failureReported;
        failureReported = true;
        if (mailbox) {
            { std::lock_guard lock(mailbox->mutex);
              if (!mailbox->failure) mailbox->failure = failure; else failure = *mailbox->failure; }
            mailbox->stop.request_stop(); mailbox->available.notify_all();
        }
        const QPointer<ScreenCastSource> lifetime(owner);
        if (first) Q_EMIT owner->failed(failure);
        if (lifetime && !producer) finish();
    }
};

ScreenCastSource::ScreenCastSource(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}
ScreenCastSource::~ScreenCastSource()
{
    if (state_->mailbox) { state_->mailbox->stop.request_stop(); state_->mailbox->available.notify_all(); }
    // The producer never waits for a GUI callback. Join before releasing the
    // Portal session or QObject context used by its queued preview notification.
    state_->pool.waitForDone();
}
bool ScreenCastSource::busy() const noexcept { return state_->active; }
bool ScreenCastSource::start(QWindow* parent, bool cursor, quint32 maximumFrameRate)
{
    if (QThread::currentThread() != thread() || state_->active || !parent || !parent->isVisible() || !parent->handle()) return false;
    state_->active = true; state_->stopping = state_->producer = state_->previewReady = state_->readerTaken = false;
    state_->failureReported = false;
    state_->parent = parent; state_->cursor = cursor; state_->frameRate = maximumFrameRate;
    state_->mailbox = std::make_shared<Mailbox>(); ++state_->generation;
#ifdef HUHUCAPTURE_WITH_PORTAL
    state_->initializing = true;
    state_->portal = new Platform::Linux::PortalCapture(this);
    connect(state_->portal, &Platform::Linux::PortalCapture::initialized, this,
        [this](const Platform::Linux::PortalCapabilities& capabilities, const Platform::Linux::PortalFailure& failure) {
            if (!state_->active) return;
            state_->initializing = false;
            if (state_->stopping) { state_->finish(); return; }
            if (failure || !capabilities.screenCast.available()) {
                state_->fail(convert(failure ? failure : capabilities.screenCast.failure)); return;
            }
            Platform::Linux::ScreenCastOptions options;
            // ScreenCast SourceType protocol bits: request every supported
            // screen/window choice, never a type the actual backend rejects
            // (for example the monitor-only wlr backend).
            options.monitors = (capabilities.sourceTypes & 1u) != 0;
            options.windows = (capabilities.sourceTypes & 2u) != 0;
            options.multiple = false;
            options.cursor = state_->cursor ? Platform::Linux::ScreenCastOptions::Cursor::Embedded : Platform::Linux::ScreenCastOptions::Cursor::Hidden;
            state_->request = state_->portal->requestScreenCast(state_->parent, options);
            if (!state_->request) state_->fail({Failure::Code::Busy, 0, QStringLiteral("The screen-sharing request could not start")});
        });
    connect(state_->portal, &Platform::Linux::PortalCapture::screenCastFinished, this,
        [this](quint64 request, const QVector<Platform::Linux::PortalStream>& streams, const QString&,
            const Platform::Linux::PortalFailure& failure) {
            if (!state_->active || request != state_->request) return;
            state_->request = 0;
            if (state_->stopping) { state_->finish(); return; }
            if (failure) { state_->fail(convert(failure)); return; }
            if (streams.size() != 1) {
                state_->fail({Failure::Code::InvalidSnapshot, 0, QStringLiteral("Select exactly one authorized source")}); return;
            }
            Platform::Linux::UniqueFd remote(state_->portal->takePipeWireFd());
            if (!remote) { state_->fail({Failure::Code::Platform, 0, QStringLiteral("The authorized PipeWire remote is unavailable")}); return; }
            state_->producer = true;
            const auto mailbox = state_->mailbox;
            const auto generation = state_->generation;
            const Platform::Linux::PipeWireTarget target{streams.front().nodeId, streams.front().pipeWireSerial};
            Platform::Linux::PipeWireCaptureOptions options;
            options.sessionId = generation; options.sourceId = generation; options.maximumFrameRate = state_->frameRate;
            options.pool = {4, 256u * 1024u * 1024u, 16384};
            auto* watcher = new QFutureWatcher<Platform::Linux::PipeWireCaptureResult>(this);
            connect(watcher, &QFutureWatcher<Platform::Linux::PipeWireCaptureResult>::finished, this,
                [this, watcher, mailbox, generation] {
                    Platform::Linux::PipeWireCaptureResult result;
                    try { result = watcher->result(); }
                    catch (...) {
                        result.failure = Platform::Linux::PipeWireFailure{Platform::Linux::PipeWireFailure::Code::Allocation,
                            0, "The authorized frame producer could not complete its work"};
                    }
                    watcher->deleteLater();
                    if (!state_->active || state_->generation != generation) return;
                    state_->producer = false;
                    if (result.failure && !state_->stopping) {
                        state_->fail({Failure::Code::Platform, result.failure->nativeCode, QString::fromStdString(result.failure->message)});
                    } else state_->finish();
                });
            watcher->setFuture(QtConcurrent::run(&state_->pool,
                [this, mailbox, generation, target, options, remote = std::move(remote)]() mutable {
                    // Set even when a callback allocation or the native adapter
                    // throws. Worker consumers never wait for a GUI watcher.
                    const ProducerFinished finished{mailbox};
                    bool first = true;
                    Platform::Linux::PipeWireCaptureCallbacks callbacks;
                    callbacks.formatChanged = [](const Core::FrameFormat&, std::uint64_t) { return true; };
                    callbacks.frame = [this, mailbox, generation, &first](Core::FrameLease frame) {
                        if (mailbox->stop.stop_requested()) return false;
                        if (first) {
                            first = false;
                            auto preview = std::make_shared<Core::FrameLease>(std::move(frame));
                            QMetaObject::invokeMethod(this, [this, mailbox, generation, preview] {
                                if (!state_->active || state_->stopping || state_->generation != generation) return;
                                state_->previewReady = true;
                                Q_EMIT ready(*preview);
                                // A FrameLease is move-only. The GUI borrows it
                                // only for this signal; afterward a static
                                // source still supplies its first real frame to
                                // the reader, unless a newer frame has arrived.
                                { std::lock_guard lock(mailbox->mutex);
                                  if (!mailbox->latest && !mailbox->userStopped) mailbox->latest = std::move(*preview); }
                                mailbox->available.notify_all();
                            }, ::Qt::QueuedConnection);
                            return true;
                        }
                        { std::lock_guard lock(mailbox->mutex); mailbox->latest = std::move(frame); }
                        mailbox->available.notify_all(); return true;
                    };
                    auto result = Platform::Linux::runPipeWireCapture(std::move(remote), target, options, std::move(callbacks), mailbox->stop.get_token());
                    { std::lock_guard lock(mailbox->mutex);
                      if (result.failure && !mailbox->userStopped && !mailbox->failure)
                          mailbox->failure = Failure{Failure::Code::Platform, result.failure->nativeCode, QString::fromStdString(result.failure->message)};
                    }
                    mailbox->available.notify_all(); return result;
                }));
        });
    connect(state_->portal, &Platform::Linux::PortalCapture::screenCastClosed, this,
        [this](const Platform::Linux::PortalFailure& failure) {
            if (!state_->active || state_->stopping) return;
            state_->fail(failure ? convert(failure) : Failure{Failure::Code::SourceChanged, 0, QStringLiteral("Screen sharing ended")});
        });
    state_->portal->initialize();
#else
    QTimer::singleShot(0, this, [this] { state_->fail({Failure::Code::Unavailable, 0, QStringLiteral("This build has no Portal/PipeWire source")}); });
#endif
    return true;
}
std::unique_ptr<Core::FrameSource> ScreenCastSource::takeFrameSource()
{
    if (!state_->active || state_->stopping || !state_->previewReady || state_->readerTaken) return {};
    state_->readerTaken = true; return std::make_unique<StreamReader>(state_->mailbox);
}
void ScreenCastSource::stop()
{
    if (!state_->active || state_->stopping) return;
    state_->stopping = true;
    if (state_->mailbox) {
        { std::lock_guard lock(state_->mailbox->mutex); state_->mailbox->userStopped = true; }
        state_->mailbox->stop.request_stop(); state_->mailbox->available.notify_all();
    }
#ifdef HUHUCAPTURE_WITH_PORTAL
    if (state_->request) { state_->portal->cancelRequest(state_->request); return; }
#endif
    if (!state_->producer && !state_->initializing) state_->finish();
}

} // namespace HuhuCapture::Capture
