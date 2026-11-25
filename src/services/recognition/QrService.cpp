#include "services/recognition/QrService.h"
#include "core/PixelBuffer.h"

#include <bit>
#include <new>
#include <utility>

namespace HuhuCapture::Recognition {

Core::QrError recognizeImage(const QImage& image, QrResult& output,
    const Core::QrLimits& limits, std::stop_token stop)
{
    static_assert(std::endian::native == std::endian::little,
        "Supported Windows x64 / Linux amd64 ARGB32 storage is BGRA");
    const auto layout = Core::pixelLayout(image.width(), image.height(), 4);
    const auto workingBytes = Core::qrWorkingBytes(image.width(), image.height());
    if (image.isNull() || !layout || !workingBytes) return Core::QrError::InvalidInput;
    if (stop.stop_requested()) return Core::QrError::Cancelled;
    const bool native = image.format() == QImage::Format_ARGB32 || image.format() == QImage::Format_RGB32;
    const auto normalizationBytes = native ? std::size_t{} : layout->bytes;
    if (normalizationBytes > limits.maximumWorkingBytes ||
        *workingBytes > limits.maximumWorkingBytes - normalizationBytes) return Core::QrError::BudgetExceeded;
    try {
        const auto pixels = native ? image : image.convertToFormat(QImage::Format_ARGB32);
        if (pixels.isNull()) return Core::QrError::Allocation;
        if (pixels.bytesPerLine() <= 0 || pixels.sizeInBytes() <= 0) return Core::QrError::InvalidInput;
        QrResult candidate;
        const Core::QrImageView view{pixels.width(), pixels.height(), static_cast<std::size_t>(pixels.bytesPerLine()),
            {pixels.constBits(), static_cast<std::size_t>(pixels.sizeInBytes())}};
        const auto error = Core::recognizeQr(view, candidate.detection,
            {limits.maximumWorkingBytes - normalizationBytes}, stop);
        if (error != Core::QrError::None) return error;
        const auto textError = Platform::decodeQrText(candidate.detection, candidate.decoded, stop);
        if (textError != Core::QrError::None) return textError;
        candidate.text = QString::fromUtf16(candidate.decoded.joined.data(),
            static_cast<qsizetype>(candidate.decoded.joined.size()));
        if (stop.stop_requested()) return Core::QrError::Cancelled;
        output = std::move(candidate);
        return Core::QrError::None;
    } catch (const std::bad_alloc&) {
        return Core::QrError::Allocation;
    }
}

} // namespace HuhuCapture::Recognition
