// Linux uses the common software cover path; Windows reuses it only when its
// retained Media Foundation cover decoder reports Unsupported.
#include "services/media/FfmpegVideoInspection.h"

namespace HuhuCapture::Media {

VideoThumbnailResult videoThumbnail(const QString& path, unsigned edge, const Limits& limits, std::stop_token stop)
{
    return Detail::softwareVideoThumbnail(path, edge, limits, stop);
}

} // namespace HuhuCapture::Media
