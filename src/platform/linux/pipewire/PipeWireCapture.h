#pragma once

#include "core/MediaFrame.h"
#include "platform/linux/UniqueFd.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace HuhuCapture::Platform::Linux {

struct PipeWireTarget {
    // Both values come from the same authorized Portal session as the FD.
    std::uint32_t nodeId{};
    std::optional<std::uint64_t> serial;
};

struct PipeWireCaptureOptions {
    Core::FramePoolLimits pool{4, 128u * 1024u * 1024u, 16384};
    std::chrono::milliseconds firstFrameTimeout{5000};
    // Zero leaves the native rate unrestricted. Recording policy supplies its
    // actual requested limit; this adapter does not duplicate/drop to invent FPS.
    std::uint32_t maximumFrameRate{};
    std::uint64_t sessionId{};
    std::uint64_t sourceId{};
    // A pause after actual capture is different from a quiet Streaming graph.
    // Only a native PAUSED transition starts this absolute recovery deadline.
    std::chrono::milliseconds pauseTimeout{5000};
};

struct PipeWireFailure {
    enum class Code {
        InvalidArgument, Initialization, Connection, Stream, UnsupportedFormat,
        UnsupportedMemory, InvalidBuffer, Budget, FirstFrameTimeout, Consumer, Allocation, PauseTimeout
    };
    Code code{Code::Initialization};
    int nativeCode{};
    std::string message;
};

struct PipeWireCaptureStats {
    std::uint64_t buffersDequeued{};
    std::uint64_t buffersReturned{};
    std::uint64_t framesDelivered{};
    std::uint64_t poolDrops{};
    std::uint64_t corruptedDrops{};
    std::uint64_t emptyDrops{};
    std::uint64_t formatChanges{};
    std::uint64_t memFdFrames{};
    std::uint64_t memPtrFrames{};
    std::size_t peakPoolBytes{};
};

struct PipeWireCaptureResult {
    std::optional<PipeWireFailure> failure;
    PipeWireCaptureStats stats;
    bool stopped{};
};

struct PipeWireCaptureCallbacks {
    // Called before any frame of that generation. Returning false rejects a
    // format the consumer cannot handle; it never keeps reading the old layout.
    std::function<bool(const Core::FrameFormat&, std::uint64_t generation)> formatChanged;
    // Return false for a clean consumer-requested stop. Enqueue/move the lease
    // into a bounded encoder queue; do not wait for a GUI callback or encode here.
    std::function<bool(Core::FrameLease)> frame;
    // Actual native STREAMING/PAUSED transitions and terminal inactivity, on
    // this source worker. Consumers may repeat a static frame only while true;
    // absence of a new damage buffer is not itself a paused/disconnected source.
    std::function<void(bool streaming)> activityChanged;
};

// Worker-only data plane. Blocks the calling worker, owns one independent PW
// loop, and returns only after stream/context teardown. There are no Qt objects,
// GUI joins or global PipeWire reconnects. Cancellation signals the private loop.
// The remote FD is consumed on every outcome, including before stream creation.
// The caller retains the Portal session until this function returns.
PipeWireCaptureResult runPipeWireCapture(UniqueFd remote, PipeWireTarget target,
    PipeWireCaptureOptions options, PipeWireCaptureCallbacks callbacks,
    std::stop_token stop = {});

} // namespace HuhuCapture::Platform::Linux
