#pragma once

#include "recording/AudioSource.h"
#include <cstdint>
#include <memory>

namespace HuhuCapture::Platform::Linux {

// The official PipeWire audio-capture stream path, with one explicit sink
// monitor and/or one default microphone. ScreenCast's restricted video FD is
// never reused as audio authorization. No requested source may be omitted.
std::unique_ptr<Recording::AudioSource> makePipeWireAudio(bool systemAudio, bool microphone,
    std::uint64_t maximumQueuedBytes = 16ull * 1024 * 1024);

} // namespace HuhuCapture::Platform::Linux
