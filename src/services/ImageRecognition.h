#pragma once
#include <windows.h>
#include <string>

// QR decoding uses quirc with UTF-8 / Shift-JIS payload conversion.
class ImageRecognition final {
public:
    static std::wstring decodeQrCode(int width, int height, BYTE* data);
};
