#pragma once

#include "core/RecordingResult.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace VideoGif {

struct GifParam {
    std::atomic<bool> isFinish{ false };
    std::wstring path;
    int w{};
    int h{};
    int x{};
    int y{};
    std::uint32_t fps{ 16 };
    // Written on the capture thread; inspected only after joining it.
    HuhuCapture::Core::RecordingResult result;
};

void createGif(GifParam* params);
void createGif2(GifParam* params);

} // namespace VideoGif
