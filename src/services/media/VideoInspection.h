#pragma once

#include "services/media/MediaArchiveService.h"

namespace HuhuCapture::Media {

// Synchronous worker APIs. Inspection decodes every declared audio/video track
// through its actual decoder EOF. frames is the sum of decoded video frames;
// dimensions describes the first decoded video track. A preview is not evidence
// of complete validation and must never authorize archive publication.
Inspection inspectVideo(const QString& path, const Limits& limits = {}, std::stop_token stop = {});

enum class ThumbnailDecoder { None, MediaFoundation, Ffmpeg };

struct VideoThumbnailResult {
    Failure failure;
    QImage image;
    ThumbnailDecoder decoder{ThumbnailDecoder::None};
};

// Decode only the first usable video frame for an already managed history item.
// No upscaling, widgets or QPixmap, and no replacement of the original file.
// Windows tries the retained MF path first, then the common software decoder
// only for Unsupported. decoder records the path that actually produced pixels.
VideoThumbnailResult videoThumbnail(const QString& path, unsigned edge,
    const Limits& limits = {}, std::stop_token stop = {});

} // namespace HuhuCapture::Media
