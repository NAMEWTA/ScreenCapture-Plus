#pragma once

#include "services/media/VideoInspection.h"

#include <memory>

struct AVFrame;

namespace HuhuCapture::Media::Detail {

// Private codec-service ownership. A reference-counted CPU AVFrame stays valid
// after the decoder/input close. The shared software cover conversion produces
// a QImage on Linux and when Windows has no supported native cover decoder.
// No codec object crosses into the UI or core model.
struct AvFrameDeleter {
    void operator()(AVFrame* frame) const noexcept;
};
using DecodedFrame = std::unique_ptr<AVFrame, AvFrameDeleter>;
struct FirstVideoFrame {
    Failure failure;
    DecodedFrame frame;
};
FirstVideoFrame decodeFirstVideoFrame(const QString& path, const Limits& limits, std::stop_token stop);
VideoThumbnailResult softwareVideoThumbnail(const QString& path, unsigned edge,
    const Limits& limits, std::stop_token stop);

} // namespace HuhuCapture::Media::Detail
