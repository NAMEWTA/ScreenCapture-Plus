#include "pch.h"
#include "services/MediaHistory.h"
#include "services/ImageExport.h"
#include "services/Lang.h"
#include "app/Brand.h"
#include "core/MediaFileName.h"
#include "core/PixelBuffer.h"
#include "platform/FileCommitter.h"
#include "platform/windows/DesktopFiles.h"

#include <shobjidl.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <format>
#include <limits>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace HuhuCapture::MediaHistory {
namespace {
constexpr std::size_t maximumImageBytes = 512ull * 1024 * 1024;

std::wstring timestamp()
{
    SYSTEMTIME value{};
    GetLocalTime(&value);
    return std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}", value.wYear, value.wMonth,
        value.wDay, value.wHour, value.wMinute, value.wSecond);
}

bool regularFile(const fs::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

fs::path prepareDirectory()
{
    const auto root = directory();
    if (root.empty()) return {};
    std::error_code error;
    fs::create_directories(root, error);
    if (error) return {};
    // Known Folder redirection may intentionally name a directory link. Resolve
    // this trusted application root before the commit adapter rejects unknown
    // reparse parents. Do not canonicalize a sender-provided media pathname.
    const auto resolved = fs::canonical(root, error);
    return error ? fs::path{} : resolved;
}

fs::path temporaryPath(const fs::path& root)
{
    std::error_code error;
    return Platform::createExclusiveTemporaryFile(root, error);
}

struct TemporaryFile {
    fs::path path;
    ~TemporaryFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
};

fs::path commitFile(const fs::path& temporary, const std::wstring& extension)
{
    const auto stamp = timestamp();
    for (std::uint32_t sequence = 0; sequence < 100000; ++sequence) {
        const auto name = Core::makeMediaFileName(stamp, extension, sequence);
        if (name.empty()) return {};
        const auto destination = temporary.parent_path() / name;
        const auto result = Platform::commitFileNoReplace(temporary, destination);
        if (result.durable()) return result.destination;
        if (result.committed()) {
            // A final file must survive even when persistence is uncertain.
            // Keep the legacy empty-result failure contract, while reporting
            // the actual surviving path instead of silently retrying it.
            reportError(nullptr, result.destination.wstring());
            return {};
        }
        if (result.error != std::errc::file_exists) return {};
    }
    return {};
}

bool readImage(const fs::path& path, std::vector<BYTE>& pixels, UINT& width, UINT& height)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(frame->GetSize(&width, &height))) return false;
    if (width > INT_MAX || height > INT_MAX) return false;
    const auto layout = Core::pixelLayout(static_cast<int>(width), static_cast<int>(height), 4);
    if (!layout || layout->bytes > maximumImageBytes) return false;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    pixels.resize(layout->bytes);
    return SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(layout->stride),
        static_cast<UINT>(pixels.size()), pixels.data()));
}

HBITMAP bitmapFromWic(IWICImagingFactory* factory, IWICBitmapSource* source, unsigned edge)
{
    UINT width = 0, height = 0;
    if (FAILED(source->GetSize(&width, &height)) || !width || !height) return nullptr;
    const auto ratio = (std::min)(1.0, static_cast<double>(edge) / (std::max)(width, height));
    const auto targetW = (std::max)(1u, static_cast<UINT>(width * ratio));
    const auto targetH = (std::max)(1u, static_cast<UINT>(height * ratio));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(source, targetW, targetH, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(targetW);
    info.bmiHeader.biHeight = -static_cast<LONG>(targetH);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* data = nullptr;
    auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    if (!bitmap) return nullptr;
    if (FAILED(converter->CopyPixels(nullptr, targetW * 4, targetW * targetH * 4, static_cast<BYTE*>(data)))) {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}

HBITMAP imageThumbnail(const fs::path& path, unsigned edge)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return nullptr;
    return bitmapFromWic(factory.Get(), frame.Get(), edge);
}

// Native fallback for MP4 when Explorer has not cached a cover. No decoder or
// ffmpeg is bundled: Windows' installed codecs determine which files can open.
HBITMAP videoThumbnail(const fs::path& path, unsigned edge)
{
    struct MediaSession {
        HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        ~MediaSession() { if (SUCCEEDED(result)) MFShutdown(); }
    } session;
    if (FAILED(session.result)) return nullptr;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1)) ||
        FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
        FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), reader.GetAddressOf())) ||
        FAILED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)) ||
        FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) ||
        FAILED(MFCreateMediaType(type.GetAddressOf())) ||
        FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()))) return nullptr;
    ComPtr<IMFSample> sample;
    for (unsigned attempts = 0; attempts < 32; ++attempts) {
        DWORD flags = 0;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                &flags, nullptr, sample.ReleaseAndGetAddressOf())) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) return nullptr;
        if (sample) break;
    }
    if (!sample) return nullptr;
    // A source reader can change its output type while producing the first
    // sample. Validate the current type after reading, before touching pixels.
    type.Reset();
    GUID subtype{};
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, type.GetAddressOf())) ||
        FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_RGB32) return nullptr;
    UINT width = 0, height = 0;
    if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) || width > INT_MAX || height > INT_MAX) return nullptr;
    const auto layout = Core::pixelLayout(static_cast<int>(width), static_cast<int>(height), 4);
    if (!layout || layout->bytes > maximumImageBytes) return nullptr;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) return nullptr;
    std::vector<BYTE> pixels(layout->bytes);
    ComPtr<IMF2DBuffer> twoD;
    BYTE* source = nullptr;
    LONG stride = 0;
    if (SUCCEEDED(buffer.As(&twoD)) && SUCCEEDED(twoD->Lock2D(&source, &stride))) {
        if (static_cast<std::uint64_t>(std::abs(static_cast<long long>(stride))) < layout->stride) {
            twoD->Unlock2D();
            return nullptr;
        }
        for (UINT row = 0; row < height; ++row)
            memcpy(pixels.data() + row * layout->stride, source + static_cast<std::ptrdiff_t>(row) * stride, layout->stride);
        twoD->Unlock2D();
    } else {
        DWORD length = 0;
        if (FAILED(buffer->Lock(&source, nullptr, &length))) return nullptr;
        UINT32 rawStride = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) stride = static_cast<LONG>(rawStride);
        else MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride);
        const auto absoluteStride = static_cast<std::uint64_t>(std::abs(static_cast<long long>(stride)));
        if (absoluteStride < layout->stride || absoluteStride * height > length) {
            buffer->Unlock();
            return nullptr;
        }
        if (stride < 0) source += (height - 1) * absoluteStride;
        for (UINT row = 0; row < height; ++row)
            memcpy(pixels.data() + row * layout->stride, source + static_cast<std::ptrdiff_t>(row) * stride, layout->stride);
        buffer->Unlock();
    }
    for (std::size_t index = 3; index < pixels.size(); index += 4) pixels[index] = 255;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(layout->stride), static_cast<UINT>(pixels.size()), pixels.data(), bitmap.GetAddressOf()))) return nullptr;
    return bitmapFromWic(factory.Get(), bitmap.Get(), edge);
}
} // namespace

fs::path directory()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) return {};
    const auto result = fs::path(path) / L"huhucapture";
    CoTaskMemFree(path);
    return result;
}

std::vector<Entry> list(std::error_code* error)
{
    std::vector<Entry> result;
    std::error_code ec;
    const auto root = directory();
    if (root.empty()) ec = std::make_error_code(std::errc::no_such_file_or_directory);
    else if (fs::exists(root, ec)) {
        fs::directory_iterator current(root, ec), end;
        for (; !ec && current != end; current.increment(ec)) {
            const auto name = current->path().filename().wstring();
            const auto parsed = Core::parseMediaFileName(name);
            if (!parsed || !regularFile(current->path())) continue;
            std::error_code itemError;
            const auto bytes = current->file_size(itemError);
            if (!itemError && bytes) result.push_back({current->path(), name, bytes, parsed->extension != L"png"});
        }
    }
    std::sort(result.begin(), result.end(), [](const Entry& left, const Entry& right) {
        return Core::newerMediaFileName(left.name, right.name);
    });
    if (error) *error = ec;
    return result;
}

std::wstring suggestedName(const std::wstring& extension)
{
    const auto root = directory();
    const auto stamp = timestamp();
    for (std::uint32_t sequence = 0; sequence < 100000; ++sequence) {
        const auto name = Core::makeMediaFileName(stamp, extension, sequence);
        if (name.empty()) return {};
        if (root.empty() || GetFileAttributesW((root / name).c_str()) == INVALID_FILE_ATTRIBUTES) return name;
    }
    return {};
}

fs::path archiveImage(int width, int height, const BYTE* pixels)
{
    const auto root = prepareDirectory();
    if (root.empty()) return {};
    TemporaryFile temporary{temporaryPath(root)};
    if (temporary.path.empty() || !ImageExport::saveToFile(temporary.path.wstring(), width, height, pixels)) return {};
    return commitFile(temporary.path, L"png");
}

fs::path archiveFile(const fs::path& source)
{
    if (!regularFile(source)) return {};
    std::error_code error;
    if (fs::file_size(source, error) == 0 || error) return {};
    const auto root = prepareDirectory();
    if (root.empty()) return {};
    if (Core::parseMediaFileName(source.filename().wstring()) && fs::equivalent(source.parent_path(), root, error) && !error)
        return source;
    auto extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".png" && extension != L".gif" && extension != L".mp4") return {};
    TemporaryFile temporary{temporaryPath(root)};
    // This operation already exclusively claimed the empty staging file.
    // Only that owned file may be replaced; the final commit never replaces.
    if (temporary.path.empty() || !CopyFileW(source.c_str(), temporary.path.c_str(), FALSE)) return {};
    // CopyFile propagates read-only attributes. The completed original remains
    // unchanged, but our own staging file needs write access for a checked flush.
    const auto attributes = GetFileAttributesW(temporary.path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        ((attributes & FILE_ATTRIBUTE_READONLY) &&
            !SetFileAttributesW(temporary.path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))) return {};
    return commitFile(temporary.path, extension.substr(1));
}

void reportError(HWND owner, const std::wstring& detail)
{
    auto text = Lang::get(L"clipboard.saveFailed");
    if (!detail.empty()) text += L"\n\n" + detail;
    MessageBoxW(owner, text.c_str(), HuhuCapture::Brand::name.data(), MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

bool copyImage(HWND owner, int width, int height, const BYTE* pixels)
{
    const auto path = archiveImage(width, height, pixels);
    if (path.empty()) { reportError(owner, directory().wstring()); return false; }
    if (!ImageExport::saveToClipboard(width, height, pixels, owner)) {
        MessageBoxW(owner, Lang::get(L"clipboard.copyFailed").c_str(), HuhuCapture::Brand::name.data(), MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }
    return true;
}

bool saveImage(HWND owner, const std::wstring& destination, int width, int height, const BYTE* pixels)
{
    if (!ImageExport::saveToFile(destination, width, height, pixels)) { reportError(owner, destination); return false; }
    if (archiveFile(destination).empty()) { reportError(owner, destination); return false; }
    return true;
}

bool copy(HWND owner, const Entry& entry)
{
    if (!regularFile(entry.path)) return false;
    if (entry.video) return ImageExport::addFileToClipboard(entry.path.wstring(), owner);
    try {
        std::vector<BYTE> pixels;
        UINT width = 0, height = 0;
        return readImage(entry.path, pixels, width, height) &&
            ImageExport::saveToClipboard(static_cast<int>(width), static_cast<int>(height), pixels.data(), owner);
    } catch (...) { return false; }
}

bool reveal(HWND owner, const fs::path& path)
{
    if (path.empty()) return false;
    std::error_code error;
    if (path == directory()) fs::create_directories(path, error);
    if (error) return false;
    if (fs::is_directory(path, error) && !error)
        return Platform::Windows::openDesktopDirectory(path, owner).completed();
    return Platform::Windows::revealDesktopFile(path).completed();
}

bool clear(HWND owner)
{
    std::error_code error;
    const auto entries = list(&error);
    if (error) return false;
    std::vector<fs::path> files;
    files.reserve(entries.size());
    for (const auto& entry : entries) if (regularFile(entry.path)) files.push_back(entry.path);
    return Platform::Windows::recycleDesktopFiles(files, owner,
        Platform::Windows::RecyclePolicy::AllowSystemPermanentConfirmation, regularFile).completed();
}

HBITMAP loadThumbnail(const fs::path& path, unsigned maximumEdge)
{
    if (!regularFile(path)) return nullptr;
    const auto edge = std::clamp(maximumEdge, 32u, 512u);
    try {
        if (path.extension() != L".mp4") return imageThumbnail(path, edge);
        ComPtr<IShellItemImageFactory> factory;
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(factory.GetAddressOf()))) &&
            SUCCEEDED(factory->GetImage({static_cast<LONG>(edge), static_cast<LONG>(edge)}, SIIGBF_THUMBNAILONLY, &bitmap))) return bitmap;
        if (bitmap) DeleteObject(bitmap);
        return videoThumbnail(path, edge);
    } catch (...) { return nullptr; }
}

} // namespace HuhuCapture::MediaHistory
