// Extracted from this project's mature ImageExport implementation. Keep the
// explicit PNG / alpha DIBV5 / conservative 24-bit bottom-up DIB compatibility.
#include "platform/windows/DesktopClipboard.h"
#include "platform/windows/ComApartment.h"
#include "core/PixelBuffer.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace HuhuCapture::Platform::Windows {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto maximumImageBytes = std::size_t{512} * 1024 * 1024;

bool validPixels(int width, int height, std::size_t stride, std::span<const std::uint8_t> pixels)
{
    const auto layout = Core::pixelLayout(width, height, 4);
    return layout && layout->bytes <= maximumImageBytes && stride >= layout->stride &&
        stride <= (std::numeric_limits<UINT>::max)() / static_cast<unsigned>(height) &&
        pixels.size() >= stride * static_cast<unsigned>(height);
}

class Locked final {
public:
    explicit Locked(HGLOBAL handle) : handle_(handle), address_(handle ? GlobalLock(handle) : nullptr) {}
    ~Locked() { if (address_) GlobalUnlock(handle_); }
    void* get() const noexcept { return address_; }
private:
    HGLOBAL handle_;
    void* address_;
};

ClipboardPreparation failed(ClipboardPreparation result, ClipboardError error, std::int64_t nativeCode)
{
    result.result.error = error; result.result.nativeCode = nativeCode;
    return result;
}
}

ClipboardPayload::~ClipboardPayload() { clear(); }
ClipboardPayload::ClipboardPayload(ClipboardPayload&& other) noexcept { *this = std::move(other); }
ClipboardPayload& ClipboardPayload::operator=(ClipboardPayload&& other) noexcept
{
    if (this != &other) {
        clear();
        png_ = std::exchange(other.png_, nullptr); dibV5_ = std::exchange(other.dibV5_, nullptr);
        dib_ = std::exchange(other.dib_, nullptr); files_ = std::exchange(other.files_, nullptr);
        text_ = std::exchange(other.text_, nullptr);
        pngFormat_ = std::exchange(other.pngFormat_, 0); required_ = std::exchange(other.required_, 0);
        consumed_ = std::exchange(other.consumed_, false);
    }
    return *this;
}
void ClipboardPayload::clear() noexcept
{
    if (png_) GlobalFree(std::exchange(png_, nullptr));
    if (dibV5_) GlobalFree(std::exchange(dibV5_, nullptr));
    if (dib_) GlobalFree(std::exchange(dib_, nullptr));
    if (files_) GlobalFree(std::exchange(files_, nullptr));
    if (text_) GlobalFree(std::exchange(text_, nullptr));
    required_ = 0; consumed_ = false;
}

HRESULT encodeDesktopPng(IStream* stream, int width, int height, std::size_t stride,
    std::span<const std::uint8_t> pixels)
{
    if (!stream || !validPixels(width, height, stride, pixels)) return E_INVALIDARG;
    const ComApartment com(COINIT_MULTITHREADED);
    if (FAILED(com.result()) && com.result() != RPC_E_CHANGED_MODE) return com.result();
    ComPtr<IWICImagingFactory> factory;
    auto result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) return result;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) ||
        FAILED(result = encoder->Initialize(stream, WICBitmapEncoderNoCache))) return result;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(result = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) ||
        FAILED(result = frame->Initialize(nullptr)) ||
        FAILED(result = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) return result;
    auto format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(result = frame->SetPixelFormat(&format))) return result;
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (FAILED(result = frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride),
            static_cast<UINT>(stride * static_cast<unsigned>(height)), const_cast<BYTE*>(pixels.data()))) ||
        FAILED(result = frame->Commit())) return result;
    return encoder->Commit();
}

ClipboardPreparation prepareClipboardImage(int width, int height, std::size_t stride,
    std::span<const std::uint8_t> pixels)
{
    ClipboardPreparation output;
    if (!validPixels(width, height, stride, pixels)) return failed(std::move(output), ClipboardError::InvalidInput, E_INVALIDARG);
    const auto layout = *Core::pixelLayout(width, height, 4);
    const auto imageBytes = static_cast<DWORD>(layout.bytes);
    auto& payload = output.payload;
    payload.pngFormat_ = RegisterClipboardFormatW(L"PNG");
    if (!payload.pngFormat_) return failed(std::move(output), ClipboardError::Publication, GetLastError());
    ComPtr<IStream> stream;
    auto result = CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (FAILED(result)) return failed(std::move(output), ClipboardError::Allocation, result);
    if (FAILED(result = encodeDesktopPng(stream.Get(), width, height, stride, pixels)))
        return failed(std::move(output), ClipboardError::Encoding, result);
    STATSTG information{};
    if (FAILED(result = stream->Stat(&information, STATFLAG_NONAME)))
        return failed(std::move(output), ClipboardError::Encoding, result);
    if (!information.cbSize.QuadPart || information.cbSize.QuadPart > (std::numeric_limits<SIZE_T>::max)())
        return failed(std::move(output), ClipboardError::Allocation, E_OUTOFMEMORY);
    HGLOBAL source{};
    if (FAILED(result = GetHGlobalFromStream(stream.Get(), &source)))
        return failed(std::move(output), ClipboardError::Encoding, result);
    const auto pngBytes = static_cast<SIZE_T>(information.cbSize.QuadPart);
    payload.png_ = GlobalAlloc(GMEM_MOVEABLE, pngBytes);
    if (!payload.png_) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
    {
        const Locked from(source), to(payload.png_);
        if (!from.get() || !to.get()) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
        std::memcpy(to.get(), from.get(), pngBytes);
    }
    payload.dibV5_ = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + layout.bytes);
    if (!payload.dibV5_) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
    {
        const Locked memory(payload.dibV5_);
        if (!memory.get()) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
        auto* header = static_cast<BITMAPV5HEADER*>(memory.get()); *header = {};
        header->bV5Size = sizeof(*header); header->bV5Width = width; header->bV5Height = -height;
        header->bV5Planes = 1; header->bV5BitCount = 32; header->bV5Compression = BI_BITFIELDS;
        header->bV5SizeImage = imageBytes; header->bV5RedMask = 0x00ff0000;
        header->bV5GreenMask = 0x0000ff00; header->bV5BlueMask = 0x000000ff; header->bV5AlphaMask = 0xff000000;
        header->bV5CSType = LCS_sRGB; header->bV5Intent = LCS_GM_GRAPHICS;
        auto* destination = reinterpret_cast<BYTE*>(header + 1);
        for (int row = 0; row < height; ++row)
            std::memcpy(destination + static_cast<std::size_t>(row) * layout.stride,
                pixels.data() + static_cast<std::size_t>(row) * stride, layout.stride);
    }
    const auto dibStride = (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t{3};
    const auto dibBytes = dibStride * static_cast<unsigned>(height);
    payload.dib_ = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPINFOHEADER) + dibBytes);
    if (!payload.dib_) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
    {
        const Locked memory(payload.dib_);
        if (!memory.get()) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
        auto* header = static_cast<BITMAPINFOHEADER*>(memory.get()); *header = {};
        header->biSize = sizeof(*header); header->biWidth = width; header->biHeight = height;
        header->biPlanes = 1; header->biBitCount = 24; header->biCompression = BI_RGB;
        header->biSizeImage = static_cast<DWORD>(dibBytes);
        auto* destination = reinterpret_cast<BYTE*>(header + 1);
        for (int row = 0; row < height; ++row) {
            auto* to = destination + static_cast<std::size_t>(height - 1 - row) * dibStride;
            const auto* from = pixels.data() + static_cast<std::size_t>(row) * stride;
            for (int column = 0; column < width; ++column, to += 3, from += 4) std::memcpy(to, from, 3);
        }
    }
    output.result.requiredFormats = payload.required_ = ClipboardPng | ClipboardDibV5 | ClipboardDib;
    return output;
}

ClipboardPreparation prepareClipboardFile(std::wstring_view path)
{
    ClipboardPreparation output;
    if (path.empty() || path.find(L'\0') != path.npos ||
        path.size() > ((std::numeric_limits<SIZE_T>::max)() - sizeof(DROPFILES)) / sizeof(wchar_t) - 2)
        return failed(std::move(output), ClipboardError::InvalidInput, E_INVALIDARG);
    const std::wstring owned(path);
    const auto attributes = GetFileAttributesW(owned.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return failed(std::move(output), ClipboardError::InvalidInput, attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_INVALID_DATA);
    const auto bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    output.payload.files_ = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!output.payload.files_) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
    {
        const Locked memory(output.payload.files_);
        if (!memory.get()) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
        auto* drop = static_cast<DROPFILES*>(memory.get()); drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
        std::memcpy(drop + 1, path.data(), path.size() * sizeof(wchar_t));
    }
    output.result.requiredFormats = output.payload.required_ = ClipboardFiles;
    return output;
}

ClipboardPreparation prepareClipboardText(std::wstring_view text, std::size_t maximumBytes)
{
    ClipboardPreparation output;
    if (maximumBytes < sizeof(wchar_t)) return failed(std::move(output), ClipboardError::InvalidInput, E_INVALIDARG);
    const auto maximumCharacters = maximumBytes / sizeof(wchar_t);
    std::size_t characters = 1; // The explicit UTF-16 terminator is part of the budget.
    bool previousCr = false;
    for (const wchar_t character : text) {
        const auto added = std::size_t{1} + (character == L'\n' && !previousCr ? 1u : 0u);
        if (!character || added > maximumCharacters - characters)
            return failed(std::move(output), ClipboardError::InvalidInput, E_INVALIDARG);
        characters += added;
        previousCr = character == L'\r';
    }
    output.payload.text_ = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, characters * sizeof(wchar_t));
    if (!output.payload.text_) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
    {
        const Locked memory(output.payload.text_);
        if (!memory.get()) return failed(std::move(output), ClipboardError::Allocation, GetLastError());
        auto* destination = static_cast<wchar_t*>(memory.get());
        previousCr = false;
        for (const wchar_t character : text) {
            if (character == L'\n' && !previousCr) *destination++ = L'\r';
            *destination++ = character;
            previousCr = character == L'\r';
        }
    }
    output.result.requiredFormats = output.payload.required_ = ClipboardText;
    return output;
}

ClipboardResult publishClipboard(ClipboardPayload& payload, HWND owner)
{
    ClipboardResult result; result.requiredFormats = payload.required_;
    if (!owner || !IsWindow(owner) || !payload.ready()) { result.error = ClipboardError::InvalidInput; return result; }
    if (!OpenClipboard(owner)) { result.error = ClipboardError::Busy; result.nativeCode = GetLastError(); return result; }
    if (!EmptyClipboard()) {
        result.error = ClipboardError::Publication; result.nativeCode = GetLastError(); CloseClipboard(); return result;
    }
    payload.consumed_ = true;
    const auto publish = [&](UINT format, HGLOBAL& memory, unsigned flag) {
        if (!memory) return;
        if (SetClipboardData(format, memory)) { memory = nullptr; result.publishedFormats |= flag; }
        else if (!result.nativeCode) result.nativeCode = GetLastError();
    };
    publish(CF_DIBV5, payload.dibV5_, ClipboardDibV5); publish(CF_DIB, payload.dib_, ClipboardDib);
    publish(payload.pngFormat_, payload.png_, ClipboardPng); publish(CF_HDROP, payload.files_, ClipboardFiles);
    publish(CF_UNICODETEXT, payload.text_, ClipboardText);
    if (result.publishedFormats != result.requiredFormats)
        result.error = result.publishedFormats ? ClipboardError::Partial : ClipboardError::Publication;
    if (!CloseClipboard() && result.error == ClipboardError::None) {
        result.error = ClipboardError::Publication; result.nativeCode = GetLastError();
    }
    return result;
}

} // namespace HuhuCapture::Platform::Windows
