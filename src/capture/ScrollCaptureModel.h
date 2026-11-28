#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace HuhuCapture::Capture {

// These values characterize the shipping CapLong loop. All platforms consume
// the same decisions; a timer or native input adapter does not own these rules.
struct ScrollCaptureRules {
    static constexpr int comparisonRows = 100;
    static constexpr int firstScrollDelayMs = 88;
    static constexpr int scrollSettleMs = 250;
    static constexpr int settleRecheckMs = 250;
    static constexpr int retryDelayMs = 500;
    static constexpr int maximumDismissals = 8; // stop on the ninth dismissal
    static constexpr int maximumSettleRechecks = 2;
};

struct ScrollCaptureLimits {
    // The original product stops after an accepted append exceeds this value.
    // It retains that complete last frame; this is not a truncation boundary.
    int stopAfterHeight{36000};
    // Accumulation capacity plus one previous BGRA frame and two gray frames.
    // Native readback and the independently bounded preview have their own owners.
    std::size_t maximumWorkingBytes{512u * 1024u * 1024u};
};

enum class ScrollStopReason {
    None, Requested, ReachedBottom, HeightLimit, MemoryLimit, CaptureFailed, InvalidFrame
};
enum class ScrollNextStep { Scroll, Settle, Stopped };
struct ScrollUpdate {
    ScrollNextStep next{ScrollNextStep::Stopped};
    ScrollStopReason reason{ScrollStopReason::None};
    bool appended{};
};
enum class ScrollStartError { None, InvalidFrame, MemoryLimit };

// Worker-confined, window-free production state. Pixels are tightly packed,
// top-down BGRA. Input is borrowed only during the call and cannot alias the
// accumulated result. A rejected allocation/append never destroys valid output.
class ScrollCaptureModel final {
public:
    explicit ScrollCaptureModel(ScrollCaptureLimits limits = {}) noexcept;
    ScrollCaptureModel(ScrollCaptureModel&&) noexcept = default;
    ScrollCaptureModel& operator=(ScrollCaptureModel&&) noexcept = default;
    ScrollCaptureModel(const ScrollCaptureModel&) = delete;
    ScrollCaptureModel& operator=(const ScrollCaptureModel&) = delete;
    ScrollStartError begin(std::span<const std::uint8_t> pixels, int width, int height) noexcept;
    // Manual streams may stay still while the user reads. Only automatic
    // wheel attempts establish the retained reached-bottom retry threshold.
    ScrollUpdate accept(std::span<const std::uint8_t> pixels, bool automatic = true) noexcept;
    ScrollUpdate captureFailed() noexcept;
    void stop(ScrollStopReason reason = ScrollStopReason::Requested) noexcept;

    bool hasImage() const noexcept { return !result_.empty(); }
    bool running() const noexcept { return hasImage() && reason_ == ScrollStopReason::None; }
    int width() const noexcept { return width_; }
    int frameHeight() const noexcept { return frameHeight_; }
    int height() const noexcept { return resultHeight_; }
    ScrollStopReason stopReason() const noexcept { return reason_; }
    const std::vector<std::uint8_t>& pixels() const noexcept { return result_; }
    // Terminal ownership transfer releases matcher storage and moves the
    // accumulated image without allocating or copying its complete pixels.
    std::vector<std::uint8_t> takePixels() noexcept
    { stop(); return std::move(result_); }
    std::size_t allocatedBytes() const noexcept;

private:
    ScrollUpdate dismissed(ScrollStopReason reason, bool automatic = true) noexcept;
    ScrollUpdate stopped() const noexcept;
    bool reserveAppend(int rows);

    ScrollCaptureLimits limits_;
    std::vector<std::uint8_t> result_, previous_, previousGray_, currentGray_;
    int width_{}, frameHeight_{}, resultHeight_{}, changeStartY_{-1};
    int dismissals_{}, settleRechecks_{};
    bool firstCheck_{true};
    ScrollStopReason reason_{ScrollStopReason::None};
};

} // namespace HuhuCapture::Capture
