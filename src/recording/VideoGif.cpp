#include "recording/VideoGif.h"
#include "platform/windows/GdiCapture.h"
#include "core/PixelBuffer.h"
#include "gifski.h"
#include "cgif/cgif.h"
#include <cstdio>
#include <cerrno>
#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace VideoGif {
namespace {

struct GifskiDeleter {
    HuhuCapture::Core::RecordingResult* result;
    void operator()(gifski* encoder) const noexcept
    {
        const auto status = gifski_finish(encoder);
        result->finish(status == GIFSKI_OK, status);
    }
};

struct CgifDeleter {
    HuhuCapture::Core::RecordingResult* result;
    void operator()(CGIFrgb* encoder) const noexcept
    {
        const auto status = cgif_rgb_close(encoder);
        result->finish(status == CGIF_OK, status);
    }
};

struct FileDeleter {
    HuhuCapture::Core::RecordingResult* result;
    void operator()(FILE* file) const noexcept
    {
        if (fclose(file) != 0) result->finish(false, errno);
    }
};

std::string pathToUtf8(const std::wstring& path)
{
    if (path.empty() || path.find(L'\0') != std::wstring::npos ||
        path.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
    const auto length = static_cast<int>(path.size());
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string text(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), length,
        text.data(), count, nullptr, nullptr) != count) return {};
    return text;
}

bool validFrame(const GifParam* params)
{
    if (!params || params->fps == 0) return false;
    const auto layout = HuhuCapture::Core::pixelLayout(params->w, params->h, 3);
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
    using HuhuCapture::Core::RecordingStage;
    if (!params) return;
    params->result = {};
    if (!validFrame(params)) {
        params->result.fail(RecordingStage::Prepare, GIFSKI_INVALID_INPUT);
        return;
    }
    GifskiSettings settings{
        .width{ static_cast<std::uint32_t>(params->w) },
        .height{ static_cast<std::uint32_t>(params->h) },
        .quality{ 80 },
        .fast{ true },
        .repeat{ 0 }
    };
    std::unique_ptr<gifski, GifskiDeleter> encoder(gifski_new(&settings), {&params->result});
    if (!encoder) {
        params->result.fail(RecordingStage::Prepare, GIFSKI_INVALID_INPUT);
        return;
    }
    try {
        const auto path = pathToUtf8(params->path);
        if (path.empty()) {
            params->result.fail(RecordingStage::Prepare, GIFSKI_INVALID_INPUT);
            return;
        }
        const auto rgb = *HuhuCapture::Core::pixelLayout(params->w, params->h, 3);
        std::vector<std::uint8_t> pixels(rgb.bytes);
        HuhuCapture::Platform::GdiCapture capture;
        const DWORD interval = 1000 / params->fps;
        bool outputStarted = false;
        while (!params->isFinish) {
            const auto start = GetTickCount64();
            if (!capture.capture(params->x, params->y, params->w, params->h, true)) {
                params->result.fail(RecordingStage::Capture, ERROR_GEN_FAILURE);
                break;
            }
            if (!HuhuCapture::Core::bgraToRgb(capture.pixels(), pixels, params->w, params->h)) {
                params->result.fail(RecordingStage::Encode, GIFSKI_INVALID_INPUT);
                break;
            }
            // Do not create an empty output stream when stopped before the first
            // frame. gifski_finish still releases its handle in that case.
            if (!outputStarted) {
                const auto status = gifski_set_file_output(encoder.get(), path.c_str());
                if (status != GIFSKI_OK) {
                    params->result.fail(RecordingStage::Prepare, status);
                    break;
                }
                outputStarted = true;
            }
            const auto index = params->result.framesWritten;
            if (index > (std::numeric_limits<std::uint32_t>::max)()) {
                params->result.fail(RecordingStage::Encode, GIFSKI_INVALID_INPUT);
                break;
            }
            const auto timestamp = static_cast<double>(index) / params->fps;
            const auto status = gifski_add_frame_rgb(encoder.get(), static_cast<std::uint32_t>(index),
                settings.width, static_cast<std::uint32_t>(rgb.stride), settings.height,
                pixels.data(), timestamp);
            if (status != GIFSKI_OK) {
                params->result.fail(RecordingStage::Encode, status);
                break;
            }
            ++params->result.framesWritten;
            waitForNextFrame(start, interval);
        }
    }
    catch (const std::bad_alloc&) {
        params->result.fail(RecordingStage::Encode, ERROR_NOT_ENOUGH_MEMORY);
    }
    catch (...) {
        params->result.fail(RecordingStage::Encode, ERROR_GEN_FAILURE);
    }
    // Explicitly join/finish gifski's writer before the owner inspects result.
    // The deleter also records finish errors on every earlier return path.
    encoder.reset();
}

// Retained alternate encoder; the application still selects gifski above.
void createGif2(GifParam* params)
{
    using HuhuCapture::Core::RecordingStage;
    if (!params) return;
    params->result = {};
    if (!validFrame(params)) {
        params->result.fail(RecordingStage::Prepare, ERROR_INVALID_PARAMETER);
        return;
    }
    FILE* opened{};
    const auto openStatus = _wfopen_s(&opened, params->path.c_str(), L"wb");
    if (openStatus != 0 || !opened) {
        params->result.fail(RecordingStage::Prepare, openStatus);
        return;
    }
    std::unique_ptr<FILE, FileDeleter> file(opened, {&params->result});
    CGIFrgb_Config config{};
    config.pWriteFn = [](void* context, const std::uint8_t* data, std::size_t size) -> int {
        return fwrite(data, 1, size, static_cast<FILE*>(context)) == size ? 0 : -1;
    };
    config.pContext = file.get();
    config.width = params->w;
    config.height = params->h;
    config.genFlags = CGIF_FRAME_GEN_USE_DIFF_WINDOW | CGIF_FRAME_GEN_USE_TRANSPARENCY;
    std::unique_ptr<CGIFrgb, CgifDeleter> encoder(cgif_rgb_newgif(&config), {&params->result});
    if (!encoder) {
        params->result.fail(RecordingStage::Prepare, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    CGIFrgb_FrameConfig frame{};
    frame.fmtChan = CGIF_CHAN_FMT_RGB;
    frame.delay = static_cast<std::uint16_t>(std::max(2, static_cast<int>(100.0 / params->fps + 0.5)));
    frame.attrFlags = CGIF_RGB_FRAME_ATTR_NO_DITHERING;
    const DWORD interval = frame.delay * 10;
    const auto rgb = *HuhuCapture::Core::pixelLayout(params->w, params->h, 3);
    std::vector<std::uint8_t> pixels(rgb.bytes);
    HuhuCapture::Platform::GdiCapture capture;
    while (!params->isFinish) {
        const auto start = GetTickCount64();
        if (!capture.capture(params->x, params->y, params->w, params->h, true)) {
            params->result.fail(RecordingStage::Capture, ERROR_GEN_FAILURE);
            break;
        }
        if (!HuhuCapture::Core::bgraToRgb(capture.pixels(), pixels, params->w, params->h)) {
            params->result.fail(RecordingStage::Encode, ERROR_INVALID_PARAMETER);
            break;
        }
        frame.pImageData = pixels.data();
        const auto status = cgif_rgb_addframe(encoder.get(), &frame);
        if (status != CGIF_OK) {
            params->result.fail(RecordingStage::Encode, status);
            break;
        }
        ++params->result.framesWritten;
        waitForNextFrame(start, interval);
    }
    // The encoder is destroyed before the FILE its callback borrows.
}

} // namespace VideoGif
