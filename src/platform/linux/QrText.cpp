#include "platform/QrText.h"

#include "quirc/quirc.h"

#include <bit>
#include <cerrno>
#include <iconv.h>
#include <limits>
#include <utility>

namespace HuhuCapture::Platform::Detail {
namespace {

// These names are supplied by the platform converter, not a second Unicode or
// QR parser. Preserve a raw/unsupported result if the installed libc lacks one.
const char* eciEncoding(std::uint32_t eci) noexcept
{
    switch (eci) {
    case 0: return nullptr;
    case QUIRC_ECI_ISO_8859_1: case 3: return "ISO-8859-1";
    case QUIRC_ECI_IBM437: return "CP437";
    case QUIRC_ECI_ISO_8859_2: return "ISO-8859-2";
    case QUIRC_ECI_ISO_8859_3: return "ISO-8859-3";
    case QUIRC_ECI_ISO_8859_4: return "ISO-8859-4";
    case QUIRC_ECI_ISO_8859_5: return "ISO-8859-5";
    case QUIRC_ECI_ISO_8859_6: return "ISO-8859-6";
    case QUIRC_ECI_ISO_8859_7: return "ISO-8859-7";
    case QUIRC_ECI_ISO_8859_8: return "ISO-8859-8";
    case QUIRC_ECI_ISO_8859_9: return "ISO-8859-9";
    case QUIRC_ECI_WINDOWS_874: return "CP874";
    case QUIRC_ECI_ISO_8859_13: return "ISO-8859-13";
    case QUIRC_ECI_ISO_8859_15: return "ISO-8859-15";
    case QUIRC_ECI_SHIFT_JIS: return "CP932";
    case QUIRC_ECI_UTF_8: return "UTF-8";
    default: return nullptr;
    }
}

class Converter final {
public:
    explicit Converter(const char* encoding) : handle_(iconv_open("UTF-16LE", encoding)) {}
    ~Converter() { if (valid()) iconv_close(handle_); }
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;
    bool valid() const noexcept { return handle_ != reinterpret_cast<iconv_t>(-1); }
    iconv_t get() const noexcept { return handle_; }
private:
    iconv_t handle_;
};

} // namespace

QrText convertQrPayload(std::span<const std::uint8_t> payload, QrTextEncoding encoding, std::uint32_t eci)
{
    static_assert(std::endian::native == std::endian::little, "The supported Linux amd64 target uses UTF-16LE code units");
    QrText result; result.encoding = encoding;
    if (payload.empty()) return result;
    const char* source = encoding == QrTextEncoding::Utf8 ? "UTF-8" :
        encoding == QrTextEncoding::Windows932 ? "CP932" : eciEncoding(eci);
    if (!source) {
        result.encoding = QrTextEncoding::Unspecified;
        result.status = eci ? QrTextStatus::UnsupportedEncoding : QrTextStatus::UnknownEncoding;
        return result;
    }
    if (encoding == QrTextEncoding::WindowsAnsi) result.encoding = QrTextEncoding::DeclaredEci;
    result.codePage = encoding == QrTextEncoding::Utf8 ? 65001 : encoding == QrTextEncoding::Windows932 ? 932 : 0;
    const Converter converter(source);
    if (!converter.valid()) { result.status = QrTextStatus::Unavailable; result.nativeCode = errno; return result; }
    if (payload.size() > ((std::numeric_limits<std::size_t>::max)() / sizeof(char16_t)) - 2) {
        result.status = QrTextStatus::Allocation; return result;
    }
    // UTF-8 and each supported legacy converter emit at most one UTF-16 code
    // unit per input byte. Extra units allow the converter's final flush.
    std::u16string text(payload.size() + 2, u'\0');
    auto* input = const_cast<char*>(reinterpret_cast<const char*>(payload.data()));
    auto remainingInput = payload.size();
    auto* destination = reinterpret_cast<char*>(text.data());
    auto remainingOutput = text.size() * sizeof(char16_t);
    const auto capacity = remainingOutput;
    const auto converted = iconv(converter.get(), &input, &remainingInput, &destination, &remainingOutput);
    if (converted == static_cast<std::size_t>(-1) || remainingInput) {
        result.nativeCode = errno;
        result.status = errno == EILSEQ || errno == EINVAL ? QrTextStatus::InvalidEncoding : QrTextStatus::Unavailable;
        return result;
    }
    if (iconv(converter.get(), nullptr, nullptr, &destination, &remainingOutput) == static_cast<std::size_t>(-1) ||
        (capacity - remainingOutput) % sizeof(char16_t)) {
        result.status = QrTextStatus::Unavailable; result.nativeCode = errno; return result;
    }
    text.resize((capacity - remainingOutput) / sizeof(char16_t));
    result.text = std::move(text); result.status = QrTextStatus::Decoded;
    return result;
}

} // namespace HuhuCapture::Platform::Detail
