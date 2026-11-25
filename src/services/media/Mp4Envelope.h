#pragma once

#include "services/media/MediaArchiveService.h"

#include <functional>
#include <span>

namespace HuhuCapture::Media::Detail {

// The caller supplies reads from the same already-open file that its decoder
// will consume. Only fixed-size box headers are read here. This is a physical
// top-level bounds check, never a replacement for the mature MOV demuxer and
// complete decoding of every declared audio/video track.
using Mp4HeaderReader = std::function<bool(std::uint64_t, std::span<std::uint8_t>)>;
Failure checkMp4Envelope(std::uint64_t fileBytes, const Mp4HeaderReader& read,
    const Limits& limits, std::stop_token stop);

} // namespace HuhuCapture::Media::Detail
