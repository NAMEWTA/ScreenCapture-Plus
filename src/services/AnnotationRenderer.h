#pragma once

#include "core/Annotation.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <span>
#include <string>
#include <vector>

// This service deliberately has no Ling, window, settings or application-state
// dependency. The editor and the native rendering regression harness use the
// same DirectWrite measurement and Direct2D rasterization implementation.
namespace AnnotationRenderer {

struct Options : HuhuCapture::Core::AnnotationLayoutOptions {
    std::wstring title;
    std::wstring emptyNoteText;
};

struct PreparedNote {
    std::uint64_t number{};
    HuhuCapture::Core::AnnotationColor color{};
    std::wstring text;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> numberLayout;
    float textHeight{};
    float textInsetX{}, textInsetY{};
};

struct Plan {
    HuhuCapture::Core::AnnotationLayout layout;
    std::vector<PreparedNote> notes;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> titleLayout;
    float titleInsetX{}, titleInsetY{};
};

// Options.maximumDimension should be context->GetMaximumBitmapSize(). Plans
// retain complete strings and measured layouts; no text trimming is enabled.
// Failures leave the previous plan unchanged. Allocation/size failures are
// reported as HRESULTs, so callers can keep the editor open and show an error.
HRESULT makePlan(IDWriteFactory* factory, IDWriteTextFormat* format,
    D2D1_SIZE_U imageSize,
    std::span<const HuhuCapture::Core::AnnotationNote> notes,
    const Options& options, Plan& output) noexcept;

// Paint only the note rail, at its planned image-relative coordinates. Call
// between BeginDraw/EndDraw; the caller may transform the whole plan for preview.
// This does not clear, change the target/transform, or touch the left image area.
HRESULT paintNotes(ID2D1DeviceContext* context, const Plan& plan) noexcept;

// Compose ONE image. Source must be a same-device BGRA/PREMULTIPLIED bitmap of
// layout.imageWidth x layout.imageHeight, already containing active pin shapes.
// The original pixels are copied exactly into the top-left rectangle, without
// resampling or alpha blending. Extra canvas area is opaque white.
// Call outside a drawing session. Owns BeginDraw/EndDraw and restores the
// caller's target, DPI, transform, unit mode and antialiasing/primitive modes.
// The resulting bitmap has 96-DPI pixel coordinates and the plan's actual size.
HRESULT render(ID2D1DeviceContext* context, ID2D1Bitmap1* source,
    const Plan& plan, Microsoft::WRL::ComPtr<ID2D1Bitmap1>& output) noexcept;

} // namespace AnnotationRenderer
