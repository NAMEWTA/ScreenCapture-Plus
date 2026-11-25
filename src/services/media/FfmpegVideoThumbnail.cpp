// Shared software cover conversion, reused from the Linux video adapter.
// Windows uses this only when the retained Media Foundation cover path reports
// Unsupported, including systems without an installed HEVC decoder.
#include "services/media/FfmpegVideoInspection.h"
#include "core/PixelBuffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <climits>
#include <memory>
#include <new>

namespace HuhuCapture::Media {
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
    if (auto error = videoLayout(frame.width, frame.height, limits)) return {std::move(error), {}};
    if (frame.color_trc == AVCOL_TRC_SMPTE2084 || frame.color_trc == AVCOL_TRC_ARIB_STD_B67)
        return {{Error::Unsupported, QStringLiteral("HDR thumbnail tone mapping has not been validated")}, {}};
    const QSize original(frame.width, frame.height);
    const auto dimensions = original.width() <= static_cast<int>(edge) && original.height() <= static_cast<int>(edge) ? original :
        original.scaled(static_cast<int>(edge), static_cast<int>(edge), Qt::KeepAspectRatio);
    QImage image(dimensions, QImage::Format_RGB32);
    if (image.isNull()) return {budget("Cannot allocate the video cover"), {}};
    std::unique_ptr<SwsContext, ScaleDeleter> scaler(sws_getContext(frame.width, frame.height,
        static_cast<AVPixelFormat>(frame.format), image.width(), image.height(), AV_PIX_FMT_BGRA,
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
    std::uint8_t* planes[]{image.bits(), nullptr, nullptr, nullptr};
    int strides[]{static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    const int rows = sws_scale(scaler.get(), frame.data, frame.linesize, 0, frame.height, planes, strides);
    if (rows != image.height()) return {invalid("Video cover scaler returned an incomplete image"), {}};
    if (stop.stop_requested()) return {cancelled(), {}};
    return {{}, std::move(image)};
}

} // namespace

VideoThumbnailResult Detail::softwareVideoThumbnail(const QString& path, unsigned edge, const Limits& limits, std::stop_token stop)
{
    if (stop.stop_requested()) return {cancelled(), {}};
    if (!edge || edge > INT_MAX) return {budget("Invalid thumbnail edge budget"), {}};
    static_assert(LIBSWSCALE_VERSION_INT == AV_VERSION_INT(10, 1, 102));
    if (swscale_version() != LIBSWSCALE_VERSION_INT)
        return {{Error::Unsupported, QStringLiteral("Loaded FFmpeg scaler differs from the pinned cover ABI")}, {}};
    try {
        auto decoded = Detail::decodeFirstVideoFrame(path, limits, stop);
        if (decoded.failure) return {std::move(decoded.failure), {}};
        if (!decoded.frame) return {invalid("Decoder did not retain a first video frame"), {}};
        return makeCover(*decoded.frame, edge, limits, stop);
    } catch (const std::bad_alloc&) {
        return {budget("Cannot allocate video thumbnail state"), {}};
    }
}
} // namespace HuhuCapture::Media
