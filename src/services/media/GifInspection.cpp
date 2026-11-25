#include "services/media/GifInspection.h"

#include <algorithm>
#include <limits>
#include <memory>

namespace HuhuCapture::Media {

Inspection inspectGif(const QString& path, const Limits& limits,
    std::stop_token stop, const HuhuReceive* receive)
{
    Inspection result;
    result.kind = Kind::Gif;
    if (stop.stop_requested()) { result.failure = {Error::Cancelled, {}}; return result; }
    if (path.isEmpty() || path.contains(QChar(u'\0'))) {
        result.failure = {Error::Read, QStringLiteral("Invalid GIF path")}; return result;
    }
    // The product records at most six minutes at 16 FPS. Keep headroom for its
    // final frame while bounding foreign input; no full-frame animation array.
    constexpr std::uint32_t maximumFrames = 6000;
    constexpr std::uint64_t maximumGifCanvas = std::uint64_t{65535} * 65535;
    const auto canvas = std::min(limits.maximumImageBytes / 4, maximumGifCanvas);
    if (!canvas || !limits.maximumFileBytes ||
        canvas > (std::numeric_limits<std::uint64_t>::max)() / maximumFrames) {
        result.failure = {Error::Budget, QStringLiteral("Invalid GIF decoding budget")}; return result;
    }
    const HuhuGifLimits budget{limits.maximumFileBytes, canvas, canvas * maximumFrames,
        maximumFrames, 16u * 1024u * 1024u};
    HuhuCancellation* raw{};
    if (huhu_cancellation_create(&raw) != HUHU_OK) {
        result.failure = {Error::Budget, QStringLiteral("Could not prepare cancellable GIF decoding")}; return result;
    }
    std::unique_ptr<HuhuCancellation, decltype(&huhu_cancellation_free)> cancellation(raw, &huhu_cancellation_free);
    // Destroy stop_callback (and wait for any active callback) before freeing
    // the opaque atomic flag. No decoder ever calls back into C++ or the GUI.
    std::stop_callback cancelled(stop, [raw]() noexcept { (void)huhu_cancellation_request(raw); });
    const auto encoded = path.toUtf8();
    HuhuGifInfo info{};
    const auto status = huhu_gif_validate_file(reinterpret_cast<const std::uint8_t*>(encoded.constData()),
        static_cast<std::size_t>(encoded.size()), &budget, receive, cancellation.get(), &info);
    if (status != HUHU_OK) {
        const auto code = status == HUHU_CANCELLED ? Error::Cancelled :
            status == HUHU_LIMIT_EXCEEDED ? Error::Budget : status == HUHU_IO ? Error::Read : Error::InvalidMedia;
        result.failure = {code, status == HUHU_CANCELLED ? QString{} : QStringLiteral("Complete GIF validation failed")};
        return result;
    }
    if (!info.width || !info.height || !info.frames) {
        result.failure = {Error::InvalidMedia, QStringLiteral("GIF contains no complete frame")}; return result;
    }
    result.dimensions = QSize(static_cast<int>(info.width), static_cast<int>(info.height));
    result.frames = info.frames;
    return result;
}

} // namespace HuhuCapture::Media
