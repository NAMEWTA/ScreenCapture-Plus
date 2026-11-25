#pragma once

#include "services/media/MediaArchiveService.h"
#include "huhucapture_native.h"

namespace HuhuCapture::Media {

// Uses the decoder already present in the one aggregated Rust library. This
// is a worker-only full-file validation, not Qt's permissive preview reader.
// The optional receive remains owned by the caller throughout this call.
Inspection inspectGif(const QString& path, const Limits& limits = {},
    std::stop_token stop = {}, const HuhuReceive* receive = nullptr);

} // namespace HuhuCapture::Media
