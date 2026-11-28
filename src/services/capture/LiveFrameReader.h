#pragma once

#include "core/FrameSource.h"
#include "core/LiveSource.h"
#include <atomic>
#include <memory>

namespace HuhuCapture::Capture {

// Shared only by one GUI authorization owner and its one media consumer.
// The GUI observes returned after the worker's stopAndWait barrier, then calls
// LiveCaptureSource::consumerFinished for this exact source/generation.
struct LiveFrameReaderState {
    Core::LiveSourceConsumer consumer;
    std::atomic_bool returned{};
};

// One native-independent adapter for the actual LiveCaptureSource consumer.
// No second queue, preview copy, producer, or terminal policy is introduced.
std::unique_ptr<Core::FrameSource> makeLiveFrameReader(std::shared_ptr<LiveFrameReaderState> state);

} // namespace HuhuCapture::Capture
