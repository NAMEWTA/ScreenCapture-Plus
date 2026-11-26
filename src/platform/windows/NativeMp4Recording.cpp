#include "platform/windows/NativeMp4Recording.h"
#include "platform/windows/ComApartment.h"
#include "platform/windows/FileHandleStream.h"
#include "recording/VideoMp4.h"

#include <atlbase.h>
#include <mferror.h>
#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace HuhuCapture::Platform::Windows {
namespace {
using namespace Recording;
using Core::RecordingStage;

class MediaFoundationSession final {
public:
    MediaFoundationSession() : result_(MFStartup(MF_VERSION)) {}
    ~MediaFoundationSession() { if (SUCCEEDED(result_)) MFShutdown(); }
    HRESULT result() const noexcept { return result_; }
private:
    HRESULT result_;
};
class PhysicalPixelScope final {
public:
    PhysicalPixelScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixelScope() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
    explicit operator bool() const noexcept { return previous_ != nullptr; }
private:
    DPI_AWARENESS_CONTEXT previous_;
};

struct DuplicationTarget {
    CComPtr<IDXGIAdapter1> adapter;
    UINT output{};
    RECT monitor{};
};

bool contains(const RECT& bounds, const PixelRegion& region) noexcept
{
    return region.x >= bounds.left && region.y >= bounds.top &&
        std::int64_t{region.x} + region.width <= bounds.right &&
        std::int64_t{region.y} + region.height <= bounds.bottom;
}

std::vector<DuplicationTarget> duplicationTargets(PixelRegion region)
{
    std::vector<DuplicationTarget> targets;
    CComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return targets;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        CComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(adapterIndex, &adapter))) break;
        for (UINT index = 0;; ++index) {
            CComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(index, &output))) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop ||
                !contains(desc.DesktopCoordinates, region)) continue;
            // A duplication surface is not rotation-normalized. Reuse the
            // existing GDI desktop source for rotated or multi-output regions;
            // never clip a requested cross-screen rectangle to one monitor.
            if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
                continue;
            targets.push_back({adapter, index, desc.DesktopCoordinates});
        }
    }
    return targets;
}

BackendResult capture(const BackendRequest& request, int descriptor,
    const BackendEvents& events, std::stop_token stop)
{
    BackendResult result;
    const auto fail = [&](HRESULT error) {
        result.media.fail(RecordingStage::Prepare, FAILED(error) ? error : E_UNEXPECTED);
        return result;
    };
    const auto region = alignedMp4Region(request.region, request.maximumFrameBytes);
    if (request.format != Format::Mp4 || !region || !request.framesPerSecond ||
        request.framesPerSecond > 240 || request.maximumDuration.count() <= 0 ||
        request.maximumDuration > std::chrono::minutes(120) || request.firstFrameTimeout.count() <= 0 ||
        request.firstFrameTimeout > std::chrono::minutes(1) || !request.maximumQueuedAudioBytes ||
        request.maximumQueuedAudioBytes > 64ull * 1024 * 1024) return fail(E_INVALIDARG);
    if (stop.stop_requested()) { result.stopRequested = true; return result; }
    std::error_code fileError;
    const auto file = readFileDescriptorSnapshot(descriptor, fileError);
    if (!file || file->bytes) return fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));

    // Preserve the existing WASAPI activation apartment; each audio worker
    // continues to initialize its own MTA before using its native interfaces.
    ComApartment apartment(COINIT_APARTMENTTHREADED);
    if (FAILED(apartment.result())) return fail(apartment.result());
    MediaFoundationSession media;
    if (FAILED(media.result())) return fail(media.result());
    PhysicalPixelScope pixels;
    if (!pixels) return fail(HRESULT_FROM_WIN32(GetLastError()));
    const auto desktopX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const auto desktopY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const auto desktopWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const auto desktopHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (desktopWidth <= 0 || desktopHeight <= 0 ||
        region->x < desktopX || region->y < desktopY ||
        std::int64_t{region->x} + region->width > std::int64_t{desktopX} + desktopWidth ||
        std::int64_t{region->y} + region->height > std::int64_t{desktopY} + desktopHeight)
        return fail(HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER));

    VideoMp4::DESKTOPCAPTUREPARAMS parameters;
    parameters.fps = static_cast<int>(request.framesPerSecond);
    parameters.vbrm = 2; parameters.vbrq = 50; parameters.Qu = 50;
    parameters.Cursor = request.cursor;
    parameters.EndMS = static_cast<unsigned long long>(request.maximumDuration.count());
    parameters.FirstFrameTimeoutMS = static_cast<unsigned long long>(request.firstFrameTimeout.count());
    parameters.MaximumFrameBytes = request.maximumFrameBytes;
    parameters.MaximumQueuedAudioBytes = request.maximumQueuedAudioBytes;
    parameters.Controls = request.controls;
    std::stop_callback cancellation(stop, [&parameters] { parameters.MustEnd = true; });
    if (parameters.MustEnd) { result.stopRequested = true; return result; }
    if (!VideoMp4::setAudio(&parameters, request.systemAudio, request.microphone)) {
        result.media = parameters.result; result.stopRequested = stop.stop_requested(); return result;
    }
    const auto targets = duplicationTargets(*region);
    int code{};
    const auto run = [&](const DuplicationTarget* target) {
        if (parameters.MustEnd) return 0;
        parameters.result = {};
        parameters.UseGdiCapture = target == nullptr;
        parameters.VerifyDesktop = target != nullptr;
        parameters.ExpectedDesktop = target ? target->monitor : RECT{};
        parameters.ad = target ? target->adapter.p : nullptr;
        parameters.nOutput = target ? target->output : 0;
        parameters.rx = target
            ? RECT{region->x - target->monitor.left, region->y - target->monitor.top,
                region->x - target->monitor.left + region->width, region->y - target->monitor.top + region->height}
            : RECT{0, 0, region->width, region->height};
        parameters.GdiRegion = {region->x, region->y, region->x + region->width, region->y + region->height};
        parameters.Prepared = [&](GUID codec) {
            result.prepared = PreparedRecording{*region, codec == MFVideoFormat_H264 ? Codec::H264 : Codec::Hevc,
                parameters.UseGdiCapture ? CaptureSource::Gdi : CaptureSource::DesktopDuplication,
                Core::MediaClock::now(), request.systemAudio, request.microphone};
            if (events.prepared) events.prepared(*result.prepared);
        };
        parameters.Finalizing = [&] {
            if (result.prepared) result.elapsed = std::chrono::duration_cast<Core::MediaTime>(
                Core::MediaClock::now() - result.prepared->started);
            if (events.finalizing) events.finalizing();
        };
        CComPtr<IStream> fileStream;
        auto status = createFileHandleStream(descriptor, &fileStream);
        if (FAILED(status)) { parameters.result.fail(RecordingStage::Prepare, status); return -19; }
        // A previous unsupported codec may have emitted a container header.
        // Only our owned staging object is reset, after that writer has closed.
        status = fileStream->SetSize(ULARGE_INTEGER{});
        if (FAILED(status)) { parameters.result.fail(RecordingStage::Prepare, status); return -19; }
        CComPtr<IMFByteStream> stream;
        status = MFCreateMFByteStreamOnStream(fileStream, &stream);
        if (FAILED(status)) { parameters.result.fail(RecordingStage::Prepare, status); return -19; }
        parameters.OutputStream = stream;
        const auto captureCode = VideoMp4::DesktopCapture(parameters);
        parameters.OutputStream = nullptr;
        status = stream->Flush();
        if (FAILED(status)) parameters.result.finish(false, status);
        const auto closed = stream->Close();
        if (FAILED(closed)) parameters.result.finish(false, closed);
        if (parameters.result.finalizationError) return -14;
        return captureCode;
    };
    for (const auto codec : {MFVideoFormat_HEVC, MFVideoFormat_H264}) {
        if (parameters.MustEnd) break;
        parameters.VIDEO_ENCODING_FORMAT = codec;
        code = -2;
        for (const auto& target : targets) {
            if (parameters.MustEnd) break;
            code = run(&target);
            if (code != -2) break;
        }
        // GDI already supplies full virtual-desktop pixels for existing GIFs.
        // It is also the real source when duplication cannot cover this region.
        if (code == -2 && !parameters.MustEnd) code = run(nullptr);
        if (code != -5 && code != -10) break;
    }
    result.media = parameters.result;
    result.stopRequested = stop.stop_requested();
    result.durationLimitReached = parameters.DurationLimitReached;
    // Unsupported adapter/codec attempts are preparation alternatives. A stop
    // before a working alternative starts is a cancellation, not the failure
    // of the last rejected alternative. Never mask cleanup or active-media errors.
    if (result.stopRequested && !result.prepared && !result.media.finalizationError &&
        (code == -2 || code == -5 || code == -10)) { result.media = {}; code = 0; }
    if (code != 0 && !result.media.firstError) result.media.fail(RecordingStage::Prepare, code);
    return result;
}
}

Recording::BackendResult runNativeMp4(const Recording::BackendRequest& request,
    FileHandle writer, const Recording::BackendEvents& events, std::stop_token stop)
{
    Recording::BackendResult result;
    try {
        if (!writer) result.media.fail(Core::RecordingStage::Prepare, E_INVALIDARG);
        else result = capture(request, writer.get(), events, stop);
    } catch (const std::bad_alloc&) { result.media.fail(Core::RecordingStage::Encode, E_OUTOFMEMORY); }
    catch (...) { result.media.fail(Core::RecordingStage::Encode, E_FAIL); }
    result.stopRequested = stop.stop_requested();
    std::error_code error;
    if (!writer.close(error)) result.media.finish(false, error.value());
    return result;
}

} // namespace HuhuCapture::Platform::Windows
