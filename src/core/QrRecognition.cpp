#include "core/QrRecognition.h"
#include "core/PixelBuffer.h"

#include "quirc/quirc_internal.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#if QUIRC_MAX_REGIONS != 65534
#error "QR recognition must retain the production QUIRC_MAX_REGIONS=65534 configuration"
#endif

namespace HuhuCapture::Core {

std::optional<std::size_t> qrWorkingBytes(int width, int height) noexcept
{
    const auto gray = pixelLayout(width, height, 1);
    if (!gray || gray->bytes > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return std::nullopt;
    constexpr auto limit = (std::numeric_limits<std::size_t>::max)();
    constexpr std::size_t bytesPerPixel = 1 + (QUIRC_PIXEL_ALIAS_IMAGE ? 0 : sizeof(quirc_pixel_t));
    if (gray->bytes > (limit - sizeof(quirc)) / bytesPerPixel) return std::nullopt;
    const auto imageBytes = sizeof(quirc) + gray->bytes * bytesPerPixel;
    const auto rows = static_cast<std::size_t>(height);
    if (rows > limit / 2) return std::nullopt;
    const auto floodEntries = (std::max)(std::size_t{1}, rows * 2 / 3);
    if (floodEntries > (limit - imageBytes) / sizeof(quirc_flood_fill_vars)) return std::nullopt;
    return imageBytes + floodEntries * sizeof(quirc_flood_fill_vars);
}

QrError recognizeQr(const QrImageView& image, QrDetection& output,
    const QrLimits& limits, std::stop_token stop)
{
    const auto layout = pixelLayout(image.width, image.height, 4);
    const auto workingBytes = qrWorkingBytes(image.width, image.height);
    if (!layout || !workingBytes || !image.bgra.data() || image.stride < layout->stride)
        return QrError::InvalidInput;
    const auto precedingRows = static_cast<std::size_t>(image.height - 1);
    if (precedingRows && image.stride > ((std::numeric_limits<std::size_t>::max)() - layout->stride) / precedingRows)
        return QrError::InvalidInput;
    if (image.bgra.size() < precedingRows * image.stride + layout->stride) return QrError::InvalidInput;
    if (*workingBytes > limits.maximumWorkingBytes) return QrError::BudgetExceeded;
    if (stop.stop_requested()) return QrError::Cancelled;

    try {
        std::unique_ptr<quirc, decltype(&quirc_destroy)> recognizer(quirc_new(), quirc_destroy);
        if (!recognizer || quirc_resize(recognizer.get(), image.width, image.height) < 0)
            return QrError::Allocation;
        int width{}, height{};
        auto* gray = quirc_begin(recognizer.get(), &width, &height);
        if (!gray || width != image.width || height != image.height) return QrError::Decoder;
        for (int y = 0; y < image.height; ++y) {
            if (stop.stop_requested()) return QrError::Cancelled;
            const auto* from = image.bgra.data() + static_cast<std::size_t>(y) * image.stride;
            auto* to = gray + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width);
            for (int x = 0; x < image.width; ++x, from += 4)
                to[x] = static_cast<std::uint8_t>((from[2] * 77 + from[1] * 150 + from[0] * 29) >> 8);
        }
        if (stop.stop_requested()) return QrError::Cancelled;
        quirc_end(recognizer.get());
        if (stop.stop_requested()) return QrError::Cancelled;
        const int count = quirc_count(recognizer.get());
        if (count < 0 || count > QUIRC_MAX_GRIDS) return QrError::Decoder;
        QrDetection candidate;
        candidate.candidates = static_cast<std::size_t>(count);
        candidate.codes.reserve(candidate.candidates);
        for (int i = 0; i < count; ++i) {
            if (stop.stop_requested()) return QrError::Cancelled;
            quirc_code code{};
            quirc_data data{};
            quirc_extract(recognizer.get(), i, &code);
            auto status = quirc_decode(&code, &data);
            bool mirrored = false;
            if (status == QUIRC_ERROR_DATA_ECC) {
                quirc_flip(&code);
                status = quirc_decode(&code, &data);
                mirrored = status == QUIRC_SUCCESS;
            }
            if (status != QUIRC_SUCCESS) { ++candidate.undecodableCandidates; continue; }
            if (data.payload_len < 0 || data.payload_len > QUIRC_MAX_PAYLOAD) return QrError::Decoder;
            QrCode decoded;
            decoded.payload.assign(data.payload, data.payload + data.payload_len);
            for (std::size_t corner = 0; corner < decoded.corners.size(); ++corner)
                decoded.corners[corner] = {code.corners[corner].x, code.corners[corner].y};
            decoded.version = data.version; decoded.dataType = data.data_type;
            decoded.eccLevel = data.ecc_level; decoded.mask = data.mask; decoded.eci = data.eci;
            decoded.mirrored = mirrored;
            candidate.codes.push_back(std::move(decoded));
        }
        if (stop.stop_requested()) return QrError::Cancelled;
        output = std::move(candidate);
        return QrError::None;
    } catch (const std::bad_alloc&) {
        return QrError::Allocation;
    }
}

} // namespace HuhuCapture::Core
