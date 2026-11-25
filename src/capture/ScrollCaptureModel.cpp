#include "capture/ScrollCaptureModel.h"

#include "core/ScrollStitch.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace HuhuCapture::Capture {

ScrollCaptureModel::ScrollCaptureModel(ScrollCaptureLimits limits) noexcept : limits_(limits) {}

std::size_t ScrollCaptureModel::allocatedBytes() const noexcept
{
    return result_.capacity() + previous_.capacity() + previousGray_.capacity() + currentGray_.capacity();
}

ScrollStartError ScrollCaptureModel::begin(std::span<const std::uint8_t> pixels, int width, int height) noexcept
{
    const auto layout = Core::pixelLayout(width, height, 4);
    if (!layout || pixels.size() != layout->bytes || limits_.stopAfterHeight <= 0)
        return ScrollStartError::InvalidFrame;
    const auto grayBytes = layout->bytes / 4;
    if (layout->bytes > limits_.maximumWorkingBytes / 2 ||
        grayBytes > (limits_.maximumWorkingBytes - layout->bytes * 2) / 2)
        return ScrollStartError::MemoryLimit;
    try {
        // Initialize transactionally: a caller which attempts to start a new
        // image cannot lose a previously completed one when allocation fails.
        ScrollCaptureModel next(limits_);
        next.width_ = width; next.frameHeight_ = next.resultHeight_ = height;
        next.result_.assign(pixels.begin(), pixels.end());
        next.previous_.assign(pixels.begin(), pixels.end());
        Core::toGrayscale(pixels, width, height, next.previousGray_);
        next.currentGray_.resize(grayBytes);
        if (next.allocatedBytes() > limits_.maximumWorkingBytes) return ScrollStartError::MemoryLimit;
        *this = std::move(next);
        return ScrollStartError::None;
    } catch (...) {
        return ScrollStartError::MemoryLimit;
    }
}

ScrollUpdate ScrollCaptureModel::stopped() const noexcept
{ return {ScrollNextStep::Stopped, reason_, false}; }

void ScrollCaptureModel::stop(ScrollStopReason reason) noexcept
{
    if (reason_ != ScrollStopReason::None) return;
    reason_ = reason == ScrollStopReason::None ? ScrollStopReason::Requested : reason;
    std::vector<std::uint8_t>().swap(previous_);
    std::vector<std::uint8_t>().swap(previousGray_);
    std::vector<std::uint8_t>().swap(currentGray_);
}

ScrollUpdate ScrollCaptureModel::dismissed(ScrollStopReason reason) noexcept
{
    if (++dismissals_ > ScrollCaptureRules::maximumDismissals) {
        stop(reason);
        return stopped();
    }
    return {ScrollNextStep::Scroll, ScrollStopReason::None, false};
}

ScrollUpdate ScrollCaptureModel::captureFailed() noexcept
{
    if (!running()) return stopped();
    return dismissed(ScrollStopReason::CaptureFailed);
}

bool ScrollCaptureModel::reserveAppend(int rows)
{
    if (rows <= 0 || resultHeight_ > (std::numeric_limits<int>::max)() - rows) return false;
    const auto layout = Core::pixelLayout(width_, resultHeight_ + rows, 4);
    if (!layout) return false;
    const auto otherBytes = previous_.capacity() + previousGray_.capacity() + currentGray_.capacity();
    if (otherBytes > limits_.maximumWorkingBytes || layout->bytes > limits_.maximumWorkingBytes - otherBytes)
        return false;
    if (layout->bytes > result_.capacity()) {
        const auto available = limits_.maximumWorkingBytes - otherBytes;
        // Keep amortized growth, but do not let vector's implicit growth exceed
        // the explicit working budget when it doubles a nearly full result.
        const auto grow = result_.capacity() > available - result_.capacity()
            ? available : result_.capacity() * 2;
        result_.reserve(std::max(layout->bytes, grow));
        if (result_.capacity() > available) return false;
    }
    return true;
}

ScrollUpdate ScrollCaptureModel::accept(std::span<const std::uint8_t> pixels) noexcept
{
    if (!running()) return stopped();
    if (pixels.size() != previous_.size()) {
        stop(ScrollStopReason::InvalidFrame);
        return stopped();
    }
    try {
        if (firstCheck_) {
            changeStartY_ = -1;
            for (int y = 0; y < frameHeight_ && changeStartY_ == -1; ++y) {
                for (int x = 0; x < width_; ++x) {
                    const auto offset = (static_cast<std::size_t>(y) * width_ + x) * 4;
                    if (previous_[offset] != pixels[offset] || previous_[offset + 1] != pixels[offset + 1] ||
                        previous_[offset + 2] != pixels[offset + 2]) {
                        changeStartY_ = y;
                        break;
                    }
                }
            }
            if (changeStartY_ == -1) return dismissed(ScrollStopReason::ReachedBottom);
            firstCheck_ = false;
        }
        const int stripHeight = std::min(ScrollCaptureRules::comparisonRows, frameHeight_ - changeStartY_);
        if (stripHeight <= 0) return {ScrollNextStep::Scroll, ScrollStopReason::None, false};
        if (!Core::toGrayscale(pixels, width_, frameHeight_, currentGray_)) {
            stop(ScrollStopReason::InvalidFrame);
            return stopped();
        }
        const auto grayOffset = static_cast<std::size_t>(changeStartY_) * width_;
        const auto previous = std::span<const std::uint8_t>(previousGray_).subspan(grayOffset);
        const auto current = std::span<const std::uint8_t>(currentGray_).subspan(grayOffset);
        int rows = Core::findMostSimilarY(previous, frameHeight_ - changeStartY_, current, stripHeight, width_);
        if (rows == 0) {
            const auto bottom = static_cast<std::size_t>(frameHeight_ - stripHeight) * width_;
            rows = Core::findScrollByBottomStrip(std::span<const std::uint8_t>(previousGray_).subspan(bottom),
                std::span<const std::uint8_t>(currentGray_).subspan(bottom), width_, stripHeight);
        }
        if (rows == 0) {
            if (!std::equal(pixels.begin(), pixels.end(), previous_.begin(), previous_.end()) &&
                settleRechecks_ < ScrollCaptureRules::maximumSettleRechecks) {
                ++settleRechecks_;
                return {ScrollNextStep::Settle, ScrollStopReason::None, false};
            }
            settleRechecks_ = 0;
            return dismissed(ScrollStopReason::ReachedBottom);
        }
        if (!reserveAppend(rows)) {
            stop(ScrollStopReason::MemoryLimit);
            return stopped();
        }
        if (!Core::appendScrollFrame(result_, width_, resultHeight_, pixels, frameHeight_, changeStartY_, rows)) {
            stop(ScrollStopReason::InvalidFrame);
            return stopped();
        }
        dismissals_ = settleRechecks_ = 0;
        // The previous frame was allocated at begin(); this copy cannot cause a
        // new allocation after the complete result has already been appended.
        std::copy(pixels.begin(), pixels.end(), previous_.begin());
        previousGray_.swap(currentGray_);
        if (resultHeight_ > limits_.stopAfterHeight) {
            stop(ScrollStopReason::HeightLimit);
            return {ScrollNextStep::Stopped, reason_, true};
        }
        return {ScrollNextStep::Scroll, ScrollStopReason::None, true};
    } catch (...) {
        stop(ScrollStopReason::MemoryLimit);
        return stopped();
    }
}

} // namespace HuhuCapture::Capture
