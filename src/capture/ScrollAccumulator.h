#pragma once

#include "capture/ScrollCaptureModel.h"
#include "core/PixelBuffer.h"
#include <string>
#include <utility>

namespace HuhuCapture::Capture {

enum class ScrollAppend { Appended, RetryScroll, WaitForSettling, ReachedBottom, Limit, Failed };

// Compatibility facade for the application's worker. The original strip,
// matching, settling, retry and growth rules have one owner: ScrollCaptureModel.
class ScrollAccumulator final {
public:
    struct Limits {
        // Complete working set: accumulation + previous BGRA + two gray frames.
        std::size_t maximumBytes{512u * 1024u * 1024u};
        int maximumHeight{36000};
    };
    bool start(std::span<const std::uint8_t> pixels, int width, int height, Limits limits)
    {
        ScrollCaptureModel candidate({limits.maximumHeight, limits.maximumBytes});
        const auto error = candidate.begin(pixels, width, height);
        if (error != ScrollStartError::None) {
            error_ = error == ScrollStartError::MemoryLimit ? "The initial scrolling working set exceeds its memory budget" :
                "The initial scrolling frame is invalid";
            return false;
        }
        model_ = std::move(candidate); frameBytes_ = pixels.size(); error_.clear(); return true;
    }
    ScrollAppend append(std::span<const std::uint8_t> pixels, bool automatic = true)
    {
        if (!model_.hasImage() || pixels.size() != frameBytes_) {
            error_ = "The scrolling source changed dimensions"; return ScrollAppend::Failed;
        }
        const auto update = model_.accept(pixels, automatic);
        if (update.reason == ScrollStopReason::MemoryLimit || update.reason == ScrollStopReason::HeightLimit)
            return ScrollAppend::Limit;
        if (update.reason == ScrollStopReason::ReachedBottom) return ScrollAppend::ReachedBottom;
        if (update.reason == ScrollStopReason::CaptureFailed || update.reason == ScrollStopReason::InvalidFrame) {
            error_ = "The scrolling frame could not be joined to the complete image"; return ScrollAppend::Failed;
        }
        if (update.appended) return ScrollAppend::Appended;
        return update.next == ScrollNextStep::Settle ? ScrollAppend::WaitForSettling : ScrollAppend::RetryScroll;
    }
    int width() const noexcept { return model_.width(); }
    int height() const noexcept { return model_.height(); }
    std::span<const std::uint8_t> pixels() const noexcept { return model_.pixels(); }
    std::vector<std::uint8_t> takePixels() noexcept { return model_.takePixels(); }
    std::size_t allocatedBytes() const noexcept { return model_.allocatedBytes(); }
    const std::string& error() const noexcept { return error_; }
private:
    ScrollCaptureModel model_;
    std::size_t frameBytes_{};
    std::string error_;
};

} // namespace HuhuCapture::Capture
