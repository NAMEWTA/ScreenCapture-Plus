#pragma once

#include "services/media/VideoInspection.h"

#include <memory>

struct AVFrame;

namespace HuhuCapture::Media::Detail {

// Private codec-service ownership. A reference-counted CPU AVFrame stays valid
// after the decoder/input close. Linux turns this first frame into a QImage;
// Windows keeps the existing MF cover path and uses FFmpeg only for the shared
// complete-file inspection. No codec object crosses into the UI or core model.
struct AvFrameDeleter {
    void operator()(AVFrame* frame) const noexcept;
};
using DecodedFrame = std::unique_ptr<AVFrame, AvFrameDeleter>;
struct FirstVideoFrame {
    Failure failure;
    DecodedFrame frame;
};
FirstVideoFrame decodeFirstVideoFrame(const QString& path, const Limits& limits, std::stop_token stop);

} // namespace HuhuCapture::Media::Detail
