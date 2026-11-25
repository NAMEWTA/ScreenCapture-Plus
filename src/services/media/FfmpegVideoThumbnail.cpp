// Shared software cover conversion, reused from the validated Linux adapter.
// Windows invokes this only when its retained native decoder is unavailable.
#include "services/media/FfmpegVideoInspection.h"
#include "core/PixelBuffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <climits>
#include <memory>
#include <new>

namespace HuhuCapture::Media::Detail {
namespace {
Failure cancelled() { return {Error::Cancelled, {}}; }
Failure invalid(const char* detail) { return {Error::InvalidMedia, QString::fromLatin1(detail)}; }
Failure budget(const char* detail) { return {Error::Budget, QString::fromLatin1(detail)}; }
Failure codecFailure(int code, const char* operation)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return {Error::InvalidMedia, QString::fromLatin1(operation) + QStringLiteral(": ") + QString::fromUtf8(message)};
}
struct ScaleDeleter { void operator()(SwsContext* value) const { sws_freeContext(value); } };
Failure videoLayout(int width, int height, const Limits& limits)
{
    const auto layout = Core::pixelLayout(width, height, 4);
    return !layout || layout->bytes > limits.maximumImageBytes ? budget("Video exceeds the decoded-pixel budget") : Failure{};
}

VideoThumbnailResult makeCover(const AVFrame& frame, unsigned edge, const Limits& limits, std::stop_token stop)
{
    if (stop.stop_requested()) return {cancelled(), {}};
    static_assert(LIBSWSCALE_VERSION_INT == AV_VERSION_INT(10, 1, 102));
    if (swscale_version() != LIBSWSCALE_VERSION_INT)
        return {{Error::Unsupported, QStringLiteral("Loaded swscale differs from the pinned thumbnail ABI")}, {}};
    if (auto error = videoLayout(frame.width, frame.height, limits)) return {std::move(error), {}};
    if (frame.color_trc == AVCOL_TRC_SMPTE2084 || frame.color_trc == AVCOL_TRC_ARIB_STD_B67)
        return {{Error::Unsupported, QStringLiteral("HDR thumbnail tone mapping has not been validated")}, {}};
    const QSize original(frame.width, frame.height);
    const auto dimensions = original.width() <= static_cast<int>(edge) && original.height() <= static_cast<int>(edge) ? original :
        original.scaled(static_cast<int>(edge), static_cast<int>(edge), Qt::KeepAspectRatio);
    // Fixed FFmpeg's SIMD output kernels require their native aligned row and
    // tail allocation. A tightly packed QImage is not that allocation: the
    // actual 252x190 X11 recording wrote 16 bytes beyond its last RGB32 row.
    // Reuse av_frame_get_buffer, then copy only complete logical image rows.
    // The bound mirrors get_video_buffer's BGRA allocation in the locked
    // libavutil/frame.c: 64-byte rows, height rounded to 32, plus native padding.
    constexpr int alignment = 64;
    const auto paddedHeight = (std::int64_t{dimensions.height()} + 31) / 32 * 32;
    if (paddedHeight > INT_MAX) return {budget("Video cover padding exceeds the dimension budget"), {}};
    const int paddedBytes = av_image_get_buffer_size(AV_PIX_FMT_BGRA, dimensions.width(),
        static_cast<int>(paddedHeight), alignment);
    constexpr std::uint64_t nativePadding = 8 * alignment;
    if (paddedBytes < 0 || static_cast<std::uint64_t>(paddedBytes) + nativePadding > limits.maximumImageBytes)
        return {budget("Video cover native storage exceeds the decoded-pixel budget"), {}};
    DecodedFrame converted(av_frame_alloc());
    if (!converted) return {budget("Cannot allocate the native video cover"), {}};
    converted->format = AV_PIX_FMT_BGRA; converted->width = dimensions.width(); converted->height = dimensions.height();
    const int allocated = av_frame_get_buffer(converted.get(), alignment);
    if (allocated < 0) return {codecFailure(allocated, "Allocate native video cover storage"), {}};
    if (!converted->buf[0] || converted->buf[0]->size > limits.maximumImageBytes)
        return {budget("Native video cover allocation exceeded its checked budget"), {}};
    std::unique_ptr<SwsContext, ScaleDeleter> scaler(sws_getContext(frame.width, frame.height,
        static_cast<AVPixelFormat>(frame.format), dimensions.width(), dimensions.height(), AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) return {budget("Cannot allocate the video cover scaler"), {}};
    int colorspace = SWS_CS_DEFAULT;
    switch (frame.colorspace) {
    case AVCOL_SPC_BT709: colorspace = SWS_CS_ITU709; break;
    case AVCOL_SPC_FCC: colorspace = SWS_CS_FCC; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: colorspace = SWS_CS_ITU601; break;
    case AVCOL_SPC_SMPTE240M: colorspace = SWS_CS_SMPTE240M; break;
    case AVCOL_SPC_BT2020_NCL: colorspace = SWS_CS_BT2020; break;
    default: break;
    }
    const auto* coefficients = sws_getCoefficients(colorspace);
    const int colorResult = sws_setColorspaceDetails(scaler.get(), coefficients,
        frame.color_range == AVCOL_RANGE_JPEG ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);
    if (colorResult < 0) return {codecFailure(colorResult, "Configure video cover color conversion"), {}};
    const int rows = sws_scale(scaler.get(), frame.data, frame.linesize, 0, frame.height,
        converted->data, converted->linesize);
    if (rows != dimensions.height()) return {invalid("Video cover scaler returned an incomplete image"), {}};
    if (stop.stop_requested()) return {cancelled(), {}};
    const QImage view(converted->data[0], dimensions.width(), dimensions.height(), converted->linesize[0], QImage::Format_RGB32);
    QImage image = view.copy();
    if (image.isNull()) return {budget("Cannot allocate the owned video cover"), {}};
    return {{}, std::move(image), ThumbnailDecoder::Ffmpeg};
}

} // namespace

VideoThumbnailResult softwareVideoThumbnail(const QString& path, unsigned edge, const Limits& limits, std::stop_token stop)
{
    if (stop.stop_requested()) return {cancelled(), {}};
    if (!edge || edge > INT_MAX) return {budget("Invalid thumbnail edge budget"), {}};
    try {
        auto decoded = decodeFirstVideoFrame(path, limits, stop);
        if (decoded.failure) return {std::move(decoded.failure), {}};
        if (!decoded.frame) return {invalid("Decoder did not retain a first video frame"), {}};
        return makeCover(*decoded.frame, edge, limits, stop);
    } catch (const std::bad_alloc&) {
        return {budget("Cannot allocate video thumbnail state"), {}};
    }
}
} // namespace HuhuCapture::Media::Detail
