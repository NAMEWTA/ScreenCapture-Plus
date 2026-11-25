#pragma once

#include "recording/AudioSource.h"
#include "recording/FrameSource.h"

namespace HuhuCapture::Recording {

// The pinned libx264/AAC path writes through the already claimed descriptor.
// Sources, codec calls and finalization belong to the recording worker.
BackendResult runSoftwareMp4Recording(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> video, std::unique_ptr<AudioSource> audio,
    const BackendEvents& events = {}, std::stop_token stop = {});

} // namespace HuhuCapture::Recording
