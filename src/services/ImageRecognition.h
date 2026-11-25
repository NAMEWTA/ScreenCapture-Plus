#pragma once
#include <cstdint>
#include <string>

// The retained Windows command delegates to the shared QR and text services.
class ImageRecognition final {
public:
    static std::wstring decodeQrCode(int width, int height, std::uint8_t* data);
};
