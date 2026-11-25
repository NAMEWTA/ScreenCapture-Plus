#pragma once

#include "core/QrRecognition.h"
#include "fixtures/qr/qr-fixtures.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace HuhuCapture::TestQrFixtures {

inline const Fixture& fixture(std::string_view name)
{
    const auto found = std::find_if(fixtures.begin(), fixtures.end(), [name](const auto& item) { return item.name == name; });
    if (found == fixtures.end()) throw std::invalid_argument("Unknown QR test fixture");
    return *found;
}

struct Image {
    int width{}, height{};
    std::size_t stride{};
    std::vector<std::uint8_t> pixels;
    Image(int w, int h, std::size_t padding = 0)
        : width(w), height(h), stride(static_cast<std::size_t>(w) * 4 + padding),
          pixels(stride * static_cast<std::size_t>(h), 255) {}
    Core::QrImageView view() const { return {width, height, stride, pixels}; }
    void pixel(int x, int y, std::array<std::uint8_t, 4> color)
    {
        auto* destination = pixels.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
        std::copy(color.begin(), color.end(), destination);
    }
};

// Paint already-encoded, checked-in modules. This is only rasterization; QR
// encoding, correction and decoding remain the real Segno / quirc algorithms.
inline void paint(Image& image, const Fixture& source, int left, int top, int scale = 6,
    int quarterTurns = 0, bool mirror = false, std::uint8_t ink = 0)
{
    const auto side = (static_cast<std::int64_t>(source.size) + 8) * scale;
    if (scale <= 0 || left < 0 || top < 0 || side > static_cast<std::int64_t>(image.width) - left ||
        side > static_cast<std::int64_t>(image.height) - top)
        throw std::invalid_argument("The real QR fixture must fit the destination image");
    for (int y = 0; y < source.size; ++y) for (int x = 0; x < source.size; ++x) {
        if (source.modules[static_cast<std::size_t>(y * source.size + x)] != '1') continue;
        int px = mirror ? source.size - 1 - x : x, py = y;
        for (int turn = 0; turn < quarterTurns; ++turn) { const int oldX = px; px = source.size - 1 - py; py = oldX; }
        for (int row = 0; row < scale; ++row) for (int column = 0; column < scale; ++column)
            image.pixel(left + (px + 4) * scale + column, top + (py + 4) * scale + row,
                {ink, ink, ink, static_cast<std::uint8_t>((x + y) % 2 ? 0 : 255)});
    }
}

inline Image render(const Fixture& source, int quarterTurns = 0, bool mirror = false, std::size_t padding = 0)
{
    Image result((source.size + 8) * 6, (source.size + 8) * 6, padding);
    paint(result, source, 0, 0, 6, quarterTurns, mirror);
    return result;
}

inline bool payloadEquals(const Core::QrCode& code, std::string_view expected)
{
    return code.payload.size() == expected.size() && std::equal(code.payload.begin(), code.payload.end(), expected.begin(),
        [](std::uint8_t value, char byte) { return value == static_cast<std::uint8_t>(byte); });
}

} // namespace HuhuCapture::TestQrFixtures
