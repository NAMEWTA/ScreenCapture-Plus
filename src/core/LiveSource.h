#pragma once

#include "core/FrameQueue.h"
#include <atomic>
#include <future>
#include <memory>
#include <stop_token>

namespace HuhuCapture::Core {

enum class LiveSourceActivity { Starting, Streaming, Paused, Closed };

// One claimed consumer for one actual authorized stream. All members are safe
// to use on its media worker; this handle has no QObject or native session.
// stop requests producer shutdown, but does not falsely close a queue before
// the native stream has returned. Its first actual terminal remains observable.
struct LiveSourceConsumer {
    std::uint64_t sourceId{}, generation{};
    FrameCrop selection; // confirmed coordinates in the normalized first frame
    std::shared_ptr<FrameQueue> frames;
    std::shared_ptr<const std::atomic<LiveSourceActivity>> activity;
    std::stop_source stop;
    // Signalled on the producer worker only after native capture has returned.
    // A media worker can drain without waiting for the GUI delivery of its
    // QFutureWatcher; queue closure alone can also mean explicit cancellation.
    std::shared_future<void> producerFinished;
    // The actual native frame used for the selection preview is transferred
    // exactly once. A quiet Streaming source need not deliver another damage
    // frame; consumers prefer a newer queued frame, otherwise use this lease.
    // Holding it consumes one of the same bounded producer pool slots.
    FrameLease firstFrame;
    // Admission time, distinct from the original first frame's source time.
    // Time spent choosing the region is never pre-recorded media duration.
    MediaClock::time_point claimedAt{};
    bool streaming() const noexcept
    { return activity && activity->load(std::memory_order_acquire) == LiveSourceActivity::Streaming && frames && !frames->closed(); }
};

} // namespace HuhuCapture::Core
