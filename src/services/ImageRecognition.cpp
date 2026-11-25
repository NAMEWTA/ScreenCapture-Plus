#include "pch.h"
#include "services/ImageRecognition.h"
#include "core/PixelBuffer.h"
#include "core/QrRecognition.h"
#include "platform/QrText.h"

#include <new>

std::wstring ImageRecognition::decodeQrCode(int width, int height, std::uint8_t* data)
{
    const auto layout = HuhuCapture::Core::pixelLayout(width, height, 4);
    if (!layout || !data) return {};
    HuhuCapture::Core::QrDetection detection;
    if (HuhuCapture::Core::recognizeQr({width, height, layout->stride, {data, layout->bytes}}, detection) !=
        HuhuCapture::Core::QrError::None) return {};
    HuhuCapture::Platform::QrTextDocument text;
    if (HuhuCapture::Platform::decodeQrText(detection, text) != HuhuCapture::Core::QrError::None) return {};
    try {
        static_assert(sizeof(wchar_t) == sizeof(char16_t));
        return {text.joined.begin(), text.joined.end()};
    } catch (const std::bad_alloc&) {
        return {};
    }
}
