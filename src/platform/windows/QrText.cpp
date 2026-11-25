#include "platform/QrText.h"

#include <windows.h>
#include <limits>

namespace HuhuCapture::Platform::Detail {

QrText convertQrPayload(std::span<const std::uint8_t> payload, QrTextEncoding encoding, std::uint32_t)
{
    QrText result; result.encoding = encoding;
    if (payload.empty()) return result;
    if (payload.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        result.status = QrTextStatus::InvalidEncoding; result.nativeCode = ERROR_INVALID_PARAMETER; return result;
    }
    const UINT page = encoding == QrTextEncoding::Utf8 ? CP_UTF8 :
        encoding == QrTextEncoding::Windows932 ? 932 : CP_ACP;
    const DWORD flags = encoding == QrTextEncoding::Utf8 ? MB_ERR_INVALID_CHARS : 0;
    result.codePage = page == CP_ACP ? GetACP() : page;
    const auto* bytes = reinterpret_cast<const char*>(payload.data());
    const auto size = static_cast<int>(payload.size());
    const int count = MultiByteToWideChar(page, flags, bytes, size, nullptr, 0);
    if (count <= 0) {
        result.nativeCode = GetLastError();
        result.status = result.nativeCode == ERROR_NO_UNICODE_TRANSLATION ?
            QrTextStatus::InvalidEncoding : QrTextStatus::Unavailable;
        return result;
    }
    // wchar_t is a distinct type from char16_t even on Windows. Use the native
    // buffer for the API and convert code units without type-punning writes.
    std::wstring native(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(page, flags, bytes, size, native.data(), count) != count) {
        result.status = QrTextStatus::Unavailable; result.nativeCode = GetLastError(); return result;
    }
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    result.text.assign(native.begin(), native.end());
    result.status = QrTextStatus::Decoded;
    return result;
}

} // namespace HuhuCapture::Platform::Detail
