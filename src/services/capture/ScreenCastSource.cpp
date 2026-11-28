#include "services/capture/ScreenCastSource.h"
#include "services/capture/LiveCaptureSource.h"
#include "services/capture/LiveFrameReader.h"
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <atomic>
#include <utility>

namespace HuhuCapture::Capture {

struct ScreenCastSource::State {
    ScreenCastSource* owner;
    std::unique_ptr<LiveCaptureSource> live;
    std::shared_ptr<LiveFrameReaderState> consumer;
    std::unique_ptr<Core::FrameSource> reader;
    QTimer returned;
    quint64 token{}, sourceId{}, generation{}, refreshId{};
    QSize extent;
    bool active{}, stopping{}, ready{}, readerTaken{}, failureReported{};
    explicit State(ScreenCastSource* target) : owner(target)
    {
        returned.setInterval(10);
        QObject::connect(&returned, &QTimer::timeout, owner, [this] {
            if (!consumer || !consumer->returned.exchange(false, std::memory_order_acq_rel)) return;
            returned.stop();
            if (live) live->consumerFinished(sourceId, generation);
        });
    }
    void fail(const Failure& error)
    {
        if (!active || failureReported) return;
        failureReported = true;
        const QPointer<ScreenCastSource> alive(owner);
        Q_EMIT owner->failed(error);
        if (alive) owner->stop();
    }
};

ScreenCastSource::ScreenCastSource(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}
ScreenCastSource::~ScreenCastSource()
{
    state_->returned.stop();
    state_->reader.reset();
    // The live owner joins only its own producer/preview workers on abrupt
    // destruction. A borrowed media reader retains independent core objects.
    state_->live.reset();
}
bool ScreenCastSource::busy() const noexcept { return state_->active; }
bool ScreenCastSource::start(QWindow* parent, bool cursor, quint32 maximumFrameRate)
{
    if (QThread::currentThread() != thread() || state_->active || !parent ||
        !parent->isVisible() || !parent->handle()) return false;
    state_->active = true;
    state_->stopping = state_->ready = state_->readerTaken = state_->failureReported = false;
    state_->token = state_->sourceId = state_->generation = state_->refreshId = 0;
    state_->consumer.reset();
    state_->reader.reset();
    state_->live = std::make_unique<LiveCaptureSource>();
    connect(state_->live.get(), &LiveCaptureSource::prepared, this,
        [this](quint64 token, const Snapshot& snapshot, const Failure& error) {
            if (!state_->active || token != state_->token) return;
            if (error) { if (!state_->stopping) state_->fail(error); return; }
            if (state_->stopping) { state_->live->stop(token); return; }
            state_->sourceId = snapshot.sourceId; state_->generation = snapshot.generation;
            state_->extent = snapshot.image.size(); state_->ready = true;
            const auto* frame = state_->live->previewFrame(state_->sourceId, state_->generation);
            if (!frame) {
                state_->fail({Failure::Code::SourceChanged, 0, QStringLiteral("The actual authorized preview frame is unavailable")});
                return;
            }
            Q_EMIT ready(*frame);
        });
    connect(state_->live.get(), &LiveCaptureSource::captureReady, this,
        [this](quint64 request, quint64 sourceId, quint64 generation, const Failure& error) {
            if (!state_->active || request != state_->refreshId || sourceId != state_->sourceId ||
                generation != state_->generation) return;
            state_->refreshId = 0;
            if (error) { if (!state_->stopping) state_->fail(error); return; }
            if (state_->stopping) return;
            auto claimed = state_->live->claim(sourceId, generation, {0, 0,
                static_cast<std::uint32_t>(state_->extent.width()), static_cast<std::uint32_t>(state_->extent.height())});
            if (!claimed || !claimed->firstFrame || !claimed->producerFinished.valid()) {
                state_->fail({Failure::Code::SourceChanged, 0, QStringLiteral("The refreshed authorized stream could not be claimed")});
                return;
            }
            state_->consumer = std::make_shared<LiveFrameReaderState>();
            state_->consumer->consumer = std::move(*claimed);
            state_->reader = makeLiveFrameReader(state_->consumer);
            state_->returned.start();
            Q_EMIT refreshed(state_->consumer->consumer.firstFrame);
        });
    connect(state_->live.get(), &LiveCaptureSource::sourceClosed, this,
        [this](quint64 sourceId, quint64 generation, const Failure& error) {
            if (sourceId == state_->sourceId && generation == state_->generation && error && !state_->stopping)
                state_->fail(error);
        });
    connect(state_->live.get(), &LiveCaptureSource::stopped, this,
        [this](quint64 token, const Failure&) {
            if (!state_->active || token != state_->token) return;
            state_->live->shutdown();
        });
    connect(state_->live.get(), &LiveCaptureSource::shutdownFinished, this, [this] {
        if (!std::exchange(state_->active, false)) return;
        state_->returned.stop(); state_->ready = false;
        Q_EMIT stopped();
    });
    LiveCaptureOptions options;
    options.maximumFrameRate = maximumFrameRate;
    options.cursor = cursor;
    options.pool = {4, 256u * 1024u * 1024u, 16384};
    state_->token = state_->live->request(parent, options);
    if (!state_->token) {
        state_->active = false; state_->live.reset(); return false;
    }
    return true;
}
bool ScreenCastSource::refresh()
{
    if (QThread::currentThread() != thread() || !state_->active || state_->stopping ||
        !state_->ready || state_->refreshId || state_->consumer || state_->readerTaken) return false;
    state_->refreshId = state_->live->refreshForCapture(state_->sourceId, state_->generation);
    return state_->refreshId != 0;
}
std::unique_ptr<Core::FrameSource> ScreenCastSource::takeFrameSource()
{
    if (QThread::currentThread() != thread() || !state_->active || state_->stopping ||
        state_->refreshId || state_->readerTaken || !state_->reader) return {};
    state_->readerTaken = true;
    return std::move(state_->reader);
}
void ScreenCastSource::stop()
{
    if (!state_->active || state_->stopping) return;
    state_->stopping = true;
    state_->reader.reset();
    if (state_->consumer) state_->consumer->consumer.stop.request_stop();
    if (state_->live) {
        state_->live->stop(state_->token);
        state_->live->shutdown();
    }
}
} // namespace HuhuCapture::Capture
