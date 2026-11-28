#include "core/MediaFrame.h"
#include "core/PixelBuffer.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <new>
#include <utility>

namespace HuhuCapture::Core {
namespace {

std::size_t strideMagnitude(std::ptrdiff_t stride) noexcept
{
    return stride < 0 ? static_cast<std::size_t>(-(stride + 1)) + 1
                      : static_cast<std::size_t>(stride);
}

std::size_t rowOffset(const FramePlaneLayout& plane, std::uint32_t row) noexcept
{
    const auto distance = strideMagnitude(plane.stride) * row;
    return plane.stride < 0 ? plane.firstRow - distance : plane.firstRow + distance;
}

} // namespace

bool validFramePlane(const FramePlaneLayout& plane, std::size_t allocationBytes) noexcept
{
    if (plane.rows == 0 || plane.rowBytes == 0 || plane.size == 0 ||
        plane.offset > allocationBytes || plane.size > allocationBytes - plane.offset ||
        plane.firstRow > plane.size || plane.rowBytes > plane.size - plane.firstRow)
        return false;
    const auto stride = strideMagnitude(plane.stride);
    if (plane.rows > 1 && (stride < plane.rowBytes ||
        static_cast<std::size_t>(plane.rows - 1) > (std::numeric_limits<std::size_t>::max)() / stride))
        return false;
    const auto distance = stride * static_cast<std::size_t>(plane.rows - 1);
    if (plane.stride < 0) return distance <= plane.firstRow;
    return distance <= plane.size - plane.firstRow - plane.rowBytes;
}

std::optional<FrameFormat> packedFrameFormat(FramePixelFormat pixelFormat,
    int width, int height, FrameColor color, FrameAlpha alpha) noexcept
{
    if (width <= 0 || height <= 0) return std::nullopt;
    FrameFormat format;
    format.pixelFormat = pixelFormat;
    format.width = width;
    format.height = height;
    format.alpha = alpha;
    format.color = color;
    const auto append = [&](int w, int h, std::size_t bytesPerPixel) {
        const auto layout = pixelLayout(w, h, bytesPerPixel);
        if (!layout || layout->stride > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) ||
            layout->bytes > (std::numeric_limits<std::size_t>::max)() - format.bytes)
            return false;
        format.planes[format.planeCount++] = {format.bytes, layout->bytes, 0,
            static_cast<std::ptrdiff_t>(layout->stride), layout->stride, static_cast<std::uint32_t>(h)};
        format.bytes += layout->bytes;
        return true;
    };
    switch (pixelFormat) {
    case FramePixelFormat::Bgra8:
    case FramePixelFormat::Rgba8:
        if (!append(width, height, 4)) return std::nullopt;
        break;
    case FramePixelFormat::Nv12:
        if (alpha != FrameAlpha::Opaque || !append(width, height, 1) ||
            !append(width / 2 + width % 2, height / 2 + height % 2, 2))
            return std::nullopt;
        break;
    case FramePixelFormat::I420:
        if (alpha != FrameAlpha::Opaque || !append(width, height, 1) ||
            !append(width / 2 + width % 2, height / 2 + height % 2, 1) ||
            !append(width / 2 + width % 2, height / 2 + height % 2, 1))
            return std::nullopt;
        break;
    default:
        return std::nullopt;
    }
    return format;
}

bool validFrameFormat(const FrameFormat& format) noexcept
{
    const auto packed = packedFrameFormat(format.pixelFormat, format.width, format.height,
                                          format.color, format.alpha);
    if (!packed || format.bytes == 0 || format.planeCount != packed->planeCount ||
        (format.alpha != FrameAlpha::Opaque && format.alpha != FrameAlpha::Straight &&
         format.alpha != FrameAlpha::Premultiplied))
        return false;
    for (std::uint32_t i = 0; i < format.planeCount; ++i) {
        const auto& plane = format.planes[i];
        if (!validFramePlane(plane, format.bytes) ||
            plane.rowBytes != packed->planes[i].rowBytes || plane.rows != packed->planes[i].rows)
            return false;
        for (std::uint32_t j = 0; j < i; ++j) {
            const auto& previous = format.planes[j];
            if (plane.offset < previous.offset + previous.size &&
                previous.offset < plane.offset + plane.size)
                return false;
        }
    }
    return true;
}

bool validFrameMetadata(const FrameMetadata& metadata, const FrameFormat& format) noexcept
{
    if (static_cast<unsigned>(metadata.transform) > static_cast<unsigned>(FrameTransform::Reflected270))
        return false;
    if (metadata.timing.sourceClock != FrameClockDomain::Unknown &&
        metadata.timing.sourceClock != FrameClockDomain::PipeWire)
        return false;
    if (metadata.timing.sourcePts &&
        (metadata.timing.sourceClock == FrameClockDomain::Unknown || metadata.timing.clockInstance == 0))
        return false;
    if (metadata.timing.graphClockId && metadata.timing.sourceClock != FrameClockDomain::PipeWire)
        return false;
    if (metadata.crop) {
        const auto& crop = *metadata.crop;
        if (format.width <= 0 || format.height <= 0 || crop.width == 0 || crop.height == 0 ||
            crop.x > static_cast<std::uint32_t>(format.width) ||
            crop.width > static_cast<std::uint32_t>(format.width) - crop.x ||
            crop.y > static_cast<std::uint32_t>(format.height) ||
            crop.height > static_cast<std::uint32_t>(format.height) - crop.y)
            return false;
    }
    return true;
}

namespace Detail {

struct FramePoolBudget {
    explicit FramePoolBudget(std::size_t bytes) : limit(bytes) {}
    const std::size_t limit;
    std::atomic_size_t used{};

    bool reserve(std::size_t bytes) noexcept
    {
        auto current = used.load(std::memory_order_relaxed);
        do {
            if (current > limit || bytes > limit - current) return false;
        } while (!used.compare_exchange_weak(current, current + bytes,
                      std::memory_order_acq_rel, std::memory_order_relaxed));
        return true;
    }
};

struct FramePoolStorage {
    struct Slot {
        std::unique_ptr<std::uint8_t[]> bytes;
        std::atomic_bool acquired{};
    };
    FramePoolStorage(std::shared_ptr<FramePoolBudget> owner, FrameFormat description,
        std::size_t count, std::uint64_t serial)
        : budget(std::move(owner)), format(std::move(description)), slots(new Slot[count]),
          slotCount(count), generation(serial)
    {
        for (std::size_t i = 0; i < slotCount; ++i)
            slots[i].bytes = std::make_unique<std::uint8_t[]>(format.bytes);
    }
    ~FramePoolStorage()
    {
        // Release the physical allocations before making their budget reusable.
        slots.reset();
        if (accounted) budget->used.fetch_sub(format.bytes * slotCount, std::memory_order_acq_rel);
    }
    std::shared_ptr<FramePoolBudget> budget;
    const FrameFormat format;
    std::unique_ptr<Slot[]> slots;
    const std::size_t slotCount;
    const std::uint64_t generation;
    bool accounted{};
};

} // namespace Detail

FrameLease::FrameLease(std::shared_ptr<Detail::FramePoolStorage> storage, std::size_t slot) noexcept
    : storage_(std::move(storage)), slot_(slot)
{
}

FrameLease::~FrameLease() { reset(); }

FrameLease::FrameLease(FrameLease&& other) noexcept
    : storage_(std::move(other.storage_)), slot_(other.slot_), metadata_(std::move(other.metadata_))
{
}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept
{
    if (this != &other) {
        reset();
        storage_ = std::move(other.storage_);
        slot_ = other.slot_;
        metadata_ = std::move(other.metadata_);
    }
    return *this;
}

FrameLease::operator bool() const noexcept { return storage_ != nullptr; }

const FrameFormat& FrameLease::format() const noexcept
{
    assert(storage_);
    return storage_->format;
}

std::uint64_t FrameLease::formatGeneration() const noexcept
{
    return storage_ ? storage_->generation : 0;
}

std::span<const std::uint8_t> FrameLease::plane(std::size_t index) const noexcept
{
    if (!storage_ || index >= storage_->format.planeCount) return {};
    const auto& layout = storage_->format.planes[index];
    return {storage_->slots[slot_].bytes.get() + layout.offset, layout.size};
}

std::span<const std::uint8_t> FrameLease::row(std::size_t planeIndex, std::uint32_t rowIndex) const noexcept
{
    if (!storage_ || planeIndex >= storage_->format.planeCount) return {};
    const auto& layout = storage_->format.planes[planeIndex];
    if (rowIndex >= layout.rows) return {};
    return plane(planeIndex).subspan(rowOffset(layout, rowIndex), layout.rowBytes);
}

void FrameLease::reset() noexcept
{
    if (storage_) {
        storage_->slots[slot_].acquired.store(false, std::memory_order_release);
        storage_.reset();
    }
}

std::span<std::uint8_t> WritableFrame::plane(std::size_t index) noexcept
{
    const auto view = lease_.plane(index);
    return {const_cast<std::uint8_t*>(view.data()), view.size()};
}

std::span<std::uint8_t> WritableFrame::row(std::size_t planeIndex, std::uint32_t rowIndex) noexcept
{
    const auto view = lease_.row(planeIndex, rowIndex);
    return {const_cast<std::uint8_t*>(view.data()), view.size()};
}

FrameLease WritableFrame::publish(FrameMetadata metadata) && noexcept
{
    if (!lease_ || !validFrameMetadata(metadata, lease_.format())) {
        lease_.reset();
        return {};
    }
    lease_.metadata_ = std::move(metadata);
    return std::move(lease_);
}

FramePool::FramePool(FramePoolLimits limits)
    : limits_(limits), budget_(std::make_shared<Detail::FramePoolBudget>(limits.maximumBytes))
{
}

FramePool::~FramePool() = default;

FramePoolStatus FramePool::configure(const FrameFormat& format) noexcept
{
    if (!validFrameFormat(format) || limits_.slotCount == 0 || limits_.maximumDimension <= 0 ||
        format.width > limits_.maximumDimension || format.height > limits_.maximumDimension)
        return FramePoolStatus::InvalidFormat;
    if (format.bytes > limits_.maximumBytes / limits_.slotCount)
        return FramePoolStatus::BudgetExceeded;
    if (generation_ == (std::numeric_limits<std::uint64_t>::max)())
        return FramePoolStatus::GenerationExhausted;
    const auto requested = format.bytes * limits_.slotCount;
    storage_.reset();
    if (!budget_->reserve(requested)) return FramePoolStatus::BudgetExceeded;
    try {
        auto next = std::make_shared<Detail::FramePoolStorage>(budget_, format,
                                                              limits_.slotCount, generation_ + 1);
        next->accounted = true;
        storage_ = std::move(next);
        ++generation_;
    } catch (const std::bad_alloc&) {
        budget_->used.fetch_sub(requested, std::memory_order_acq_rel);
        return FramePoolStatus::AllocationFailed;
    }
    return FramePoolStatus::Ready;
}

std::optional<WritableFrame> FramePool::tryAcquire() noexcept
{
    if (!storage_) return std::nullopt;
    for (std::size_t i = 0; i < storage_->slotCount; ++i) {
        bool expected = false;
        if (storage_->slots[i].acquired.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed))
            return WritableFrame(FrameLease(storage_, i));
    }
    return std::nullopt;
}

void FramePool::clear() noexcept { storage_.reset(); }

std::size_t FramePool::allocatedBytes() const noexcept
{
    return budget_->used.load(std::memory_order_acquire);
}

std::uint64_t FramePool::formatGeneration() const noexcept
{
    return storage_ ? storage_->generation : 0;
}

} // namespace HuhuCapture::Core
