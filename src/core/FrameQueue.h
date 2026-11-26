#pragma once

#include "core/MediaFrame.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace HuhuCapture::Core {

enum class FrameSourceEnd {
    Stopped, Cancelled, PermissionDenied, SourceClosed, UnsupportedFormat,
    PlatformFailure, BudgetExceeded
};
struct FrameSourceTerminal {
    FrameSourceEnd reason{FrameSourceEnd::Stopped};
    std::int64_t nativeCode{};
    std::string detail;
};
struct FrameQueueStatistics {
    std::uint64_t submitted{}, consumed{}, droppedOldest{}, droppedOnClose{};
    std::size_t queued{}, highWater{};
};
struct FrameQueueRead {
    FrameLease frame;
    // A closed queue yields its immutable first terminal only after all retained
    // frames have been consumed. Empty/open takeLatest has neither value.
    std::optional<FrameSourceTerminal> terminal;
    // A wait's stop token interrupts that read without closing or draining the
    // source queue. It never turns a platform failure into a clean source stop.
    bool interrupted{};
};

// A fixed-capacity, single-consumer capture queue. The producer never waits for
// encoding or GUI work: a full push releases the oldest lease and records it.
// FramePool remains responsible for the separate total pixel-memory budget.
// Keep this object alive until both producer and consumer have finished. The
// owner must close it during source teardown; destruction is not a shutdown API.
class FrameQueue final {
public:
    explicit FrameQueue(std::size_t capacity);
    ~FrameQueue();
    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    bool push(FrameLease frame);
    FrameQueueRead takeLatest();
    FrameQueueRead waitNext(std::stop_token stop = {});
    // Empty/open at the deadline is distinct from an immutable source terminal.
    FrameQueueRead waitUntil(MediaClock::time_point deadline, std::stop_token stop = {});
    // First terminal wins. Normal stop retains queued frames for encoder drain;
    // discardQueued is for explicit abandonment, and records all released input.
    void close(FrameSourceTerminal terminal = {}, bool discardQueued = false);
    FrameQueueStatistics statistics() const;
    bool closed() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Core
