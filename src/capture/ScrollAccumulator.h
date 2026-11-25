#pragma once

#include "core/ScrollStitch.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace HuhuCapture::Capture {

enum class ScrollAppend { Appended, RetryScroll, WaitForSettling, ReachedBottom, Limit, Failed };

// Extracted from the shipping CapLong sequence, retaining the 100-row strip,
// bottom-strip fallback, two settling rechecks and eight no-motion retries.
// Only incoming-frame tails are appended; accumulated pixels are never copied
// for matching, and every failed append retains the last complete image.
class ScrollAccumulator final {
public:
    struct Limits { std::size_t maximumBytes{512u * 1024u * 1024u}; int maximumHeight{36000}; };
    bool start(std::span<const std::uint8_t> bgra, int width, int height, Limits limits)
    {
        const auto bytes = Core::pixelLayout(width, height, 4);
        if (!bytes || bgra.size() != bytes->bytes || bytes->bytes > limits.maximumBytes || limits.maximumHeight <= 0)
            return fail("The initial scrolling image is outside its pixel budget");
        try {
            std::vector<std::uint8_t> image(bgra.begin(), bgra.end()), previous = image, gray;
            if (!Core::toGrayscale(image, width, height, gray)) return fail("The initial scrolling image is invalid");
            image_.swap(image); previous_.swap(previous); previousGray_.swap(gray); currentGray_.clear();
        } catch (...) { return fail("The initial scrolling image could not be retained"); }
        width_ = width; frameHeight_ = resultHeight_ = height; limits_ = limits;
        firstChange_ = true; changeStart_ = misses_ = rechecks_ = 0; error_.clear(); return true;
    }
    ScrollAppend append(std::span<const std::uint8_t> frame, bool automatic = true)
    {
        if (image_.empty() || frame.size() != previous_.size()) { fail("The scrolling source changed dimensions"); return ScrollAppend::Failed; }
        if (firstChange_) {
            changeStart_ = -1;
            for (int row = 0; row < frameHeight_ && changeStart_ == -1; ++row) {
                for (int column = 0; column < width_; ++column) {
                    const auto offset = (static_cast<std::size_t>(row) * width_ + column) * 4;
                    if (previous_[offset] != frame[offset] || previous_[offset + 1] != frame[offset + 1] || previous_[offset + 2] != frame[offset + 2]) {
                        changeStart_ = row; break;
                    }
                }
            }
            if (changeStart_ == -1) return miss(automatic);
            firstChange_ = false;
        }
        const int stripHeight = std::min(100, frameHeight_ - changeStart_);
        if (stripHeight <= 0) return miss(automatic);
        try {
            if (!Core::toGrayscale(frame, width_, frameHeight_, currentGray_)) { fail("The scrolling frame is incomplete"); return ScrollAppend::Failed; }
            const auto offset = static_cast<std::size_t>(changeStart_) * width_;
            int distance = Core::findMostSimilarY(std::span(previousGray_).subspan(offset), frameHeight_ - changeStart_,
                std::span(currentGray_).subspan(offset), stripHeight, width_);
            if (!distance) {
                const auto bottom = static_cast<std::size_t>(frameHeight_ - stripHeight) * width_;
                distance = Core::findScrollByBottomStrip(std::span(previousGray_).subspan(bottom),
                    std::span(currentGray_).subspan(bottom), width_, stripHeight);
            }
            if (!distance) {
                if (!std::equal(frame.begin(), frame.end(), previous_.begin(), previous_.end()) && rechecks_ < 2) {
                    ++rechecks_; return ScrollAppend::WaitForSettling;
                }
                rechecks_ = 0; return miss(automatic);
            }
            if (distance > (std::numeric_limits<int>::max)() - resultHeight_) return ScrollAppend::Limit;
            const auto newBytes = Core::pixelLayout(width_, resultHeight_ + distance, 4);
            if (!newBytes || newBytes->bytes > limits_.maximumBytes) return ScrollAppend::Limit;
            if (!Core::appendScrollFrame(image_, width_, resultHeight_, frame, frameHeight_, changeStart_, distance)) {
                fail("The scrolling frame could not be joined to the complete image"); return ScrollAppend::Failed;
            }
            previous_.assign(frame.begin(), frame.end()); previousGray_.swap(currentGray_);
            misses_ = rechecks_ = 0;
            return resultHeight_ > limits_.maximumHeight ? ScrollAppend::Limit : ScrollAppend::Appended;
        } catch (...) { fail("The scrolling image reached the available memory limit"); return ScrollAppend::Limit; }
    }
    int width() const noexcept { return width_; }
    int height() const noexcept { return resultHeight_; }
    std::span<const std::uint8_t> pixels() const noexcept { return image_; }
    std::vector<std::uint8_t> takePixels() noexcept { return std::move(image_); }
    const std::string& error() const noexcept { return error_; }
private:
    bool fail(std::string detail) { error_ = std::move(detail); return false; }
    ScrollAppend miss(bool automatic)
    {
        // Manual Wayland scrolling may legitimately remain still while the user
        // reads or moves to the target. Only an automatic wheel attempt can
        // establish the legacy reached-bottom retry threshold.
        return automatic && ++misses_ > 8 ? ScrollAppend::ReachedBottom : ScrollAppend::RetryScroll;
    }
    Limits limits_;
    int width_{}, frameHeight_{}, resultHeight_{}, changeStart_{}, misses_{}, rechecks_{};
    bool firstChange_{true};
    std::string error_;
    std::vector<std::uint8_t> image_, previous_, previousGray_, currentGray_;
};

} // namespace HuhuCapture::Capture
