#include "platform/QrText.h"

#include "quirc/quirc.h"

#include <new>
#include <utility>

namespace HuhuCapture::Platform {

QrText decodeQrPayload(std::span<const std::uint8_t> payload, int dataType, std::uint32_t eci)
{
    if (payload.empty()) return {};
    try {
        auto utf8 = Detail::convertQrPayload(payload, QrTextEncoding::Utf8, eci);
        if (utf8.status != QrTextStatus::InvalidEncoding) return utf8;
        return Detail::convertQrPayload(payload,
            dataType == QUIRC_DATA_TYPE_KANJI ? QrTextEncoding::Windows932 : QrTextEncoding::WindowsAnsi, eci);
    } catch (const std::bad_alloc&) {
        QrText result; result.status = QrTextStatus::Allocation; return result;
    }
}

Core::QrError decodeQrText(const Core::QrDetection& detection, QrTextDocument& output,
    std::stop_token stop)
{
    try {
        QrTextDocument candidate;
        candidate.codes.reserve(detection.codes.size());
        for (const auto& code : detection.codes) {
            if (stop.stop_requested()) return Core::QrError::Cancelled;
            auto text = decodeQrPayload(code.payload, code.dataType, code.eci);
            if (text.status == QrTextStatus::Allocation) return Core::QrError::Allocation;
            if (!text.readable()) ++candidate.unreadable;
            if (text.readable() && !text.text.empty()) {
                if (!candidate.joined.empty()) candidate.joined += u'\n';
                candidate.joined += text.text;
            }
            candidate.codes.push_back(std::move(text));
        }
        if (stop.stop_requested()) return Core::QrError::Cancelled;
        output = std::move(candidate);
        return Core::QrError::None;
    } catch (const std::bad_alloc&) {
        return Core::QrError::Allocation;
    }
}

} // namespace HuhuCapture::Platform
