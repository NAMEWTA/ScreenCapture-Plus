#include "core/MediaFrame.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>

using namespace HuhuCapture::Core;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void layouts()
{
    check(!packedFrameFormat(FramePixelFormat::Bgra8, -1, 32), "negative image dimensions must fail");
    check(!packedFrameFormat(FramePixelFormat::Nv12, 3, 0), "zero image dimensions must fail");
    check(!packedFrameFormat(static_cast<FramePixelFormat>(55), 3, 4), "unsupported pixel formats must fail");
    check(!packedFrameFormat(FramePixelFormat::I420, 3, 5, {}, FrameAlpha::Straight),
        "non-alpha formats must not promise an alpha plane");

    const auto nv12 = packedFrameFormat(FramePixelFormat::Nv12, 3, 5);
    check(nv12 && validFrameFormat(*nv12), "odd-sized NV12 plane geometry must be checked independently");
    check(nv12->planeCount == 2 && nv12->planes[0].rowBytes == 3 && nv12->planes[0].rows == 5 &&
        nv12->planes[1].rowBytes == 4 && nv12->planes[1].rows == 3 && nv12->bytes == 27,
        "chroma dimensions must round up without dropping an odd edge");
    const auto i420 = packedFrameFormat(FramePixelFormat::I420, 3, 5);
    check(i420 && i420->planeCount == 3 && i420->planes[1].rowBytes == 2 &&
        i420->planes[2].rows == 3 && i420->bytes == 27, "I420 must retain three separately bounded planes");

    auto malformed = *nv12;
    malformed.planes[1].offset = 14;
    check(!validFrameFormat(malformed), "overlapping color planes must fail");
    malformed = *nv12;
    malformed.planes[1].rows = 2;
    check(!validFrameFormat(malformed), "incomplete chroma planes must fail");
    malformed = *nv12;
    malformed.bytes = 26;
    check(!validFrameFormat(malformed), "the final plane must fit the allocation");

    const FramePlaneLayout padded{3, 32, 0, 12, 8, 3};
    check(validFramePlane(padded, 35), "the last row need not include its trailing padding");
    auto bad = padded;
    bad.size = 31;
    check(!validFramePlane(bad, 35), "a one-byte short row extent must fail");
    bad = padded;
    bad.stride = 7;
    check(!validFramePlane(bad, 35), "rows must not overlap");
    bad = padded;
    bad.offset = (std::numeric_limits<std::size_t>::max)();
    check(!validFramePlane(bad, 35), "offset addition must not wrap");
    bad = padded;
    bad.stride = (std::numeric_limits<std::ptrdiff_t>::min)();
    check(!validFramePlane(bad, 35), "the minimum signed stride must not overflow an absolute value");

    const FramePlaneLayout inverted{0, 28, 20, -10, 8, 3};
    check(validFramePlane(inverted, 28), "a checked negative stride must be accepted");
    bad = inverted;
    bad.firstRow = 19;
    check(!validFramePlane(bad, 28), "negative strides must not address before their allocation");
}

void leases()
{
    const auto format = *packedFrameFormat(FramePixelFormat::Bgra8, 2, 3);
    FramePool pool({2, 96, 8192});
    check(pool.configure(format) == FramePoolStatus::Ready && pool.allocatedBytes() == 48,
        "the configured pool must account for all slots");
    auto first = pool.tryAcquire();
    auto second = pool.tryAcquire();
    check(first && second && !pool.tryAcquire(), "a full pool must not allocate another frame");
    std::ranges::fill(first->plane(0), std::uint8_t{17});
    FrameMetadata metadata;
    metadata.sessionId = 8;
    metadata.sourceId = 3;
    metadata.timing.sourcePts = 17ms;
    metadata.timing.sourceClock = FrameClockDomain::PipeWire;
    metadata.timing.clockInstance = 8;
    metadata.timing.graphClockId = 42;
    metadata.timing.sourceSequence = 31;
    metadata.timing.receivedAt = MediaClock::now();
    metadata.crop = FrameCrop{1, 1, 1, 2};
    metadata.transform = FrameTransform::Reflected90;
    auto lease = std::move(*first).publish(metadata);
    check(lease && !*first && lease.plane(0).size() == 24 && lease.row(0, 2).size() == 8 &&
        lease.row(0, 2)[0] == 17 && lease.row(1, 0).empty() && lease.row(0, 3).empty(),
        "publish must transfer immutable pixels and keep row bounds");
    check(lease.metadata().timing.sourcePts == 17ms && lease.metadata().crop == metadata.crop &&
        lease.metadata().transform == FrameTransform::Reflected90 && lease.formatGeneration() == 1,
        "clock/crop/transform/source metadata must survive handoff");
    second.reset();
    auto replacement = pool.tryAcquire();
    check(replacement.has_value(), "discarding an unpublished frame must return its slot");
    auto other = std::move(*replacement).publish({});
    other = std::move(lease);
    check(other && !lease && pool.tryAcquire().has_value(), "move assignment must release its former slot");
    other.reset();
    other.reset();
    check(pool.tryAcquire().has_value(), "lease reset must be idempotent");

    auto invalid = pool.tryAcquire();
    metadata.crop = FrameCrop{0, 0, 3, 3};
    check(!std::move(*invalid).publish(metadata), "out-of-frame crops must not be published");
    check(pool.tryAcquire().has_value(), "invalid metadata must return the acquired slot");
    metadata.crop.reset();
    metadata.timing.sourceClock = FrameClockDomain::Unknown;
    check(!validFrameMetadata(metadata, format), "source PTS must not masquerade as an unknown/steady epoch");
    metadata.timing.sourceClock = FrameClockDomain::PipeWire;
    metadata.timing.clockInstance = 0;
    check(!validFrameMetadata(metadata, format), "a source clock must identify its session instance");
}

void planeViews()
{
    auto inverted = *packedFrameFormat(FramePixelFormat::Bgra8, 2, 3);
    inverted.bytes = 35;
    inverted.planes[0] = {3, 32, 24, -12, 8, 3};
    FramePool pool({1, 200, 8});
    check(pool.configure(inverted) == FramePoolStatus::Ready,
        "a bottom-up allocation with leading/trailing padding must configure");
    auto writable = pool.tryAcquire();
    for (std::uint32_t row = 0; row < 3; ++row)
        std::ranges::fill(writable->row(0, row), static_cast<std::uint8_t>(10 + row));
    auto frame = std::move(*writable).publish({});
    check(frame.row(0, 0)[0] == 10 && frame.row(0, 2)[7] == 12 &&
        frame.plane(0)[24] == 10 && frame.plane(0)[0] == 12 && frame.plane(0)[8] == 0,
        "logical rows must honor negative stride without overwriting physical padding");
    frame.reset();
    for (const auto pixels : {FramePixelFormat::Nv12, FramePixelFormat::I420}) {
        const auto format = *packedFrameFormat(pixels, 3, 5);
        check(pool.configure(format) == FramePoolStatus::Ready, "odd multi-plane frames must configure");
        writable = pool.tryAcquire();
        for (std::uint32_t plane = 0; plane < format.planeCount; ++plane)
            std::ranges::fill(writable->plane(plane), static_cast<std::uint8_t>(30 + plane));
        frame = std::move(*writable).publish({});
        for (std::uint32_t plane = 0; plane < format.planeCount; ++plane)
            check(frame.row(plane, format.planes[plane].rows - 1).back() == 30 + plane &&
                frame.row(plane, format.planes[plane].rows).empty(),
                "separate chroma planes and final odd edges must remain independently bounded");
        frame.reset();
    }
}

void generations()
{
    const auto small = *packedFrameFormat(FramePixelFormat::Bgra8, 2, 2);
    const auto large = *packedFrameFormat(FramePixelFormat::Bgra8, 4, 2);
    FramePool pool({2, 64, 8192});
    check(pool.configure(small) == FramePoolStatus::Ready, "initial generation must allocate");
    auto writable = pool.tryAcquire();
    std::ranges::fill(writable->plane(0), std::uint8_t{63});
    auto old = std::move(*writable).publish({});
    check(pool.configure(large) == FramePoolStatus::BudgetExceeded && pool.allocatedBytes() == 32,
        "outstanding old generations must stay charged against a format-change budget");
    check(!pool.tryAcquire() && old.plane(0)[0] == 63 && old.format().width == 2,
        "format rejection must not recycle a consumer's old memory or read it with new dimensions");
    old.reset();
    check(pool.allocatedBytes() == 0 && pool.configure(large) == FramePoolStatus::Ready &&
        pool.formatGeneration() == 2, "a released generation must return its complete byte reservation");
    auto frame = pool.tryAcquire();
    auto held = std::move(*frame).publish({});
    pool.clear();
    check(pool.allocatedBytes() == 64 && held.formatGeneration() == 2,
        "clear must not revoke a live lease");
    held.reset();
    check(pool.allocatedBytes() == 0, "the last lease must release retired storage");

    FrameLease survivor;
    {
        FramePool temporary({1, 16, 8192});
        check(temporary.configure(small) == FramePoolStatus::Ready, "temporary pool must allocate");
        auto producer = temporary.tryAcquire();
        producer->row(0, 1)[0] = 92;
        survivor = std::move(*producer).publish({});
    }
    check(survivor.row(0, 1)[0] == 92, "a lease must outlive the pool owner safely");
    std::thread release([frame = std::move(survivor)]() mutable { frame.reset(); });
    release.join();

    FramePool bounded({1, 15, 8192});
    check(bounded.configure(small) == FramePoolStatus::BudgetExceeded && bounded.allocatedBytes() == 0,
        "over-budget dimensions must fail before allocating");
    FramePool dimensions({1, 4096, 1});
    check(dimensions.configure(small) == FramePoolStatus::InvalidFormat,
        "caller-specified maximum dimensions must remain enforceable");
}

void crossThreadReuse()
{
    FramePool pool({1, 16, 8192});
    check(pool.configure(*packedFrameFormat(FramePixelFormat::Rgba8, 2, 2)) == FramePoolStatus::Ready,
        "cross-thread pool must allocate");
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        auto producer = pool.tryAcquire();
        check(producer.has_value(), "returned slot must be available to the producer");
        producer->row(0, 0)[0] = static_cast<std::uint8_t>(iteration);
        auto lease = std::move(*producer).publish({});
        std::atomic_bool returned{false};
        std::thread consumer([frame = std::move(lease), &returned, iteration]() mutable {
            check(frame.row(0, 0)[0] == iteration, "handoff must preserve frame contents");
            frame.reset();
            returned.store(true, std::memory_order_release);
        });
        while (!returned.load(std::memory_order_acquire)) std::this_thread::yield();
        consumer.join();
    }
}

} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<FrameLease>);
    static_assert(std::is_nothrow_move_constructible_v<FrameLease>);
    static_assert(std::is_nothrow_move_assignable_v<FrameLease>);
    layouts();
    leases();
    planeViews();
    generations();
    crossThreadReuse();
    std::cout << "media planes, metadata, bounded generations and cross-thread leases passed\n";
}
