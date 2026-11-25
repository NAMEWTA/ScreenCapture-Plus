#include "recording/FrameSource.h"
#include "recording/FramePixels.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

namespace HuhuCapture::Recording {
namespace {
class PollingFrameSource final : public FrameSource {
public:
    PollingFrameSource(BackendRequest request, CaptureSource kind, BgraCapture capture)
        : request_(request), kind_(kind), capture_(std::move(capture)),
          pool_({2, static_cast<std::size_t>(request.maximumFrameBytes * 2), 16384})
    {
        format_ = Core::packedFrameFormat(Core::FramePixelFormat::Bgra8,
            request.region.width, request.region.height,
            {Core::ColorPrimaries::Bt709, Core::ColorTransfer::Srgb,
             Core::ColorMatrix::Rgb, Core::ColorRange::Full});
        if (format_) initialized_ = pool_.configure(*format_) == Core::FramePoolStatus::Ready;
    }
    CaptureSource kind() const noexcept override { return kind_; }
    FrameRead read(Core::MediaClock::time_point, std::stop_token stop) override
    {
        if (stop.stop_requested()) return {FrameReadStatus::Stopped, {}, 0, {}};
        if (!initialized_) return {FrameReadStatus::Failed, {}, ENOMEM, "Cannot allocate the recording frame pool"};
        auto target = pool_.tryAcquire();
        if (!target) return {FrameReadStatus::Failed, {}, ENOBUFS, "The recording consumer retained too many frames"};
        const auto source = capture_(request_.region, request_.cursor);
        if (source.error || source.pixels.empty())
            return {FrameReadStatus::Failed, {}, source.error ? source.error : EIO,
                source.detail.empty() ? "The desktop frame source stopped producing pixels" : source.detail};
        const auto rowBytes = static_cast<std::size_t>(request_.region.width) * 4;
        const auto rows = static_cast<std::size_t>(request_.region.height);
        if (source.stride < rowBytes || (rows > 1 && source.stride >
                ((std::numeric_limits<std::size_t>::max)() - rowBytes) / (rows - 1)) ||
            source.pixels.size() < (rows - 1) * source.stride + rowBytes)
            return {FrameReadStatus::Failed, {}, EINVAL, "The desktop returned an incomplete pixel buffer"};
        for (std::uint32_t row = 0; row < static_cast<std::uint32_t>(rows); ++row)
            std::memcpy(target->row(0, row).data(), source.pixels.data() + row * source.stride, rowBytes);
        Core::FrameMetadata metadata;
        metadata.sessionId = 1; metadata.sourceId = 1;
        metadata.timing.receivedAt = Core::MediaClock::now();
        metadata.timing.sourceSequence = sequence_++;
        auto frame = std::move(*target).publish(metadata);
        if (!frame) return {FrameReadStatus::Failed, {}, EINVAL, "The desktop frame metadata is invalid"};
        return {FrameReadStatus::Frame, std::move(frame), 0, {}};
    }
private:
    BackendRequest request_;
    CaptureSource kind_;
    BgraCapture capture_;
    Core::FramePool pool_;
    std::optional<Core::FrameFormat> format_;
    std::uint64_t sequence_{};
    bool initialized_{};
};

class CroppedFrameSource final : public FrameSource {
public:
    CroppedFrameSource(std::unique_ptr<FrameSource> source, PixelRegion selection,
        FrameSourceContract expected, std::size_t maximumBytes)
        : source_(std::move(source)), selection_(selection), expected_(std::move(expected)),
          pool_({2, maximumBytes, 16384}) {}
    CaptureSource kind() const noexcept override { return source_->kind(); }
    void stopAndWait() noexcept override { source_->stopAndWait(); }
    FrameRead read(Core::MediaClock::time_point deadline, std::stop_token stop) override
    {
        auto input = source_->read(deadline, stop);
        if (input.status != FrameReadStatus::Frame) return input;
        const auto size = normalizedFrameSize(input.frame);
        if (!size || *size != expected_.normalizedSize || !expected_.matches(input.frame))
            return {FrameReadStatus::Failed, {}, EINVAL, "The authorized screen source changed; select the recording region again"};
        const auto format = Core::packedFrameFormat(Core::FramePixelFormat::Bgra8,
            selection_.width, selection_.height, input.frame.format().color);
        if (!format) return {FrameReadStatus::Failed, {}, EINVAL, "Invalid selected recording region"};
        if (!format_ || *format != *format_) {
            if (pool_.configure(*format) != Core::FramePoolStatus::Ready)
                return {FrameReadStatus::Failed, {}, ENOMEM, "Cannot allocate the selected recording frame"};
            format_ = *format;
        }
        auto output = pool_.tryAcquire();
        if (!output) return {FrameReadStatus::Failed, {}, ENOBUFS, "The recording consumer retained too many selected frames"};
        if (!copyFrameBgra(input.frame, selection_, output->plane(0), static_cast<std::size_t>(format->planes[0].stride)))
            return {FrameReadStatus::Failed, {}, EINVAL, "The authorized frame crop, color or transform is unsupported"};
        auto metadata = input.frame.metadata(); metadata.crop.reset(); metadata.transform = Core::FrameTransform::None;
        return {FrameReadStatus::Frame, std::move(*output).publish(metadata), 0, {}};
    }
private:
    std::unique_ptr<FrameSource> source_;
    PixelRegion selection_;
    FrameSourceContract expected_;
    Core::FramePool pool_;
    std::optional<Core::FrameFormat> format_;
};
}

std::unique_ptr<FrameSource> makePollingFrameSource(const BackendRequest& request,
    CaptureSource kind, BgraCapture capture)
{
    if (!capture || request.maximumFrameBytes == 0 || request.maximumFrameBytes > 512ull * 1024 * 1024 ||
        request.maximumFrameBytes > (std::numeric_limits<std::size_t>::max)() / 2 ||
        request.region.width <= 0 || request.region.height <= 0) return {};
    const auto layout = Core::pixelLayout(request.region.width, request.region.height, 4);
    if (!layout || layout->bytes > request.maximumFrameBytes) return {};
    return std::make_unique<PollingFrameSource>(request, kind, std::move(capture));
}

std::unique_ptr<FrameSource> makeCroppedFrameSource(std::unique_ptr<FrameSource> source,
    PixelRegion selection, FrameSourceContract expected, std::size_t maximumBytes)
{
    if (!source || maximumBytes == 0 || maximumBytes > 1024ull * 1024 * 1024 ||
        !expected.generation || !Core::validFrameFormat(expected.format) ||
        !Core::validFrameMetadata(expected.preview, expected.format) ||
        selection.x < 0 || selection.y < 0 || selection.width <= 0 || selection.height <= 0 ||
        std::int64_t{selection.x} + selection.width > expected.normalizedSize.first ||
        std::int64_t{selection.y} + selection.height > expected.normalizedSize.second) return {};
    return std::make_unique<CroppedFrameSource>(std::move(source), selection, std::move(expected), maximumBytes);
}

} // namespace HuhuCapture::Recording
