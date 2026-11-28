#include "core/FrameQueue.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace HuhuCapture::Core {

struct FrameQueue::State {
    explicit State(std::size_t capacity) : slots(capacity) {}
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::vector<FrameLease> slots;
    std::size_t head{}, size{};
    std::optional<FrameSourceTerminal> terminal;
    FrameQueueStatistics statistics;

    FrameLease pop() noexcept
    {
        auto frame = std::move(slots[head]);
        head = (head + 1) % slots.size();
        --size;
        return frame;
    }
    FrameQueueRead read()
    {
        if (size) {
            ++statistics.consumed;
            return {pop(), {}, false};
        }
        return {{}, terminal, false};
    }
};

FrameQueue::FrameQueue(std::size_t capacity)
{
    // This bounds queue bookkeeping; frame storage has its own stricter pool
    // budget. A zero-capacity queue cannot define a useful capture contract.
    if (!capacity || capacity > 1024) throw std::invalid_argument("Invalid frame queue capacity");
    state_ = std::make_unique<State>(capacity);
}
FrameQueue::~FrameQueue() = default;

bool FrameQueue::push(FrameLease frame)
{
    if (!frame) return false;
    FrameLease discarded;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->terminal) return false;
        if (state_->size == state_->slots.size()) {
            discarded = state_->pop();
            ++state_->statistics.droppedOldest;
        }
        state_->slots[(state_->head + state_->size) % state_->slots.size()] = std::move(frame);
        ++state_->size;
        ++state_->statistics.submitted;
        if (state_->size > state_->statistics.highWater) state_->statistics.highWater = state_->size;
    }
    // Releasing an old pool generation can free a large pixel allocation. Do it
    // outside the hot producer lock; no consumer/platform callback is invoked.
    discarded.reset();
    state_->changed.notify_one();
    return true;
}

FrameQueueRead FrameQueue::takeLatest()
{
    std::lock_guard lock(state_->mutex);
    while (state_->size > 1) {
        // FrameLease release only updates its core pool slot/shared ownership.
        // It cannot call back into a native producer or application object.
        state_->pop().reset();
        ++state_->statistics.droppedOldest;
    }
    return state_->read();
}

FrameQueueRead FrameQueue::waitNext(std::stop_token stop)
{
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, stop, [&] { return state_->size || state_->terminal.has_value(); });
    if (stop.stop_requested()) return {{}, {}, true};
    return state_->read();
}

FrameQueueRead FrameQueue::waitUntil(MediaClock::time_point deadline, std::stop_token stop)
{
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_until(lock, stop, deadline, [&] { return state_->size || state_->terminal.has_value(); });
    if (stop.stop_requested()) return {{}, {}, true};
    return state_->read();
}

void FrameQueue::close(FrameSourceTerminal terminal, bool discardQueued)
{
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->terminal) state_->terminal = std::move(terminal);
        // A later explicit abandonment can still release pending leases, while
        // preserving the actual earlier source failure for every consumer read.
        if (discardQueued) while (state_->size) {
            state_->pop().reset();
            ++state_->statistics.droppedOnClose;
        }
    }
    state_->changed.notify_all();
}

FrameQueueStatistics FrameQueue::statistics() const
{
    std::lock_guard lock(state_->mutex);
    auto result = state_->statistics;
    result.queued = state_->size;
    return result;
}
bool FrameQueue::closed() const
{
    std::lock_guard lock(state_->mutex);
    return state_->terminal.has_value();
}

} // namespace HuhuCapture::Core
