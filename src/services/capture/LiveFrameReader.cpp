#include "services/capture/LiveFrameReader.h"
#include <utility>

namespace HuhuCapture::Capture {
namespace {
class StreamReader final : public Core::FrameSource {
public:
    explicit StreamReader(std::shared_ptr<LiveFrameReaderState> state) : state_(std::move(state)) {}
    ~StreamReader() override
    {
        // An unconsumed reader can be abandoned on the GUI thread. The live
        // owner still awaits the real producer and Portal teardown.
        state_->consumer.stop.request_stop();
        state_->consumer.firstFrame.reset();
        state_->returned.store(true, std::memory_order_release);
    }
    Core::FrameSourceKind kind() const noexcept override { return Core::FrameSourceKind::PipeWire; }
    void stopAndWait() noexcept override
    {
        auto& source = state_->consumer;
        source.stop.request_stop();
        if (source.producerFinished.valid()) source.producerFinished.wait();
        source.firstFrame.reset();
        if (source.frames) { auto pending = source.frames->takeLatest(); pending.frame.reset(); }
        state_->returned.store(true, std::memory_order_release);
    }
    Core::FrameRead read(Core::MediaClock::time_point deadline, std::stop_token stop) override
    {
        auto& source = state_->consumer;
        if (!source.frames || !source.activity)
            return {Core::FrameReadStatus::Failed, {}, 0, "The authorized source has no consumer queue"};
        auto next = source.frames->takeLatest();
        if (next.terminal) return terminal(*next.terminal, stop);
        if (stop.stop_requested() || source.stop.stop_requested())
            return {Core::FrameReadStatus::Stopped, {}, 0, {}};
        const auto activity = source.activity->load(std::memory_order_acquire);
        if (activity == Core::LiveSourceActivity::Paused)
            return {Core::FrameReadStatus::Paused, {}, 0, "The authorized capture source is paused"};
        if (next.frame) {
            source.firstFrame.reset();
            return {Core::FrameReadStatus::Frame, std::move(next.frame), 0, {}};
        }
        if (source.firstFrame && activity == Core::LiveSourceActivity::Streaming)
            return {Core::FrameReadStatus::Frame, std::move(source.firstFrame), 0, {}};
        next = source.frames->waitUntil(deadline, stop);
        if (next.terminal) return terminal(*next.terminal, stop);
        if (next.interrupted || stop.stop_requested() || source.stop.stop_requested())
            return {Core::FrameReadStatus::Stopped, {}, 0, {}};
        if (source.activity->load(std::memory_order_acquire) == Core::LiveSourceActivity::Paused)
            return {Core::FrameReadStatus::Paused, {}, 0, "The authorized capture source is paused"};
        if (next.frame) return {Core::FrameReadStatus::Frame, std::move(next.frame), 0, {}};
        if (source.activity->load(std::memory_order_acquire) == Core::LiveSourceActivity::Streaming)
            return {Core::FrameReadStatus::Idle, {}, 0, {}};
        return {Core::FrameReadStatus::Failed, {}, 0, "The authorized capture source ended"};
    }
private:
    Core::FrameRead terminal(const Core::FrameSourceTerminal& value, std::stop_token stop) const
    {
        const bool stopping = state_->consumer.stop.stop_requested() || stop.stop_requested();
        const bool stopped = stopping && (value.reason == Core::FrameSourceEnd::Stopped ||
            value.reason == Core::FrameSourceEnd::Cancelled);
        auto detail = value.detail;
        if (!stopped && detail.empty()) detail = "The authorized capture source ended without a matching stop request";
        return {stopped ? Core::FrameReadStatus::Stopped : Core::FrameReadStatus::Failed,
            {}, value.nativeCode, std::move(detail)};
    }
    std::shared_ptr<LiveFrameReaderState> state_;
};
}

std::unique_ptr<Core::FrameSource> makeLiveFrameReader(std::shared_ptr<LiveFrameReaderState> state)
{
    if (!state) return {};
    return std::make_unique<StreamReader>(std::move(state));
}

} // namespace HuhuCapture::Capture
