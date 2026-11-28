#include "core/FrameQueue.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>

using namespace HuhuCapture::Core;
using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
FrameLease frame(FramePool& pool, std::uint64_t index)
{
    const auto deadline = MediaClock::now() + 5s;
    for (;;) {
        if (auto writable = pool.tryAcquire()) {
            writable->row(0, 0)[0] = static_cast<std::uint8_t>(index);
            FrameMetadata metadata;
            metadata.sessionId = 17; metadata.sourceId = 23;
            metadata.timing.sourceSequence = index;
            metadata.timing.receivedAt = MediaClock::now();
            auto value = std::move(*writable).publish(metadata);
            require(static_cast<bool>(value), "Publish real immutable bounded storage");
            return value;
        }
        require(MediaClock::now() < deadline, "Queue did not release a real source pool slot");
        std::this_thread::yield();
    }
}
void bounded(FramePool& pool)
{
    FrameQueue queue(2);
    require(queue.push(frame(pool, 1)) && queue.push(frame(pool, 2)) && queue.push(frame(pool, 3)), "Accept actual leases");
    auto value = queue.waitNext();
    require(value.frame && value.frame.metadata().timing.sourceSequence == 2, "A full producer drops exactly its oldest frame");
    require(value.frame.row(0, 0)[0] == 2, "Queue retains the correct actual immutable pixels");
    value.frame.reset();
    require(queue.push(frame(pool, 4)), "Released producer storage can be reused");
    value = queue.takeLatest();
    require(value.frame && value.frame.metadata().timing.sourceSequence == 4, "Sampling takes an actual newest frame");
    value.frame.reset();
    require(!queue.takeLatest().frame, "No new capture must not invent or repeat a frame");
    queue.close({FrameSourceEnd::SourceClosed, -32, "real source ended"});
    queue.close({FrameSourceEnd::Stopped, 0, {}});
    value = queue.waitNext();
    require(value.terminal && value.terminal->reason == FrameSourceEnd::SourceClosed && value.terminal->nativeCode == -32,
        "A later stop cannot erase the first source error");
    require(!queue.push(frame(pool, 5)), "Closed admission rejects and releases a late producer lease");
    const auto stats = queue.statistics();
    require(stats.submitted == 4 && stats.consumed == 2 && stats.droppedOldest == 2 &&
        stats.queued == 0 && stats.highWater == 2, "Accounting distinguishes source input, delivery and explicit drops");
}
void terminalDrain(FramePool& pool)
{
    FrameQueue queue(2);
    require(queue.push(frame(pool, 1)) && queue.push(frame(pool, 2)), "Queue real input before stopping");
    queue.close();
    require(queue.waitNext().frame.metadata().timing.sourceSequence == 1, "Normal stop drains oldest retained input");
    require(queue.waitNext().frame.metadata().timing.sourceSequence == 2, "Normal stop drains its actual last input");
    const auto end = queue.waitNext();
    require(!end.frame && end.terminal && end.terminal->reason == FrameSourceEnd::Stopped,
        "Only after drain does the consumer receive the actual terminal");

    FrameQueue discarded(2);
    require(discarded.push(frame(pool, 8)) && discarded.push(frame(pool, 9)), "Queue real abandoned input");
    discarded.close({FrameSourceEnd::PermissionDenied, 5, "permission revoked"});
    discarded.close({}, true);
    const auto abandoned = discarded.waitNext();
    require(abandoned.terminal && abandoned.terminal->reason == FrameSourceEnd::PermissionDenied &&
        discarded.statistics().droppedOnClose == 2, "Abandonment releases leases without replacing source failure");
}
void cancelledWait(FramePool& pool)
{
    FrameQueue queue(2);
    std::stop_source stop;
    std::latch entered(1);
    auto future = std::async(std::launch::async, [&] { entered.count_down(); return queue.waitNext(stop.get_token()); });
    entered.wait();
    stop.request_stop();
    require(future.wait_for(5s) == std::future_status::ready, "A real waiting worker must observe stop promptly");
    const auto cancelled = future.get();
    require(cancelled.interrupted && !cancelled.frame && !cancelled.terminal && !queue.closed(),
        "Consumer cancellation must not forge a source terminal");
    require(queue.push(frame(pool, 7)), "Interrupted consumer does not close producer admission");
    auto pending = queue.waitNext(stop.get_token());
    require(pending.interrupted && queue.statistics().queued == 1, "A cancelled read must not steal a queued frame");
    auto real = queue.waitNext();
    require(real.frame && real.frame.metadata().timing.sourceSequence == 7, "Actual later drain still owns the frame");
    real.frame.reset();
    auto blocked = std::async(std::launch::async, [&] { return queue.waitNext(); });
    queue.close({FrameSourceEnd::PlatformFailure, -5, "backend stopped"});
    require(blocked.wait_for(5s) == std::future_status::ready && blocked.get().terminal->nativeCode == -5,
        "Closing the actual source must wake a consumer without another frame");
}
void threaded(FramePool& pool)
{
    FrameQueue queue(3);
    auto consumer = std::async(std::launch::async, [&] {
        std::uint64_t last{}, count{};
        for (;;) {
            auto next = queue.waitNext();
            if (next.terminal) {
                require(next.terminal->reason == FrameSourceEnd::Stopped, "Observe the producer's completed terminal");
                break;
            }
            require(next.frame && next.frame.metadata().timing.sourceSequence > last, "Every delivered source sequence must advance");
            last = *next.frame.metadata().timing.sourceSequence;
            require(next.frame.row(0, 0)[0] == static_cast<std::uint8_t>(last), "Producer cannot reuse a slot retained by the consumer");
            ++count;
        }
        require(last == 1000, "Normal producer close must not discard the last actual frame");
        return count;
    });
    for (std::uint64_t index = 1; index <= 1000; ++index) require(queue.push(frame(pool, index)), "Active producer remains admitted");
    queue.close();
    require(consumer.wait_for(5s) == std::future_status::ready, "Real producer/consumer must fully drain without a GUI loop");
    const auto delivered = consumer.get();
    const auto statistics = queue.statistics();
    require(statistics.submitted == 1000 && statistics.consumed == delivered &&
        delivered + statistics.droppedOldest == 1000 && !statistics.queued && statistics.highWater <= 3,
        "Every real submitted frame must be delivered or explicitly counted as dropped");
}
void deadlineWait(FramePool& pool)
{
    FrameQueue queue(2);
    const auto expired = queue.waitUntil(MediaClock::now());
    require(!expired.frame && !expired.terminal && !expired.interrupted && !queue.closed(),
        "A sampling deadline is distinct from source closure and consumer cancellation");
    std::stop_source stop;
    std::latch entered(1);
    auto waiter = std::async(std::launch::async, [&] {
        entered.count_down(); return queue.waitUntil(MediaClock::now() + 30s, stop.get_token());
    });
    entered.wait(); stop.request_stop();
    require(waiter.wait_for(5s) == std::future_status::ready && waiter.get().interrupted,
        "Cancellation wakes an actual long-deadline read without waiting for another frame");
    require(queue.push(frame(pool, 13)), "Deadline/cancellation leave the real producer admitted");
    const auto cancelled = queue.waitUntil(MediaClock::now(), stop.get_token());
    require(cancelled.interrupted && queue.statistics().queued == 1,
        "An interrupted deadline read cannot consume an existing frame");
    auto actual = queue.waitUntil(MediaClock::now() + 5s);
    require(actual.frame && actual.frame.metadata().timing.sourceSequence == 13,
        "A subsequent reader retains the exact real input");
    actual.frame.reset();
    auto terminal = std::async(std::launch::async, [&] { return queue.waitUntil(MediaClock::now() + 30s); });
    queue.close({FrameSourceEnd::PermissionDenied, 5, "permission revoked"});
    require(terminal.wait_for(5s) == std::future_status::ready, "Source closure wakes a deadline read promptly");
    const auto ended = terminal.get();
    require(ended.terminal && ended.terminal->reason == FrameSourceEnd::PermissionDenied && ended.terminal->nativeCode == 5,
        "Deadline reads preserve the actual source failure");
}
}
int main()
{
    try {
        const auto format = *packedFrameFormat(FramePixelFormat::Bgra8, 8, 8);
        FramePool pool({8, format.bytes * 8, 64});
        require(pool.configure(format) == FramePoolStatus::Ready, "Allocate actual source frame slots");
        bounded(pool); terminalDrain(pool); cancelledWait(pool); deadlineWait(pool); threaded(pool);
        std::cout << "PASS bounded real leases, drop accounting, retained terminal/drain, cancellation and concurrent delivery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL frame queue: " << error.what() << '\n';
        return 1;
    }
}
