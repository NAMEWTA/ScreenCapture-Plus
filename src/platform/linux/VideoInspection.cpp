// The same bounded software thumbnail service also supplies Windows covers
// when the platform has no native decoder for a supported file.
#include "services/media/FfmpegVideoInspection.h"

namespace HuhuCapture::Media {
VideoThumbnailResult videoThumbnail(const QString& path, unsigned edge, const Limits& limits, std::stop_token stop)
{
    return Detail::softwareVideoThumbnail(path, edge, limits, stop);
}
} // namespace HuhuCapture::Media
