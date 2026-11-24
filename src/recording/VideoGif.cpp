#include "pch.h"
#include "recording/VideoGif.h"
#include "platform/windows/GdiCapture.h"
#include "core/PixelBuffer.h"
#include "gifski.h"
#include "cgif/cgif.h"
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace VideoGif {
namespace {

struct GifskiDeleter {
    void operator()(gifski* encoder) const noexcept { gifski_finish(encoder); }
};

struct CgifDeleter {
    void operator()(CGIFrgb* encoder) const noexcept { cgif_rgb_close(encoder); }
};

bool validFrame(const GifParam* params)
{
    if (!params || params->fps == 0) return false;
    const auto layout = ScreenCapture::Core::pixelLayout(params->w, params->h, 3);
    return layout && layout->stride <= (std::numeric_limits<std::uint32_t>::max)();
}

void waitForNextFrame(ULONGLONG start, DWORD interval)
{
    const auto elapsed = GetTickCount64() - start;
    if (elapsed < interval) Sleep(interval - static_cast<DWORD>(elapsed));
}

} // namespace

void createGif(GifParam* params)
{
    if (!validFrame(params)) return;
    GifskiSettings settings{
        .width{ static_cast<std::uint32_t>(params->w) },
        .height{ static_cast<std::uint32_t>(params->h) },
        .quality{ 80 },
        .fast{ true },
        .repeat{ 0 }
    };
    std::unique_ptr<gifski, GifskiDeleter> encoder(gifski_new(&settings));
    if (!encoder) return;
    const auto path = Ling::Util::convertToStr(params->path);
    if (gifski_set_file_output(encoder.get(), path.c_str()) != GIFSKI_OK) return;

    const auto rgb = *ScreenCapture::Core::pixelLayout(params->w, params->h, 3);
    std::vector<std::uint8_t> pixels(rgb.bytes);
    ScreenCapture::Platform::GdiCapture capture;
    const DWORD interval = 1000 / params->fps;
    while (!params->isFinish) {
        const auto start = GetTickCount64();
        if (!capture.capture(params->x, params->y, params->w, params->h, true)) break;
        if (!ScreenCapture::Core::bgraToRgb(capture.pixels(), pixels, params->w, params->h)) break;
        const auto index = params->framesWritten;
        const auto timestamp = static_cast<double>(index) / params->fps;
        if (gifski_add_frame_rgb(encoder.get(), index, settings.width,
                                static_cast<std::uint32_t>(rgb.stride), settings.height,
                                pixels.data(), timestamp) != GIFSKI_OK) break;
        ++params->framesWritten;
        waitForNextFrame(start, interval);
    }
}

// Retained alternate encoder; the application still selects gifski above.
void createGif2(GifParam* params)
{
    if (!validFrame(params)) return;
    FILE* opened{};
    if (_wfopen_s(&opened, params->path.c_str(), L"wb") != 0 || !opened) return;
    std::unique_ptr<FILE, decltype(&fclose)> file(opened, &fclose);
    CGIFrgb_Config config{};
    config.pWriteFn = [](void* context, const std::uint8_t* data, std::size_t size) -> int {
        return fwrite(data, 1, size, static_cast<FILE*>(context)) == size ? 0 : -1;
    };
    config.pContext = file.get();
    config.width = params->w;
    config.height = params->h;
    config.genFlags = CGIF_FRAME_GEN_USE_DIFF_WINDOW | CGIF_FRAME_GEN_USE_TRANSPARENCY;
    std::unique_ptr<CGIFrgb, CgifDeleter> encoder(cgif_rgb_newgif(&config));
    if (!encoder) return;
    CGIFrgb_FrameConfig frame{};
    frame.fmtChan = CGIF_CHAN_FMT_RGB;
    frame.delay = static_cast<std::uint16_t>(std::max(2, static_cast<int>(100.0 / params->fps + 0.5)));
    frame.attrFlags = CGIF_RGB_FRAME_ATTR_NO_DITHERING;
    const DWORD interval = frame.delay * 10;
    const auto rgb = *ScreenCapture::Core::pixelLayout(params->w, params->h, 3);
    std::vector<std::uint8_t> pixels(rgb.bytes);
    ScreenCapture::Platform::GdiCapture capture;
    while (!params->isFinish) {
        const auto start = GetTickCount64();
        if (!capture.capture(params->x, params->y, params->w, params->h, true)) break;
        if (!ScreenCapture::Core::bgraToRgb(capture.pixels(), pixels, params->w, params->h)) break;
        frame.pImageData = pixels.data();
        if (cgif_rgb_addframe(encoder.get(), &frame) != CGIF_OK) break;
        ++params->framesWritten;
        waitForNextFrame(start, interval);
    }
    // The encoder is destroyed before the FILE its callback borrows.
}

} // namespace VideoGif
