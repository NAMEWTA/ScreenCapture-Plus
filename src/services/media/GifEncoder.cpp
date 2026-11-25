#include "services/media/GifEncoder.h"
#include "core/PixelBuffer.h"
#include "core/GifLimits.h"
#include "platform/FileOutput.h"
#include "gifski.h"

#include <atomic>
#include <cerrno>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace HuhuCapture::Media {
namespace {
using Core::RecordingStage;

bool opaqueSdr(const Core::FrameFormat& format) noexcept
{
    using namespace Core;
    return format.alpha == FrameAlpha::Opaque &&
        (format.color.primaries == ColorPrimaries::Unspecified || format.color.primaries == ColorPrimaries::Bt709) &&
        (format.color.transfer == ColorTransfer::Unspecified || format.color.transfer == ColorTransfer::Srgb) &&
        (format.color.matrix == ColorMatrix::Unspecified || format.color.matrix == ColorMatrix::Rgb) &&
        (format.color.range == ColorRange::Unspecified || format.color.range == ColorRange::Full);
}

}

struct GifEncoder::State {
    GifEncoderOptions options;
    Platform::FileHandle file;
    Core::RecordingResult result;
    std::vector<std::uint8_t> rgb;
    std::size_t rgbStride{};
    gifski* encoder{};
    Core::MediaTime lastPresentationTime{};
    bool finished{};
    // Only the Rust writer touches bytesWritten. The worker reads it after
    // gifski_finish has joined that writer. I/O errors may also be observed
    // after add() returns, so their first value is transferred atomically.
    std::uint64_t bytesWritten{};
    std::atomic<int> firstIoError{};

    State(GifEncoderOptions settings, Platform::FileHandle writer)
        : options(settings), file(std::move(writer))
    {
        const auto source = Core::pixelLayout(options.width, options.height, 4);
        const auto destination = Core::pixelLayout(options.width, options.height, 3);
        if (!file || !source || !destination || options.width > 65535 || options.height > 65535 ||
            source->bytes > options.maximumFrameBytes || !options.maximumOutputBytes ||
            destination->stride > (std::numeric_limits<std::uint32_t>::max)()) {
            result.fail(RecordingStage::Prepare, GIFSKI_INVALID_INPUT); return;
        }
        std::error_code error;
        const auto opened = Platform::readFileDescriptorSnapshot(file.get(), error);
        if (!opened || opened->bytes) {
            result.fail(RecordingStage::Prepare, error ? error.value() : GIFSKI_INVALID_INPUT); return;
        }
        try {
            rgb.resize(destination->bytes);
            rgbStride = destination->stride;
        } catch (const std::bad_alloc&) {
            result.fail(RecordingStage::Prepare, static_cast<int>(std::errc::not_enough_memory));
        } catch (...) {
            result.fail(RecordingStage::Prepare, GIFSKI_OTHER);
        }
    }

    int failWrite(int code) noexcept
    {
        int empty{};
        firstIoError.compare_exchange_strong(empty, code ? code : EIO);
        return GIFSKI_OTHER;
    }

    static int writeBytes(std::size_t count, const std::uint8_t* bytes, void* context) noexcept
    {
        auto& self = *static_cast<State*>(context);
        if (self.firstIoError.load()) return GIFSKI_OTHER;
        // CallbackWriter::flush passes a null pointer and zero bytes. Direct
        // descriptor writes have no user-space buffer; durable flush is checked
        // once after the codec joins, not for every GIF chunk.
        if (!count) return GIFSKI_OK;
        if (!bytes) return self.failWrite(EINVAL);
        if (self.bytesWritten > self.options.maximumOutputBytes ||
            count > self.options.maximumOutputBytes - self.bytesWritten) return self.failWrite(EFBIG);
        const auto error = Platform::writeFileDescriptor(self.file.get(), {bytes, count});
        if (error) return self.failWrite(error.value());
        self.bytesWritten += count;
        return GIFSKI_OK;
    }

    bool ready() const noexcept { return !finished && !result.firstError && static_cast<bool>(file); }
    bool reject() noexcept
    {
        if (!finished) result.fail(RecordingStage::Encode, GIFSKI_INVALID_INPUT);
        return false;
    }
    bool checkTime(Core::MediaTime time) noexcept
    {
        if (!ready()) return false;
        if (time.count() < 0 || (!result.framesWritten && time.count() != 0) ||
            (result.framesWritten && time <= lastPresentationTime) ||
            result.framesWritten >= Core::maximumValidatedGifFrames) return reject();
        if (const auto error = firstIoError.load()) {
            result.fail(RecordingStage::Encode, error); return false;
        }
        return true;
    }
    bool append(Core::MediaTime time) noexcept
    {
        if (!encoder) {
            const GifskiSettings settings{static_cast<std::uint32_t>(options.width),
                static_cast<std::uint32_t>(options.height), 80, true, 0};
            encoder = gifski_new(&settings);
            if (!encoder) { result.fail(RecordingStage::Prepare, GIFSKI_INVALID_INPUT); return false; }
            // Lazily start only after a complete first input was validated. A
            // stop before any input never starts a zero-frame writer which the
            // fixed upstream correctly finishes with INVALID_STATE.
            const auto status = gifski_set_write_callback(encoder, &writeBytes, this);
            if (status != GIFSKI_OK) { result.fail(RecordingStage::Prepare, status); return false; }
        }
        const auto status = gifski_add_frame_rgb(encoder, static_cast<std::uint32_t>(result.framesWritten),
            static_cast<std::uint32_t>(options.width), static_cast<std::uint32_t>(rgbStride),
            static_cast<std::uint32_t>(options.height), rgb.data(), std::chrono::duration<double>(time).count());
        if (status == GIFSKI_OK) {
            ++result.framesWritten;
            lastPresentationTime = time;
        }
        if (const auto error = firstIoError.load()) result.fail(RecordingStage::Encode, error);
        if (status != GIFSKI_OK) result.fail(RecordingStage::Encode, status);
        return !result.firstError;
    }
    Core::RecordingResult finish() noexcept
    {
        if (finished) return result;
        finished = true;
        const auto status = encoder ? gifski_finish(std::exchange(encoder, nullptr)) : GIFSKI_OK;
        // No callback can access file/bytesWritten/state after this point.
        if (const auto error = firstIoError.load()) result.fail(RecordingStage::Encode, error);
        result.finish(status == GIFSKI_OK, status);
        if (file) {
            const auto error = Platform::flushFileDescriptor(file.get());
            if (error) result.finish(false, error.value());
        }
        std::error_code error;
        if (!file.close(error)) result.finish(false, error.value());
        return result;
    }
};

GifEncoder::GifEncoder(GifEncoderOptions options, Platform::FileHandle writer)
    : state_(std::make_unique<State>(options, std::move(writer))) {}
GifEncoder::~GifEncoder() { state_->finish(); }
bool GifEncoder::ready() const noexcept { return state_->ready(); }
const Core::RecordingResult& GifEncoder::result() const noexcept { return state_->result; }

bool GifEncoder::addBgra(std::span<const std::uint8_t> pixels, std::size_t stride,
    Core::MediaTime time) noexcept
{
    auto& state = *state_;
    if (!state.checkTime(time)) return false;
    const auto rowBytes = static_cast<std::size_t>(state.options.width) * 4;
    if (pixels.size() > state.options.maximumFrameBytes ||
        stride > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)())) return state.reject();
    const Core::FramePlaneLayout plane{0, pixels.size(), 0, static_cast<std::ptrdiff_t>(stride),
        rowBytes, static_cast<std::uint32_t>(state.options.height)};
    if (!Core::validFramePlane(plane, pixels.size())) return state.reject();
    for (int y = 0; y < state.options.height; ++y) {
        const auto row = static_cast<std::size_t>(y);
        if (!Core::bgraToRgb(pixels.subspan(row * stride, rowBytes),
            std::span(state.rgb).subspan(row * state.rgbStride, state.rgbStride), state.options.width, 1))
            return state.reject();
    }
    return state.append(time);
}

bool GifEncoder::addFrame(const Core::FrameLease& frame, Core::MediaTime time) noexcept
{
    auto& state = *state_;
    if (!state.checkTime(time)) return false;
    if (!frame) return state.reject();
    const auto& format = frame.format();
    const auto& metadata = frame.metadata();
    if (!Core::validFrameFormat(format) || !Core::validFrameMetadata(metadata, format) ||
        format.width != state.options.width || format.height != state.options.height ||
        format.bytes > state.options.maximumFrameBytes || !opaqueSdr(format) ||
        (format.pixelFormat != Core::FramePixelFormat::Bgra8 && format.pixelFormat != Core::FramePixelFormat::Rgba8) ||
        metadata.transform != Core::FrameTransform::None ||
        (metadata.crop && *metadata.crop != Core::FrameCrop{0, 0, static_cast<std::uint32_t>(format.width),
            static_cast<std::uint32_t>(format.height)})) return state.reject();
    const auto rowBytes = static_cast<std::size_t>(format.width) * 4;
    for (int y = 0; y < format.height; ++y) {
        const auto source = frame.row(0, static_cast<std::uint32_t>(y));
        auto destination = std::span(state.rgb).subspan(static_cast<std::size_t>(y) * state.rgbStride, state.rgbStride);
        if (source.size() < rowBytes) return state.reject();
        if (format.pixelFormat == Core::FramePixelFormat::Bgra8) {
            if (!Core::bgraToRgb(source, destination, format.width, 1)) return state.reject();
        } else {
            // Opaque capture RGBA uses the same three color components. Alpha
            // is not a second transparency policy for screen recordings.
            for (std::size_t x = 0, d = 0; x < rowBytes; x += 4, d += 3) {
                destination[d] = source[x]; destination[d + 1] = source[x + 1]; destination[d + 2] = source[x + 2];
            }
        }
    }
    return state.append(time);
}

Core::RecordingResult GifEncoder::finish() noexcept { return state_->finish(); }

} // namespace HuhuCapture::Media
