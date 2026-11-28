#pragma once

#include "services/media/VideoInspection.h"
#include <functional>
#include <memory>

namespace HuhuCapture::Media {

using VideoCoverReader = std::function<VideoThumbnailResult(const QString&, unsigned,
    const Limits&, std::stop_token)>;

// Application-owned bounded QImage cache, keyed by exact file snapshot and
// requested size. load() is worker-only; all readers hold the same archive lease.
// A first-frame preview is never a complete-media validation or publication gate.
class ThumbnailCache final {
public:
    explicit ThumbnailCache(std::shared_ptr<MediaArchiveService> archive,
        VideoCoverReader videoCover = {}, Limits limits = {});
    ~ThumbnailCache();
    VideoThumbnailResult load(const Entry& entry, QSize maximumSize, std::stop_token stop = {});
    void clear();
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Media
