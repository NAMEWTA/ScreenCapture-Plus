// Native SourceReader, media-type and RGB-buffer flow reused from the shipping
// services/MediaHistory.cpp videoThumbnail. Keep recording on the existing MF
// backend; complete inspection uses the common strict FFmpeg service. This
// platform adapter only produces Qt-owned covers from the native first frame.
#define NOMINMAX
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include "services/media/VideoInspection.h"
#include "services/media/Mp4Envelope.h"
#include "core/MediaFrame.h"
#include "core/PixelBuffer.h"

#include <QSize>
#include <climits>
#include <cstring>
#include <limits>
#include <new>
#include <optional>

namespace HuhuCapture::Media {
namespace {
using Microsoft::WRL::ComPtr;

Failure nativeFailure(HRESULT code, const char* operation, Error category = Error::InvalidMedia)
{
    if (code == E_OUTOFMEMORY) category = Error::Budget;
    if (code == MF_E_TOPO_CODEC_NOT_FOUND || code == MF_E_INVALIDMEDIATYPE ||
        code == MF_E_UNSUPPORTED_BYTESTREAM_TYPE) category = Error::Unsupported;
    return {category, QString::fromLatin1(operation) + QStringLiteral(" (0x%1)")
        .arg(static_cast<quint32>(code), 8, 16, QChar(u'0'))};
}

Failure cancelled() { return {Error::Cancelled, {}}; }
Failure invalid(const char* detail) { return {Error::InvalidMedia, QString::fromLatin1(detail)}; }
Failure budget(const char* detail) { return {Error::Budget, QString::fromLatin1(detail)}; }

struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct MediaSession {
    HRESULT result{MFStartup(MF_VERSION, MFSTARTUP_LITE)};
    ~MediaSession() { if (SUCCEEDED(result)) MFShutdown(); }
};
struct NativeReader {
    // Destruction order keeps the original read-only stream and its sharing
    // reservation alive until the SourceReader and byte-stream wrapper stop.
    ComPtr<IStream> file;
    ComPtr<IMFByteStream> bytes;
    ComPtr<IMFSourceReader> reader;
    std::optional<DWORD> videoStream;

    Failure open(const QString& path, const Limits& limits, std::stop_token stop)
    {
        if (path.isEmpty() || path.contains(QChar(u'\0'))) return {Error::Read, QStringLiteral("Invalid MP4 path")};
        if (!limits.maximumFileBytes || !limits.maximumImageBytes) return budget("Invalid media budget");
        const auto native = path.toStdWString();
        HRESULT hr = SHCreateStreamOnFileEx(native.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
            FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, file.GetAddressOf());
        if (FAILED(hr)) return nativeFailure(hr, "Open local video file", Error::Read);
        STATSTG information{};
        hr = file->Stat(&information, STATFLAG_NONAME);
        if (FAILED(hr)) return nativeFailure(hr, "Read video file size", Error::Read);
        if (!information.cbSize.QuadPart || information.cbSize.QuadPart > limits.maximumFileBytes ||
            information.cbSize.QuadPart > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()))
            return budget("Video file exceeds the byte budget");
        const auto headerReader = [&](std::uint64_t offset, std::span<std::uint8_t> bytes) {
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            ULONG received{};
            hr = file->Seek(position, STREAM_SEEK_SET, nullptr);
            if (SUCCEEDED(hr)) hr = file->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &received);
            return SUCCEEDED(hr) && received == bytes.size();
        };
        if (auto error = Detail::checkMp4Envelope(information.cbSize.QuadPart, headerReader, limits, stop)) return error;
        LARGE_INTEGER start{};
        hr = file->Seek(start, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) return nativeFailure(hr, "Reset the validated MP4 envelope", Error::Read);
        hr = MFCreateMFByteStreamOnStream(file.Get(), bytes.GetAddressOf());
        if (FAILED(hr)) return nativeFailure(hr, "Wrap the owned file stream");
        ComPtr<IMFAttributes> byteAttributes;
        if (SUCCEEDED(bytes.As(&byteAttributes))) {
            hr = byteAttributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4");
            if (FAILED(hr)) return nativeFailure(hr, "Set local MP4 content type");
        }
        ComPtr<IMFAttributes> attributes;
        if (FAILED(hr = MFCreateAttributes(attributes.GetAddressOf(), 3)) ||
            FAILED(hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
            FAILED(hr = attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE)) ||
            FAILED(hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE)) ||
            FAILED(hr = MFCreateSourceReaderFromByteStream(bytes.Get(), attributes.Get(), reader.GetAddressOf())) ||
            FAILED(hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)))
            return nativeFailure(hr, "Create native local-file decoder");
        constexpr DWORD maximumStreams = 16;
        for (DWORD index = 0; index <= maximumStreams; ++index) {
            ComPtr<IMFMediaType> type;
            hr = reader->GetNativeMediaType(index, 0, type.GetAddressOf());
            if (hr == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(hr)) return nativeFailure(hr, "Read declared stream type");
            if (index == maximumStreams) return budget("Video container exceeds the stream-count budget");
            GUID major{};
            if (FAILED(hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major)))
                return nativeFailure(hr, "Read stream media kind");
            if (major != MFMediaType_Video) continue;
            UINT32 width{}, height{};
            if (FAILED(hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)))
                return nativeFailure(hr, "Read declared video dimensions");
            if (width > INT_MAX || height > INT_MAX) return budget("Video dimensions overflow the pixel layout");
            const auto layout = Core::pixelLayout(static_cast<int>(width), static_cast<int>(height), 4);
            if (!layout || layout->bytes > limits.maximumImageBytes)
                return budget("Declared video dimensions exceed the decoded-frame budget");
            videoStream = index;
            break;
        }
        if (!videoStream) return invalid("MP4 contains no declared video track");
        return {};
    }

    Failure select()
    {
        HRESULT hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(hr) || FAILED(hr = reader->SetStreamSelection(*videoStream, TRUE)))
            return nativeFailure(hr, "Select the declared media track");
        ComPtr<IMFMediaType> type;
        if (FAILED(hr = MFCreateMediaType(type.GetAddressOf())) ||
            FAILED(hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
            FAILED(hr = reader->SetCurrentMediaType(*videoStream, nullptr, type.Get())))
            return nativeFailure(hr, "Configure a real uncompressed decoder output");
        PROPVARIANT start{};
        start.vt = VT_I8;
        start.hVal.QuadPart = 0;
        hr = reader->SetCurrentPosition(GUID_NULL, start);
        return FAILED(hr) ? nativeFailure(hr, "Reset the selected track to its start") : Failure{};
    }
};

struct VideoDescription {
    ComPtr<IMFMediaType> type;
    QSize dimensions;
    Core::PixelLayout layout{};
};

Failure describeVideo(IMFSourceReader* reader, DWORD index, const Limits& limits, VideoDescription& description)
{
    HRESULT hr = reader->GetCurrentMediaType(index, description.type.GetAddressOf());
    GUID subtype{};
    UINT32 width{}, height{};
    if (FAILED(hr) || FAILED(hr = description.type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        subtype != MFVideoFormat_RGB32)
        return FAILED(hr) ? nativeFailure(hr, "Read decoded video type") : invalid("Decoder did not produce RGB32");
    if (FAILED(hr = MFGetAttributeSize(description.type.Get(), MF_MT_FRAME_SIZE, &width, &height)))
        return nativeFailure(hr, "Read decoded video dimensions");
    if (width > INT_MAX || height > INT_MAX) return budget("Decoded video dimensions overflow the pixel layout");
    const auto layout = Core::pixelLayout(static_cast<int>(width), static_cast<int>(height), 4);
    if (!layout || layout->bytes > limits.maximumImageBytes)
        return budget("Decoded video exceeds the pixel budget");
    description.dimensions = QSize(static_cast<int>(width), static_cast<int>(height));
    description.layout = *layout;
    return {};
}

Failure checkSample(IMFSample* sample, const Limits& limits, DWORD& length)
{
    UINT32 corrupted{};
    const auto flag = sample->GetUINT32(MFSampleExtension_FrameCorruption, &corrupted);
    if (FAILED(flag) && flag != MF_E_ATTRIBUTENOTFOUND) return nativeFailure(flag, "Read native corruption status");
    if (corrupted) return invalid("Native decoder reported a corrupted frame");
    const auto hr = sample->GetTotalLength(&length);
    if (FAILED(hr)) return nativeFailure(hr, "Read decoded sample length");
    if (!length) return invalid("Decoder produced an empty media sample");
    if (length > limits.maximumImageBytes) return budget("Decoded sample exceeds the byte budget");
    return {};
}

VideoThumbnailResult copyCover(IMFSample* sample, const VideoDescription& description,
    unsigned edge, const Limits& limits, std::stop_token stop)
{
    VideoThumbnailResult result;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(buffer.GetAddressOf());
    if (FAILED(hr)) { result.failure = nativeFailure(hr, "Access decoded video pixels"); return result; }
    ComPtr<IMF2DBuffer2> twoD;
    BYTE* scanline{};
    BYTE* start{};
    DWORD length{};
    LONG stride{};
    bool locked2D = false;
    if (SUCCEEDED(buffer.As(&twoD)) &&
        SUCCEEDED(twoD->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline, &stride, &start, &length)))
        locked2D = true;
    else {
        if (FAILED(hr = buffer->Lock(&start, nullptr, &length))) {
            result.failure = nativeFailure(hr, "Lock decoded video pixels"); return result;
        }
        UINT32 rawStride{};
        if (SUCCEEDED(description.type->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)))
            stride = static_cast<LONG>(rawStride);
        else
            hr = MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                static_cast<DWORD>(description.dimensions.width()), &stride);
        if (SUCCEEDED(hr) && start) {
            const auto distance = stride < 0 ? -static_cast<std::int64_t>(stride) : static_cast<std::int64_t>(stride);
            const auto first = stride < 0 ? static_cast<std::uint64_t>(description.dimensions.height() - 1) * distance : 0;
            if (first <= length) scanline = start + first;
        }
    }
    struct Unlock {
        IMFMediaBuffer* buffer;
        IMF2DBuffer2* twoD;
        bool locked2D;
        ~Unlock() { if (buffer) { if (locked2D) twoD->Unlock2D(); else buffer->Unlock(); } }
    } unlock{buffer.Get(), twoD.Get(), locked2D};
    const auto first = reinterpret_cast<std::uintptr_t>(scanline);
    const auto base = reinterpret_cast<std::uintptr_t>(start);
    if (FAILED(hr) || !scanline || !start || first < base || first - base > length || length > limits.maximumImageBytes ||
        !Core::validFramePlane({0, length, first - base, stride, description.layout.stride,
            static_cast<std::uint32_t>(description.dimensions.height())}, length)) {
        result.failure = invalid("Decoded video row range exceeds its locked allocation"); return result;
    }
    QImage image(description.dimensions, QImage::Format_RGB32);
    if (image.isNull()) { result.failure = budget("Cannot allocate the video cover"); return result; }
    for (int y = 0; y < image.height(); ++y) {
        if (stop.stop_requested()) { result.failure = cancelled(); return result; }
        std::memcpy(image.scanLine(y), scanline + static_cast<std::ptrdiff_t>(y) * stride, description.layout.stride);
        Core::makeOpaque({image.scanLine(y), description.layout.stride});
    }
    hr = locked2D ? twoD->Unlock2D() : buffer->Unlock();
    unlock.buffer = nullptr;
    if (FAILED(hr)) { result.failure = nativeFailure(hr, "Release decoded video pixels"); return result; }
    const auto target = image.size().boundedTo(QSize(static_cast<int>(edge), static_cast<int>(edge)));
    result.image = image.size() == target ? std::move(image) :
        image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (result.image.isNull()) result.failure = budget("Cannot scale the video cover");
    if (stop.stop_requested()) { result.image = {}; result.failure = cancelled(); }
    return result;
}

} // namespace

VideoThumbnailResult videoThumbnail(const QString& path, unsigned edge, const Limits& limits, std::stop_token stop)
{
    try {
        if (stop.stop_requested()) return {cancelled(), {}};
        if (!edge || edge > INT_MAX) return {budget("Invalid thumbnail edge budget"), {}};
        Apartment apartment;
        if (FAILED(apartment.result)) return {nativeFailure(apartment.result, "Initialize worker COM apartment"), {}};
        MediaSession session;
        if (FAILED(session.result)) return {nativeFailure(session.result, "Initialize Media Foundation"), {}};
        NativeReader input;
        if (auto error = input.open(path, limits, stop)) return {std::move(error), {}};
        if (auto error = input.select()) return {std::move(error), {}};
        for (unsigned attempt = 0; attempt < 4096; ++attempt) {
            if (stop.stop_requested()) return {cancelled(), {}};
            ComPtr<IMFSample> sample;
            DWORD flags{};
            auto hr = input.reader->ReadSample(*input.videoStream, 0, nullptr, &flags, nullptr, sample.GetAddressOf());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR))
                return {nativeFailure(FAILED(hr) ? hr : E_FAIL, "Decode native video cover"), {}};
            if (sample) {
                DWORD length{};
                if (auto error = checkSample(sample.Get(), limits, length)) return {std::move(error), {}};
                VideoDescription description;
                if (auto error = describeVideo(input.reader.Get(), *input.videoStream, limits, description))
                    return {std::move(error), {}};
                return copyCover(sample.Get(), description, edge, limits, stop);
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        return {invalid("Video contains no usable decoded cover"), {}};
    } catch (const std::bad_alloc&) { return {budget("Cannot allocate video thumbnail state"), {}}; }
}

} // namespace HuhuCapture::Media
