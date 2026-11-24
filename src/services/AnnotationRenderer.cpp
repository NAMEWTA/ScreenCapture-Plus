#include "services/AnnotationRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

HRESULT createText(IDWriteFactory* factory, IDWriteTextFormat* format,
    const std::wstring& text, float size, float width,
    ComPtr<IDWriteTextLayout>& layout)
{
    if (text.size() > (std::numeric_limits<UINT32>::max)() ||
        !std::isfinite(size) || size <= 0 || !std::isfinite(width) || width <= 0)
        return E_INVALIDARG;
    const auto length = static_cast<UINT32>(text.size());
    HRESULT result = factory->CreateTextLayout(text.data(), length, format,
        width, (std::numeric_limits<float>::max)(), layout.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    if (FAILED(result = layout->SetFontSize(size, {0, length}))) return result;
    if (FAILED(result = layout->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, {0, length}))) return result;
    if (FAILED(result = layout->SetFontStyle(DWRITE_FONT_STYLE_NORMAL, {0, length}))) return result;
    if (FAILED(result = layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING))) return result;
    if (FAILED(result = layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR))) return result;
    if (FAILED(result = layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP))) return result;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    return layout->SetTrimming(&trimming, nullptr);
}

// Include ink overhang in the measured row. No CLIP drawing option is used, but
// these margins also keep fallback-font glyphs from touching another row/badge.
HRESULT measureWrapped(IDWriteFactory* factory, IDWriteTextFormat* format,
    const std::wstring& text, float size, float width,
    ComPtr<IDWriteTextLayout>& layout, float& height, float& insetX, float& insetY)
{
    float left = 4.0f, right = 4.0f;
    HRESULT result = createText(factory, format, text, size, width - left - right, layout);
    if (FAILED(result)) return result;
    for (;;) {
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(result = layout->GetMetrics(&metrics))) return result;
        if (!std::isfinite(metrics.height) || metrics.height < 0) return E_FAIL;
        if (FAILED(result = layout->SetMaxHeight((std::max)(1.0f, metrics.height)))) return result;
        DWRITE_OVERHANG_METRICS overhang{};
        if (FAILED(result = layout->GetOverhangMetrics(&overhang))) return result;
        const float nextLeft = (std::max)(left, overhang.left + 2.0f);
        const float nextRight = (std::max)(right, overhang.right + 2.0f);
        if (nextLeft > left || nextRight > right) {
            left = nextLeft;
            right = nextRight;
            const float contentWidth = width - left - right;
            if (!std::isfinite(contentWidth) || contentWidth <= 0) return E_FAIL;
            if (FAILED(result = layout->SetMaxWidth(contentWidth))) return result;
            continue;
        }
        insetX = left;
        insetY = 4.0f + (std::max)(0.0f, overhang.top);
        height = metrics.height + insetY + 4.0f + (std::max)(0.0f, overhang.bottom);
        return std::isfinite(height) ? S_OK : E_FAIL;
    }
}

struct ContextState {
    explicit ContextState(ID2D1DeviceContext* value) : context(value)
    {
        context->GetTarget(target.GetAddressOf());
        context->GetTransform(&transform);
        context->GetDpi(&dpiX, &dpiY);
        unitMode = context->GetUnitMode();
        antialias = context->GetAntialiasMode();
        textAntialias = context->GetTextAntialiasMode();
        primitiveBlend = context->GetPrimitiveBlend();
    }
    ~ContextState()
    {
        context->SetTarget(target.Get());
        context->SetTransform(transform);
        context->SetDpi(dpiX, dpiY);
        context->SetUnitMode(unitMode);
        context->SetAntialiasMode(antialias);
        context->SetTextAntialiasMode(textAntialias);
        context->SetPrimitiveBlend(primitiveBlend);
    }
    ID2D1DeviceContext* context;
    ComPtr<ID2D1Image> target;
    D2D1_MATRIX_3X2_F transform{};
    float dpiX{}, dpiY{};
    D2D1_UNIT_MODE unitMode{};
    D2D1_ANTIALIAS_MODE antialias{};
    D2D1_TEXT_ANTIALIAS_MODE textAntialias{};
    D2D1_PRIMITIVE_BLEND primitiveBlend{};
};

} // namespace

HRESULT AnnotationRenderer::makePlan(IDWriteFactory* factory, IDWriteTextFormat* format,
    D2D1_SIZE_U imageSize, std::span<const HuhuCapture::Core::AnnotationNote> notes,
    const Options& options, Plan& output) noexcept
{
    if (!factory || !format) return E_POINTER;
    try {
        Plan candidate;
        auto geometry = static_cast<HuhuCapture::Core::AnnotationLayoutOptions>(options);
        if (notes.empty()) {
            const auto layout = HuhuCapture::Core::layoutAnnotations(imageSize.width, imageSize.height,
                0, geometry, [](std::size_t, float, float) -> std::optional<float> { return 0.0f; });
            if (!layout) return E_INVALIDARG;
            candidate.layout = *layout;
            output = std::move(candidate);
            return S_OK;
        }
        if (geometry.maximumDimension == 0 || geometry.panelWidth > geometry.maximumDimension ||
            !std::isfinite(geometry.preferredFontSize) || geometry.preferredFontSize <= 0 ||
            !std::isfinite(geometry.padding) || geometry.padding < 0)
            return E_INVALIDARG;
        candidate.notes.reserve(notes.size());
        HRESULT result = S_OK;
        // Measure identifiers once. Large ordinals grow the badges instead of
        // truncating digits. Compact plans later scale both badge and numeral
        // together, retaining a readable 12-pixel numeral floor.
        for (const auto& note : notes) {
            if (note.number == 0) return E_INVALIDARG;
            PreparedNote prepared;
            prepared.number = note.number;
            prepared.color = HuhuCapture::Core::annotationColor(note.number);
            prepared.text = note.note.empty() ? options.emptyNoteText : note.note;
            if (prepared.text.empty()) prepared.text = L"\u2014";
            const auto numberText = std::to_wstring(note.number);
            result = createText(factory, format, numberText, 16.0f,
                (std::numeric_limits<float>::max)(), prepared.numberLayout);
            if (FAILED(result)) return result;
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(result = prepared.numberLayout->GetMetrics(&metrics))) return result;
            geometry.badgeDiameter = (std::max)(geometry.badgeDiameter,
                std::ceil((std::max)(metrics.width, metrics.height) + 12.0f));
            candidate.notes.push_back(std::move(prepared));
        }
        geometry.headerHeight = geometry.padding;
        if (!options.title.empty()) {
            float titleHeight = 0;
            result = measureWrapped(factory, format, options.title,
                (std::max)(18.0f, geometry.preferredFontSize),
                static_cast<float>(geometry.panelWidth) - geometry.padding * 2.0f,
                candidate.titleLayout, titleHeight, candidate.titleInsetX, candidate.titleInsetY);
            if (FAILED(result)) return result;
            geometry.headerHeight = geometry.padding * 2.0f + titleHeight;
        }

        const auto layout = HuhuCapture::Core::layoutAnnotations(imageSize.width, imageSize.height,
            candidate.notes.size(), geometry,
            [&](std::size_t index, float size, float width) -> std::optional<float> {
                auto& note = candidate.notes[index];
                result = measureWrapped(factory, format, note.text, size, width,
                    note.textLayout, note.textHeight, note.textInsetX, note.textInsetY);
                if (FAILED(result)) return std::nullopt;
                return note.textHeight;
            });
        if (!layout) return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        candidate.layout = *layout;
        for (auto& note : candidate.notes) {
            auto* badge = note.numberLayout.Get();
            const auto numberLength = static_cast<UINT32>(std::to_wstring(note.number).size());
            if (FAILED(result = badge->SetFontSize(layout->badgeFontSize, {0, numberLength}))) return result;
            if (FAILED(result = badge->SetMaxWidth(layout->badgeDiameter))) return result;
            if (FAILED(result = badge->SetMaxHeight(layout->badgeDiameter))) return result;
            if (FAILED(result = badge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER))) return result;
            if (FAILED(result = badge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER))) return result;
        }
        output = std::move(candidate);
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (const std::length_error&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT AnnotationRenderer::paintNotes(ID2D1DeviceContext* context, const Plan& plan) noexcept
{
    if (!context) return E_POINTER;
    if (plan.notes.empty()) return S_OK;
    if (plan.layout.rows.size() != plan.notes.size()) return E_INVALIDARG;
    ComPtr<ID2D1SolidColorBrush> brush;
    HRESULT result = context->CreateSolidColorBrush(D2D1::ColorF(0xF8FAFC), brush.GetAddressOf());
    if (FAILED(result)) return result;
    const auto& layout = plan.layout;
    context->FillRectangle(D2D1::RectF(static_cast<float>(layout.imageWidth), 0,
        static_cast<float>(layout.width), static_cast<float>(layout.height)), brush.Get());
    for (std::uint32_t column = 0; column < layout.columns; ++column) {
        const float left = static_cast<float>(layout.imageWidth + column * layout.panelWidth);
        brush->SetColor(D2D1::ColorF(0xCBD5E1));
        context->DrawLine({left + 0.5f, 0}, {left + 0.5f, static_cast<float>(layout.height)}, brush.Get());
        if (plan.titleLayout) {
            brush->SetColor(D2D1::ColorF(0x172033));
            context->DrawTextLayout({left + layout.padding + plan.titleInsetX,
                layout.padding + plan.titleInsetY}, plan.titleLayout.Get(), brush.Get());
        }
    }
    for (const auto& row : layout.rows) {
        if (row.index >= plan.notes.size()) return E_INVALIDARG;
        const auto& note = plan.notes[row.index];
        if (!note.textLayout || !note.numberLayout) return E_INVALIDARG;
        const float radius = layout.badgeDiameter / 2.0f;
        // Keep the identifier beside the first lines of a long note.
        const float badgeY = row.y;
        brush->SetColor(D2D1::ColorF(note.color.backgroundRgb));
        context->FillEllipse(D2D1::Ellipse({row.x + radius, badgeY + radius}, radius, radius), brush.Get());
        brush->SetColor(D2D1::ColorF(note.color.textRgb));
        context->DrawTextLayout({row.x, badgeY}, note.numberLayout.Get(), brush.Get());
        brush->SetColor(D2D1::ColorF(0x27364B));
        context->DrawTextLayout({row.x + layout.badgeDiameter + layout.badgeGap + note.textInsetX,
            row.y + (row.height - note.textHeight) / 2.0f + note.textInsetY},
            note.textLayout.Get(), brush.Get());
    }
    return S_OK;
}

HRESULT AnnotationRenderer::render(ID2D1DeviceContext* context, ID2D1Bitmap1* source,
    const Plan& plan, ComPtr<ID2D1Bitmap1>& output) noexcept
{
    if (!context || !source) return E_POINTER;
    const auto sourceSize = source->GetPixelSize();
    const auto pixelFormat = source->GetPixelFormat();
    if (sourceSize.width != plan.layout.imageWidth || sourceSize.height != plan.layout.imageHeight ||
        plan.layout.width < sourceSize.width || plan.layout.height < sourceSize.height ||
        plan.layout.width > context->GetMaximumBitmapSize() || plan.layout.height > context->GetMaximumBitmapSize() ||
        pixelFormat.format != DXGI_FORMAT_B8G8R8A8_UNORM || pixelFormat.alphaMode != D2D1_ALPHA_MODE_PREMULTIPLIED)
        return E_INVALIDARG;
    ComPtr<ID2D1Bitmap1> bitmap;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    HRESULT result = context->CreateBitmap({plan.layout.width, plan.layout.height}, nullptr, 0,
        properties, bitmap.GetAddressOf());
    if (FAILED(result)) return result;
    {
        ContextState state(context);
        context->SetTarget(bitmap.Get());
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0xFFFFFF));
        result = paintNotes(context, plan);
        const HRESULT drawResult = context->EndDraw();
        if (FAILED(result)) return result;
        if (FAILED(drawResult)) return drawResult;
    }
    const D2D1_POINT_2U destination{0, 0};
    const D2D1_RECT_U original{0, 0, sourceSize.width, sourceSize.height};
    result = bitmap->CopyFromBitmap(&destination, source, &original);
    if (FAILED(result)) return result;
    output = std::move(bitmap);
    return S_OK;
}
