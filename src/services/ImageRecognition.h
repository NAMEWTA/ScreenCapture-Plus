#pragma once
#include <windows.h>
#include <string>

// Recognition adapters. OCR keeps the existing ImageReader.exe lookup order;
// QR decoding keeps quirc, its grayscale weights and UTF-8/Shift-JIS fallback.
class ImageRecognition final {
public:
    static bool openWithImageReader(int width, int height, BYTE* data);
    static std::wstring decodeQrCode(int width, int height, BYTE* data);
};
