#pragma once

#include "recording/FrameSource.h"

namespace HuhuCapture::Recording {

// Shared production gifski path. Receives actual platform-owned frames and an
// identity-checked writable file object; never opens/truncates a pathname.
// The source is stopped/destroyed before gifski_finish joins its output thread.
BackendResult runGifRecording(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> source, const BackendEvents& events = {},
    std::stop_token stop = {});

} // namespace HuhuCapture::Recording
