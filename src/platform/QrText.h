#pragma once

#include "core/QrRecognition.h"

#include <string>

namespace HuhuCapture::Platform {

enum class QrTextStatus { Decoded, Empty, UnknownEncoding, UnsupportedEncoding, InvalidEncoding, Unavailable, Allocation };
enum class QrTextEncoding { Unspecified, Utf8, Windows932, WindowsAnsi, DeclaredEci };

struct QrText {
    QrTextStatus status{QrTextStatus::Empty};
    QrTextEncoding encoding{QrTextEncoding::Unspecified};
    std::u16string text;
    std::uint32_t codePage{}; // Actual Windows page; zero for a non-Windows ECI conversion.
    std::int64_t nativeCode{};
    bool readable() const noexcept { return status == QrTextStatus::Decoded || status == QrTextStatus::Empty; }
};

struct QrTextDocument {
    std::vector<QrText> codes; // One result for each decoded raw payload, including unknown encodings.
    std::u16string joined;    // Existing nonempty-readable-code newline joining, with embedded NUL intact.
    std::size_t unreadable{};
};

// Shared policy: strict UTF-8 first, then Kanji as Windows CP932, otherwise the
// old Windows ACP. Linux never guesses an unavailable ACP; an explicit ECI can
// identify a supported converter, otherwise bytes stay in Core::QrDetection.
QrText decodeQrPayload(std::span<const std::uint8_t> payload, int dataType, std::uint32_t eci);
Core::QrError decodeQrText(const Core::QrDetection& detection, QrTextDocument& output,
    std::stop_token stop = {});

namespace Detail {
// Only this conversion boundary differs by OS. It does not detect QR codes,
// choose fallback order, join text, access the clipboard, or show a dialog.
QrText convertQrPayload(std::span<const std::uint8_t> payload,
    QrTextEncoding encoding, std::uint32_t eci);
}

} // namespace HuhuCapture::Platform
