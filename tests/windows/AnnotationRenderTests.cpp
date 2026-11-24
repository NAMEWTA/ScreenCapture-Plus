#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "services/AnnotationRenderer.h"

using Microsoft::WRL::ComPtr;
using ScreenCapture::Core::AnnotationNote;
using ScreenCapture::Core::annotationColor;
namespace fs = std::filesystem;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checked(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result)) return;
    std::ostringstream message;
    message << operation << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(result);
    throw std::runtime_error(message.str());
}

class ComApartment {
public:
    ComApartment() { checked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~ComApartment() { CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

struct Pixels {
    UINT width{}, height{};
    std::vector<BYTE> bytes;
};

class Renderer {
public:
    Renderer()
    {
        const D3D_FEATURE_LEVEL featureLevels[]{D3D_FEATURE_LEVEL_11_0};
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 1, D3D11_SDK_VERSION,
            d3d.GetAddressOf(), nullptr, nullptr), "Create WARP device");
        ComPtr<IDXGIDevice> dxgi;
        checked(d3d.As(&dxgi), "Get DXGI device");
        checked(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            nullptr, reinterpret_cast<void**>(factory.GetAddressOf())), "Create D2D factory");
        checked(factory->CreateDevice(dxgi.Get(), device.GetAddressOf()), "Create D2D device");
        checked(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.GetAddressOf()),
            "Create D2D device context");
        checked(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())), "Create DirectWrite factory");
        checked(writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"zh-CN",
            format.GetAddressOf()), "Create note text format");
        checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wic.GetAddressOf())), "Create WIC factory");
    }

    ComPtr<ID2D1Bitmap1> bitmap(UINT width, UINT height, const BYTE* data = nullptr,
        D2D1_BITMAP_OPTIONS options = D2D1_BITMAP_OPTIONS_NONE)
    {
        ComPtr<ID2D1Bitmap1> result;
        const auto properties = D2D1::BitmapProperties1(options,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        checked(context->CreateBitmap({width, height}, data, data ? width * 4 : 0,
            &properties, result.GetAddressOf()), "Create test bitmap");
        return result;
    }

    Pixels read(ID2D1Bitmap1* bitmap)
    {
        const auto size = bitmap->GetPixelSize();
        auto cpu = this->bitmap(size.width, size.height, nullptr,
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
        checked(cpu->CopyFromBitmap(nullptr, bitmap, nullptr), "Copy image to CPU bitmap");
        D2D1_MAPPED_RECT mapped{};
        checked(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped), "Map image pixels");
        Pixels result{size.width, size.height, std::vector<BYTE>(static_cast<std::size_t>(size.width) * size.height * 4)};
        for (UINT y = 0; y < size.height; ++y) {
            std::memcpy(result.bytes.data() + static_cast<std::size_t>(y) * size.width * 4,
                mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(size.width) * 4);
        }
        checked(cpu->Unmap(), "Unmap image pixels");
        return result;
    }

    void png(const fs::path& path, const Pixels& pixels)
    {
        ComPtr<IWICBitmap> image;
        checked(wic->CreateBitmapFromMemory(pixels.width, pixels.height, GUID_WICPixelFormat32bppPBGRA,
            pixels.width * 4, static_cast<UINT>(pixels.bytes.size()), const_cast<BYTE*>(pixels.bytes.data()),
            image.GetAddressOf()), "Create WIC image");
        ComPtr<IWICStream> stream;
        checked(wic->CreateStream(stream.GetAddressOf()), "Create PNG stream");
        checked(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open PNG destination");
        ComPtr<IWICBitmapEncoder> encoder;
        checked(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()), "Create PNG encoder");
        checked(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
        ComPtr<IWICBitmapFrameEncode> frame;
        checked(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr), "Create PNG frame");
        checked(frame->Initialize(nullptr), "Initialize PNG frame");
        checked(frame->SetSize(pixels.width, pixels.height), "Set PNG dimensions");
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        checked(frame->SetPixelFormat(&pixelFormat), "Set PNG pixel format");
        checked(frame->WriteSource(image.Get(), nullptr), "Write composed PNG image");
        checked(frame->Commit(), "Commit PNG frame");
        checked(encoder->Commit(), "Commit PNG encoder");
        frame.Reset();
        encoder.Reset();
        stream.Reset();

        // Decode the artifact again, so this check also covers the actual PNG
        // dimensions and content rather than only the in-memory composition.
        ComPtr<IWICBitmapDecoder> decoder;
        checked(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()), "Decode generated PNG");
        ComPtr<IWICBitmapFrameDecode> decoded;
        checked(decoder->GetFrame(0, decoded.GetAddressOf()), "Read generated PNG frame");
        UINT width{}, height{};
        checked(decoded->GetSize(&width, &height), "Read generated PNG dimensions");
        require(width == pixels.width && height == pixels.height, "PNG dimensions differ from composed image");
        ComPtr<IWICFormatConverter> converter;
        checked(wic->CreateFormatConverter(converter.GetAddressOf()), "Create PNG verification converter");
        checked(converter->Initialize(decoded.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "Convert generated PNG pixels");
        std::vector<BYTE> roundTrip(pixels.bytes.size());
        checked(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(roundTrip.size()), roundTrip.data()),
            "Read generated PNG pixels");
        require(roundTrip == pixels.bytes, "Opaque PNG pixels differ after encoding and decoding");
    }

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IWICImagingFactory> wic;
};

ComPtr<ID2D1Bitmap1> makeSource(Renderer& renderer, std::span<const AnnotationNote> notes)
{
    constexpr UINT width = 480, height = 280;
    std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            const BYTE shade = ((x / 24 + y / 24) % 2 == 0) ? 242 : 232;
            pixels[offset] = shade;
            pixels[offset + 1] = shade;
            pixels[offset + 2] = shade;
            pixels[offset + 3] = 255;
        }
    }
    auto result = renderer.bitmap(width, height, pixels.data(), D2D1_BITMAP_OPTIONS_TARGET);
    auto* context = renderer.context.Get();
    context->SetTarget(result.Get());
    context->SetDpi(96.0f, 96.0f);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ComPtr<ID2D1SolidColorBrush> brush;
    checked(context->CreateSolidColorBrush(D2D1::ColorF(0x172033), brush.GetAddressOf()), "Create fixture brush");
    context->BeginDraw();
    context->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(width), 42), brush.Get());
    ComPtr<IDWriteTextLayout> title;
    const std::wstring titleText = L"Synthetic capture / native render verification";
    checked(renderer.writeFactory->CreateTextLayout(titleText.data(), static_cast<UINT32>(titleText.size()),
        renderer.format.Get(), 460.0f, 32.0f, title.GetAddressOf()), "Create fixture title");
    title->SetFontSize(16.0f, {0, static_cast<UINT32>(titleText.size())});
    brush->SetColor(D2D1::ColorF(0xFFFFFF));
    context->DrawTextLayout({10, 10}, title.Get(), brush.Get());
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const auto color = annotationColor(notes[index].number);
        const float cx = 28.0f + static_cast<float>(index % 9) * 53.0f;
        const float cy = 68.0f + static_cast<float>(index / 9) * 43.0f;
        brush->SetColor(D2D1::ColorF(color.backgroundRgb));
        context->FillEllipse(D2D1::Ellipse({cx, cy}, 16.0f, 16.0f), brush.Get());
        const auto text = std::to_wstring(notes[index].number);
        ComPtr<IDWriteTextLayout> number;
        checked(renderer.writeFactory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
            renderer.format.Get(), 32.0f, 32.0f, number.GetAddressOf()), "Create fixture marker number");
        number->SetFontSize(14.0f, {0, static_cast<UINT32>(text.size())});
        number->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        number->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(D2D1::ColorF(color.textRgb));
        context->DrawTextLayout({cx - 16.0f, cy - 16.0f}, number.Get(), brush.Get());
    }
    checked(context->EndDraw(), "Draw synthetic source fixture");
    context->SetTarget(nullptr);
    return result;
}

void verifySourcePreserved(const Pixels& original, const Pixels& composed)
{
    require(composed.width >= original.width && composed.height >= original.height, "Output clipped source image");
    for (UINT y = 0; y < original.height; ++y) {
        require(std::memcmp(original.bytes.data() + static_cast<std::size_t>(y) * original.width * 4,
            composed.bytes.data() + static_cast<std::size_t>(y) * composed.width * 4,
            static_cast<std::size_t>(original.width) * 4) == 0,
            "Composition changed captured-image pixels");
    }
}

void verifyNotes(const AnnotationRenderer::Plan& plan, std::span<const AnnotationNote> notes,
    const AnnotationRenderer::Options& options, const Pixels& pixels)
{
    require(plan.notes.size() == notes.size() && plan.layout.rows.size() == notes.size(), "A note was omitted");
    for (const auto& row : plan.layout.rows) {
        const auto& note = plan.notes.at(row.index);
        const auto& input = notes[row.index];
        require(note.number == input.number, "A note's identifier changed during export");
        require(note.text == (input.note.empty() ? options.emptyNoteText : input.note), "A note's text was truncated");
        require(note.color == annotationColor(input.number), "A note lost its stable marker color");
        require(note.textHeight <= row.height + 0.01f, "A wrapped note exceeds its planned row");
        require(row.x >= plan.layout.imageWidth && row.y >= 0 &&
            row.x + row.width <= pixels.width && row.y + row.height <= pixels.height,
            "A note row falls outside the exported right-hand panel");
        FLOAT badgeFontSize{};
        DWRITE_TEXT_METRICS badgeMetrics{};
        checked(note.numberLayout->GetFontSize(0, &badgeFontSize, nullptr), "Inspect marker number font size");
        checked(note.numberLayout->GetMetrics(&badgeMetrics), "Measure marker number text");
        require(std::abs(badgeFontSize - plan.layout.badgeFontSize) < 0.01f &&
            badgeMetrics.widthIncludingTrailingWhitespace <= plan.layout.badgeDiameter + 0.5f &&
            badgeMetrics.height <= plan.layout.badgeDiameter + 0.5f,
            "Marker text no longer fits its compact badge");
        DWRITE_TRIMMING trimming{};
        ComPtr<IDWriteInlineObject> trimmingSign;
        checked(note.textLayout->GetTrimming(&trimming, trimmingSign.GetAddressOf()), "Inspect note trimming");
        require(trimming.granularity == DWRITE_TRIMMING_GRANULARITY_NONE, "Note text enabled lossy trimming");
        DWRITE_TEXT_METRICS textMetrics{};
        DWRITE_OVERHANG_METRICS overhang{};
        checked(note.textLayout->GetMetrics(&textMetrics), "Measure rasterized note text");
        checked(note.textLayout->GetOverhangMetrics(&overhang), "Measure rasterized note ink");
        require(textMetrics.widthIncludingTrailingWhitespace <= note.textLayout->GetMaxWidth() + 0.5f,
            "A note's wrapped text exceeds the available line width");
        require(note.textInsetX - overhang.left >= -0.5f &&
            note.textInsetX + note.textLayout->GetMaxWidth() + (std::max)(0.0f, overhang.right) <= plan.layout.textWidth + 0.5f,
            "A note's glyph overhang exceeds the available text area");

        std::size_t markerPixels = 0, textPixels = 0;
        const auto rgb = note.color.backgroundRgb;
        for (UINT y = static_cast<UINT>(std::ceil(row.y)); y < static_cast<UINT>(row.y + row.height); ++y) {
            for (UINT x = static_cast<UINT>(std::ceil(row.x)); x < static_cast<UINT>(row.x + row.width); ++x) {
                const auto offset = (static_cast<std::size_t>(y) * pixels.width + x) * 4;
                const auto* pixel = pixels.bytes.data() + offset;
                if (pixel[0] == (rgb & 255) && pixel[1] == ((rgb >> 8) & 255) && pixel[2] == ((rgb >> 16) & 255))
                    ++markerPixels;
                if (x > row.x + plan.layout.badgeDiameter + plan.layout.badgeGap &&
                    pixel[0] < 180 && pixel[1] < 180 && pixel[2] < 180) ++textPixels;
            }
        }
        require(markerPixels >= 12, "A note's colored marker was not rasterized");
        require(textPixels >= 4, "A note's text was not rasterized");
    }
}

void verifyRenderState(Renderer& renderer, ID2D1Bitmap1* source,
    const AnnotationRenderer::Plan& plan, ComPtr<ID2D1Bitmap1>& result)
{
    auto sentinel = renderer.bitmap(16, 16, nullptr, D2D1_BITMAP_OPTIONS_TARGET);
    auto* context = renderer.context.Get();
    const auto transform = D2D1::Matrix3x2F::Scale(1.25f, 0.75f) * D2D1::Matrix3x2F::Translation(13.0f, 17.0f);
    context->SetTarget(sentinel.Get());
    context->SetDpi(120.0f, 144.0f);
    context->SetTransform(transform);
    context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    checked(AnnotationRenderer::render(context, source, plan, result), "Render production annotation export");
    ComPtr<ID2D1Image> restoredTarget;
    context->GetTarget(restoredTarget.GetAddressOf());
    D2D1_MATRIX_3X2_F restoredTransform{};
    context->GetTransform(&restoredTransform);
    FLOAT dpiX{}, dpiY{};
    context->GetDpi(&dpiX, &dpiY);
    require(restoredTarget.Get() == sentinel.Get(), "Export changed the editor render target");
    require(std::memcmp(&restoredTransform, &transform, sizeof(transform)) == 0, "Export changed the editor transform");
    require(dpiX == 120.0f && dpiY == 144.0f, "Export changed the editor DPI");
    require(context->GetUnitMode() == D2D1_UNIT_MODE_DIPS &&
        context->GetTextAntialiasMode() == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE &&
        context->GetAntialiasMode() == D2D1_ANTIALIAS_MODE_ALIASED &&
        context->GetPrimitiveBlend() == D2D1_PRIMITIVE_BLEND_ADD, "Export leaked render state into the editor");
    result->GetDpi(&dpiX, &dpiY);
    require(dpiX == 96.0f && dpiY == 96.0f, "Export depends on display DPI");
    context->SetTarget(nullptr);
}

AnnotationRenderer::Plan runCase(Renderer& renderer, const fs::path& directory, const char* name,
    const std::vector<AnnotationNote>& notes, AnnotationRenderer::Options options)
{
    auto source = makeSource(renderer, notes);
    const auto original = renderer.read(source.Get());
    AnnotationRenderer::Plan plan;
    checked(AnnotationRenderer::makePlan(renderer.writeFactory.Get(), renderer.format.Get(),
        source->GetPixelSize(), notes, options, plan), "Measure production annotation layout");
    ComPtr<ID2D1Bitmap1> output;
    verifyRenderState(renderer, source.Get(), plan, output);
    const auto pixels = renderer.read(output.Get());
    require(pixels.width == plan.layout.width && pixels.height == plan.layout.height, "Export did not use planned dimensions");
    verifySourcePreserved(original, pixels);
    if (notes.empty()) {
        require(pixels.width == original.width && pixels.height == original.height, "Empty annotations added an unwanted panel");
    } else {
        require(pixels.width > original.width, "Notes were not exported to the right of the capture");
        verifyNotes(plan, notes, options, pixels);
    }
    renderer.png(directory / (std::string(name) + ".png"), pixels);
    std::cout << "PASS " << name << ": " << notes.size() << " complete notes, " << pixels.width << 'x'
        << pixels.height << " PNG, " << plan.layout.columns << " right-side column(s)\n";
    return plan;
}

void verifyAlphaAndRejections(Renderer& renderer, AnnotationRenderer::Options options)
{
    // Valid premultiplied BGRA values include fully transparent and partly
    // transparent pixels. Copying a capture into a white rail must preserve them.
    const BYTE bytes[]{0, 0, 0, 0, 24, 48, 64, 128, 255, 128, 64, 255, 1, 2, 3, 4};
    auto source = renderer.bitmap(2, 2, bytes);
    const std::vector<AnnotationNote> notes{{9, L"Alpha preserved"}};
    AnnotationRenderer::Plan plan;
    checked(AnnotationRenderer::makePlan(renderer.writeFactory.Get(), renderer.format.Get(), {2, 2}, notes,
        options, plan), "Plan alpha fixture");
    ComPtr<ID2D1Bitmap1> output;
    verifyRenderState(renderer, source.Get(), plan, output);
    verifySourcePreserved(renderer.read(source.Get()), renderer.read(output.Get()));

    AnnotationRenderer::Plan invalid;
    require(FAILED(AnnotationRenderer::makePlan(renderer.writeFactory.Get(), renderer.format.Get(), {0, 10},
        notes, options, invalid)), "A zero-width capture was accepted");
    auto limited = options;
    limited.maximumBytes = 32;
    require(FAILED(AnnotationRenderer::makePlan(renderer.writeFactory.Get(), renderer.format.Get(), {480, 280},
        notes, limited, invalid)), "The export allocation limit was ignored");
    limited = options;
    limited.maximumDimension = 400;
    require(FAILED(AnnotationRenderer::makePlan(renderer.writeFactory.Get(), renderer.format.Get(), {480, 280},
        notes, limited, invalid)), "The graphics dimension limit was ignored");
    require(FAILED(AnnotationRenderer::render(nullptr, source.Get(), plan, output)), "Null render context was accepted");
    require(FAILED(AnnotationRenderer::render(renderer.context.Get(), nullptr, plan, output)), "Null source image was accepted");
    std::cout << "PASS premultiplied-alpha preservation, zero dimensions, allocation/dimension limits, invalid render arguments\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        require(argc == 2, "Usage: AnnotationRenderTests.exe <render-output-directory>");
        const fs::path directory(argv[1]);
        fs::create_directories(directory);
        ComApartment apartment;
        Renderer renderer;
        AnnotationRenderer::Options options;
        options.title = L"标注备注 / Annotation notes";
        options.emptyNoteText = L"（尚未填写备注）";
        options.maximumDimension = (std::min)(4096u, renderer.context->GetMaximumBitmapSize());
        options.maximumBytes = 64u * 1024u * 1024u;

        runCase(renderer, directory, "01-no-annotations", {}, options);
        const std::vector<AnnotationNote> mixed{
            {1, L"中文备注：点击这里查看完整截图。"},
            {2, L"English note with natural wrapping and preserved numbering."},
            {5, L""}
        };
        runCase(renderer, directory, "02-cjk-and-empty-note", mixed, options);

        std::wstring longText;
        for (int index = 0; index < 12; ++index)
            longText += L"这是较长的备注，换行后的文字也必须完整导出。Long notes remain readable and must never be ellipsized.\n";
        const std::vector<AnnotationNote> longNotes{{1, longText}, {7, L"最后一条备注仍然存在。\nThe final note is present."}};
        const auto longPlan = runCase(renderer, directory, "03-long-multiline-notes", longNotes, options);
        require(longPlan.layout.height > longPlan.layout.imageHeight, "Long notes did not expand the export canvas");

        std::vector<AnnotationNote> many;
        for (std::uint64_t number = 1; number <= 45; ++number)
            many.push_back({number, L"完整备注 " + std::to_wstring(number) + L" / complete note"});
        auto columnOptions = options;
        columnOptions.maximumDimension = 1400;
        const auto columns = runCase(renderer, directory, "04-many-notes-multiple-columns", many, columnOptions);
        require(columns.layout.columns >= 2, "Dimension-limited notes did not flow into multiple columns");

        const std::wstring url = L"https://example.invalid/reports/" + std::wstring(1200, L'W') +
            L"?token=" + std::wstring(300, L'x');
        const auto unbroken = runCase(renderer, directory, "05-unbroken-url", {{3, url}}, options);
        DWRITE_TEXT_METRICS urlMetrics{};
        checked(unbroken.notes.front().textLayout->GetMetrics(&urlMetrics), "Measure unbroken URL fixture");
        require(urlMetrics.lineCount > 1, "A long unbroken URL was not wrapped");

        std::vector<AnnotationNote> shortNotes;
        for (std::uint64_t number = 1; number <= 5; ++number)
            shortNotes.push_back({number, L"Compact note #" + std::to_wstring(number)});
        const auto compact = runCase(renderer, directory, "06-compact-short-notes", shortNotes, options);
        require(compact.layout.fontSize < options.preferredFontSize && compact.layout.height == 280,
            "Short notes did not compact into the original capture height");
        verifyAlphaAndRejections(renderer, options);
        std::cout << "Native annotation integration checks passed using software D3D11 WARP, Direct2D, DirectWrite and WIC.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
