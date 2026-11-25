#pragma once

#include "core/MediaClock.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace HuhuCapture::Core {

// CPU formats implemented by the shared layout rules. Native GPU allocations
// remain backend-owned leases; they must not be passed as a linear CPU plane.
enum class FramePixelFormat { Bgra8, Rgba8, Nv12, I420 };
enum class FrameAlpha { Opaque, Straight, Premultiplied };
enum class ColorPrimaries {
    Unspecified, Bt709, Bt470M, Bt470Bg, Smpte170M, Smpte240M, Film,
    Bt2020, AdobeRgb, Smpte428, DciP3, DisplayP3, Ebu3213
};
enum class ColorTransfer {
    Unspecified, Linear, Gamma18, Gamma20, Gamma22, Bt709, Smpte240M,
    Srgb, Gamma28, Log100, Log316, Bt2020_12, AdobeRgb, Bt2020_10,
    Pq, Hlg, Bt601
};
enum class ColorMatrix { Unspecified, Rgb, Fcc, Bt709, Bt601, Smpte240M, Bt2020 };
enum class ColorRange { Unspecified, Full, Limited };

struct FrameColor {
    ColorPrimaries primaries{ColorPrimaries::Unspecified};
    ColorTransfer transfer{ColorTransfer::Unspecified};
    ColorMatrix matrix{ColorMatrix::Unspecified};
    ColorRange range{ColorRange::Unspecified};
    bool operator==(const FrameColor&) const = default;
};

// [offset, offset + size) is this plane's valid storage in the allocation.
// firstRow is relative to that interval. stride advances between logical rows;
// a negative value describes bottom-up storage without rebasing outside it.
// rowBytes excludes padding. Every addressed byte must remain in the interval.
struct FramePlaneLayout {
    std::size_t offset{};
    std::size_t size{};
    std::size_t firstRow{};
    std::ptrdiff_t stride{};
    std::size_t rowBytes{};
    std::uint32_t rows{};
    bool operator==(const FramePlaneLayout&) const = default;
};

struct FrameFormat {
    FramePixelFormat pixelFormat{FramePixelFormat::Bgra8};
    int width{};
    int height{};
    FrameAlpha alpha{FrameAlpha::Opaque};
    FrameColor color;
    std::size_t bytes{};
    std::uint32_t planeCount{};
    std::array<FramePlaneLayout, 4> planes{};
    bool operator==(const FrameFormat&) const = default;
};

bool validFramePlane(const FramePlaneLayout& plane, std::size_t allocationBytes) noexcept;
bool validFrameFormat(const FrameFormat& format) noexcept;
std::optional<FrameFormat> packedFrameFormat(FramePixelFormat pixelFormat,
    int width, int height, FrameColor color = {}, FrameAlpha alpha = FrameAlpha::Opaque) noexcept;

struct FrameCrop {
    std::uint32_t x{}, y{}, width{}, height{};
    bool operator==(const FrameCrop&) const = default;
};

// Counter-clockwise rotation; reflected variants reflect through the vertical
// center line first. These describe the transform applied to the source buffer,
// not a transform that has already been undone by the consumer.
enum class FrameTransform {
    None, Rotate90, Rotate180, Rotate270,
    Reflected, Reflected90, Reflected180, Reflected270
};

enum class FrameClockDomain { Unknown, PipeWire };
struct FrameTiming {
    std::optional<std::chrono::nanoseconds> sourcePts;
    FrameClockDomain sourceClock{FrameClockDomain::Unknown};
    // A service-assigned clock instance separates independent capture sessions.
    std::uint64_t clockInstance{};
    std::optional<std::uint32_t> graphClockId;
    std::optional<std::uint64_t> sourceSequence;
    MediaClock::time_point receivedAt{};
    bool discontinuity{};
};

struct FrameMetadata {
    // Service identities, not an OS node ID or a display index.
    std::uint64_t sessionId{};
    std::uint64_t sourceId{};
    FrameTiming timing;
    // No crop means the complete negotiated pixel rectangle.
    std::optional<FrameCrop> crop;
    FrameTransform transform{FrameTransform::None};
};

bool validFrameMetadata(const FrameMetadata& metadata, const FrameFormat& format) noexcept;

namespace Detail {
struct FramePoolBudget;
struct FramePoolStorage;
}

class WritableFrame;
class FramePool;

// Move-only ownership keeps a pool slot alive until the asynchronous consumer
// releases it. The bytes are immutable after publish(). Releasing a lease is
// safe on another thread and never calls a platform API or a destroyed pool.
class FrameLease {
public:
    FrameLease() noexcept = default;
    ~FrameLease();
    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    explicit operator bool() const noexcept;
    const FrameFormat& format() const noexcept;
    const FrameMetadata& metadata() const noexcept { return metadata_; }
    std::uint64_t formatGeneration() const noexcept;
    std::span<const std::uint8_t> plane(std::size_t index) const noexcept;
    std::span<const std::uint8_t> row(std::size_t planeIndex, std::uint32_t rowIndex) const noexcept;
    void reset() noexcept;

private:
    friend class FramePool;
    friend class WritableFrame;
    FrameLease(std::shared_ptr<Detail::FramePoolStorage> storage, std::size_t slot) noexcept;
    std::shared_ptr<Detail::FramePoolStorage> storage_;
    std::size_t slot_{};
    FrameMetadata metadata_;
};

// Exclusive producer view. publish() consumes this view. The producer must also
// discard any raw writable spans obtained before publishing.
class WritableFrame {
public:
    WritableFrame() noexcept = default;
    WritableFrame(WritableFrame&&) noexcept = default;
    WritableFrame& operator=(WritableFrame&&) noexcept = default;
    WritableFrame(const WritableFrame&) = delete;
    WritableFrame& operator=(const WritableFrame&) = delete;

    explicit operator bool() const noexcept { return static_cast<bool>(lease_); }
    const FrameFormat& format() const noexcept { return lease_.format(); }
    std::span<std::uint8_t> plane(std::size_t index) noexcept;
    std::span<std::uint8_t> row(std::size_t planeIndex, std::uint32_t rowIndex) noexcept;
    // Invalid crop/transform/clock metadata returns an empty lease and releases
    // the slot; no partially described frame becomes visible to a consumer.
    FrameLease publish(FrameMetadata metadata) && noexcept;

private:
    friend class FramePool;
    explicit WritableFrame(FrameLease lease) noexcept : lease_(std::move(lease)) {}
    FrameLease lease_;
};

struct FramePoolLimits {
    std::size_t slotCount{};
    std::size_t maximumBytes{};
    int maximumDimension{};
};
enum class FramePoolStatus { Ready, InvalidFormat, BudgetExceeded, AllocationFailed, GenerationExhausted };

// One producer owns configure()/tryAcquire()/clear(). Consumers only own leases.
// Pixel allocations happen on configure(), never for an individual frame. A new
// format invalidates the producer's old pool; outstanding old-format leases keep
// their bytes, and count against the same budget until they are released.
class FramePool {
public:
    explicit FramePool(FramePoolLimits limits);
    ~FramePool();
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    FramePool(FramePool&&) = delete;
    FramePool& operator=(FramePool&&) = delete;

    FramePoolStatus configure(const FrameFormat& format) noexcept;
    std::optional<WritableFrame> tryAcquire() noexcept;
    void clear() noexcept;
    std::size_t allocatedBytes() const noexcept;
    std::uint64_t formatGeneration() const noexcept;

private:
    FramePoolLimits limits_;
    std::shared_ptr<Detail::FramePoolBudget> budget_;
    std::shared_ptr<Detail::FramePoolStorage> storage_;
    std::uint64_t generation_{};
};

} // namespace HuhuCapture::Core
