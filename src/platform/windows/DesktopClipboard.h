#pragma once

#include <windows.h>
#include <objidl.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace HuhuCapture::Platform::Windows {

enum class ClipboardError { None, InvalidInput, Allocation, Encoding, Busy, Publication, Partial };
enum ClipboardFormat : unsigned { ClipboardPng = 1, ClipboardDibV5 = 2, ClipboardDib = 4, ClipboardFiles = 8 };

struct ClipboardResult {
    ClipboardError error{ClipboardError::None};
    std::int64_t nativeCode{};
    unsigned requiredFormats{}, publishedFormats{};
    bool completed() const noexcept
    { return error == ClipboardError::None && requiredFormats && publishedFormats == requiredFormats; }
};

struct ClipboardPreparation;

// Prepared away from the GUI thread. Each handle is released to the system only
// after its successful SetClipboardData; every other handle remains RAII-owned.
// No Qt, Ling, application PCH, archive policy, or window ownership lives here.
class ClipboardPayload final {
public:
    ClipboardPayload() noexcept = default;
    ~ClipboardPayload();
    ClipboardPayload(ClipboardPayload&& other) noexcept;
    ClipboardPayload& operator=(ClipboardPayload&& other) noexcept;
    ClipboardPayload(const ClipboardPayload&) = delete;
    ClipboardPayload& operator=(const ClipboardPayload&) = delete;
    bool ready() const noexcept { return required_ != 0 && !consumed_; }
private:
    void clear() noexcept;
    HGLOBAL png_{}, dibV5_{}, dib_{}, files_{};
    UINT pngFormat_{};
    unsigned required_{};
    bool consumed_{};
    friend struct ClipboardPreparation;
    friend ClipboardPreparation prepareClipboardImage(int, int, std::size_t, std::span<const std::uint8_t>);
    friend ClipboardPreparation prepareClipboardFile(std::wstring_view);
    friend ClipboardResult publishClipboard(ClipboardPayload&, HWND);
};

struct ClipboardPreparation { ClipboardResult result; ClipboardPayload payload; };

// The existing WIC encoder is shared by old file export and prepared clipboard
// output. Pixels are straight-alpha, top-down BGRA, with a checked row stride.
HRESULT encodeDesktopPng(IStream* stream, int width, int height, std::size_t stride,
    std::span<const std::uint8_t> pixels);
ClipboardPreparation prepareClipboardImage(int width, int height, std::size_t stride,
    std::span<const std::uint8_t> pixels);
ClipboardPreparation prepareClipboardFile(std::wstring_view path);
// Call on the GUI thread with a live owner HWND. Busy does not empty or replace
// the old clipboard. Partial publication is observable and is never full success.
ClipboardResult publishClipboard(ClipboardPayload& payload, HWND owner);

} // namespace HuhuCapture::Platform::Windows
