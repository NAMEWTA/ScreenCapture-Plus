#pragma once

#include "core/MediaFrame.h"
#include "recording/RecordingBackend.h"

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace HuhuCapture::Recording {

using FrameReadStatus = Core::FrameReadStatus;
using FrameRead = Core::FrameRead;
using FrameSource = Core::FrameSource;

// Native polling adapters retain their capture surfaces. This borrowed span is
// consumed before another capture call; the shared adapter makes one bounded
// frame-pool copy, preserving the same ownership contract as PipeWire.
struct BorrowedBgraFrame {
    std::span<const std::uint8_t> pixels;
    std::size_t stride{};
    std::int64_t error{};
    std::string detail;
};
using BgraCapture = std::function<BorrowedBgraFrame(PixelRegion, bool cursor)>;
std::unique_ptr<FrameSource> makePollingFrameSource(const BackendRequest& request,
    CaptureSource kind, BgraCapture capture);

struct FrameSourceContract {
    Core::FrameFormat format;
    Core::FrameMetadata preview;
    std::uint64_t generation{};
    std::pair<int, int> normalizedSize;

    bool matches(const Core::FrameLease& frame) const noexcept
    {
        return frame && frame.formatGeneration() == generation && frame.format() == format &&
            frame.metadata().sessionId == preview.sessionId && frame.metadata().sourceId == preview.sourceId &&
            frame.metadata().crop == preview.crop && frame.metadata().transform == preview.transform;
    }
};
// Worker-only: rejection also drains the transferred producer before releasing it.
// Selection is in the normalized pixels of the already authorized preview.
// A generation, crop, transform or source change invalidates that selection,
// including changes that happen to retain the same normalized dimensions.
std::unique_ptr<FrameSource> makeCroppedFrameSource(std::unique_ptr<FrameSource> source,
    PixelRegion selection, FrameSourceContract expected, std::size_t maximumBytes);

} // namespace HuhuCapture::Recording
