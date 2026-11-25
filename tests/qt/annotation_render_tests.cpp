#include "services/rendering/QtAnnotationRenderer.h"

#include <QColorSpace>
#include <QDir>
#include <QFontInfo>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QImageReader>
#include <QPainter>
#include <QRawFont>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Renderer = HuhuCapture::QtAnnotationRenderer;
using HuhuCapture::Core::AnnotationNote;
using HuhuCapture::Core::annotationColor;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checked(Renderer::Status status, const char* operation)
{
    if (status != Renderer::Status::Ok)
        throw std::runtime_error(std::string(operation) + " failed: status " + std::to_string(static_cast<int>(status)));
}

QString normalizedParagraphs(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QChar('\r'), QChar('\n'));
    text.replace(QChar(0x2029), QChar('\n'));
    return text;
}

void verifyCompleteText(const Renderer::PreparedText& text)
{
    QString reconstructed;
    std::size_t lines = 0;
    for (std::size_t index = 0; index < text.paragraphs.size(); ++index) {
        const auto& paragraph = *text.paragraphs[index].layout;
        if (index != 0) reconstructed += QChar('\n');
        reconstructed += paragraph.text();
        int covered = 0;
        for (int lineIndex = 0; lineIndex < paragraph.lineCount(); ++lineIndex) {
            const auto line = paragraph.lineAt(lineIndex);
            require(line.isValid() && line.textStart() == covered, "Text layout skipped or repeated code units");
            covered += line.textLength();
        }
        require(covered == paragraph.text().size(), "Text layout omitted the end of a paragraph");
        lines += static_cast<std::size_t>((std::max)(1, paragraph.lineCount()));
    }
    require(reconstructed == normalizedParagraphs(text.text), "Layout did not retain the complete input text");
    require(lines == text.lineCount, "Empty paragraphs or line counts were lost");
}

void verifySourcePreserved(const QImage& original, const QImage& composed)
{
    require(composed.width() >= original.width() && composed.height() >= original.height(),
        "Composition clipped the source image");
    require(composed.format() == original.format(), "Composition converted the source pixel format");
    for (int y = 0; y < original.height(); ++y)
        require(std::memcmp(original.constScanLine(y), composed.constScanLine(y),
            static_cast<std::size_t>(original.width()) * 4) == 0,
            "Composition changed original pixels or alpha");
}

void verifyRequiredCjkFonts(const Renderer::PreparedText& text)
{
    // A missing-glyph box has ink too: pixel counts alone cannot establish that
    // Chinese text was rendered. This fixture contains ordinary visible CJK and
    // punctuation, so glyph zero is a missing required font, not a control char.
    bool sawGlyph = false;
    QStringList fallbackFonts;
    for (const auto& paragraph : text.paragraphs) {
        const QFontInfo requested(paragraph.layout->font());
        std::cout << "FONT QFontInfo: " << requested.family().toStdString() << " / "
            << requested.styleName().toStdString() << ", point=" << requested.pointSizeF()
            << ", pixel=" << requested.pixelSize() << ", exact=" << requested.exactMatch() << '\n';
        for (const auto& run : paragraph.layout->glyphRuns()) {
            const auto font = run.rawFont();
            require(font.isValid(), "CJK text has no usable raw font");
            const auto description = font.familyName() + QStringLiteral(" / ") + font.styleName() +
                QStringLiteral(" / ") + QString::number(font.pixelSize()) + QStringLiteral(" px");
            if (!fallbackFonts.contains(description)) {
                fallbackFonts.push_back(description);
                std::cout << "FONT actual CJK/ASCII run: " << description.toStdString() << '\n';
            }
            for (const auto glyph : run.glyphIndexes()) {
                require(glyph != 0,
                    "Required CJK font missing: glyph zero is not complete Chinese rendering; install fonts-noto-cjk on Linux");
                sawGlyph = true;
            }
        }
    }
    require(sawGlyph, "CJK fixture had no shaped glyphs");
    std::cout << "PASS required Chinese font coverage (no missing-glyph boxes)\n";
}

QImage makeSource()
{
    QImage result(480, 280, QImage::Format_ARGB32_Premultiplied);
    require(!result.isNull(), "Could not allocate source fixture");
    for (int y = 0; y < result.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const int shade = ((x / 24 + y / 24) % 2 == 0) ? 242 : 232;
            row[x] = y < 42 ? qRgb(23, 32, 51) : qRgb(shade, shade, shade);
        }
    }
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}

void verifyNotes(const Renderer::Plan& plan, const std::vector<AnnotationNote>& inputs,
    const Renderer::Options& options, const QImage& image)
{
    require(plan.notes().size() == inputs.size() && plan.layout().rows.size() == inputs.size(),
        "A note was omitted");
    const auto& geometry = plan.layout();
    if (!options.title.isEmpty()) {
        require(plan.title().text == options.title, "The full title was not retained");
        verifyCompleteText(plan.title());
        require(geometry.padding + plan.title().height <= geometry.headerHeight,
            "A wrapped title exceeds its header");
    }
    for (const auto& row : geometry.rows) {
        require(row.index < inputs.size(), "Layout references an unknown note");
        const auto& input = inputs[row.index];
        const auto& note = plan.notes()[row.index];
        auto expected = input.note.empty() ? options.emptyNoteText : QString::fromStdWString(input.note);
        if (expected.isEmpty()) expected = QString(QChar(0x2014));
        require(note.number == input.number && note.body.text == expected, "A note's identifier or text changed");
        require(note.color == annotationColor(input.number), "A note's stable color changed");
        verifyCompleteText(note.body);
        verifyCompleteText(note.badge);
        require(note.badge.text == QString::number(static_cast<qulonglong>(input.number)), "An identifier was elided");
        require(note.body.height <= row.height + 0.01, "Full text extends below its row");
        require(row.x >= geometry.imageWidth && row.y >= 0 &&
            row.x + row.width <= image.width() && row.y + row.height <= image.height(),
            "A row falls outside its image-relative rail");
        const auto bodyInk = note.body.inkBounds.translated(note.body.inset);
        require(bodyInk.left() >= -0.01 && bodyInk.right() <= geometry.textWidth + 0.01 &&
            bodyInk.top() >= -0.01 && bodyInk.bottom() <= note.body.height + 0.01,
            "Fallback or combining glyph ink exceeds its measured body");
        const auto badgeInk = note.badge.inkBounds.translated(note.badge.inset);
        require(badgeInk.left() >= -0.01 && badgeInk.right() <= geometry.badgeDiameter + 0.01 &&
            badgeInk.top() >= -0.01 && badgeInk.bottom() <= geometry.badgeDiameter + 0.01,
            "Identifier ink exceeds its badge");
        require(std::abs(note.badge.fontSize - geometry.badgeFontSize) < 0.01,
            "Badge and text compaction diverged");

        std::size_t markerPixels = 0, textPixels = 0;
        for (int y = static_cast<int>(std::ceil(row.y)); y < static_cast<int>(row.y + row.height); ++y) {
            for (int x = static_cast<int>(std::ceil(row.x)); x < static_cast<int>(row.x + row.width); ++x) {
                const auto pixel = image.pixel(x, y);
                if ((pixel & 0xFFFFFFu) == note.color.backgroundRgb) ++markerPixels;
                if (x > row.x + geometry.badgeDiameter + geometry.badgeGap &&
                    qRed(pixel) < 180 && qGreen(pixel) < 180 && qBlue(pixel) < 180)
                    ++textPixels;
            }
        }
        require(markerPixels >= 12, "The actual colored badge was not rasterized");
        require(textPixels >= 4, "The actual note text was not rasterized");
    }
}

void saveAndVerifyPng(const QImage& image, const QDir& directory, const QString& name)
{
    const auto path = directory.filePath(name + QStringLiteral(".png"));
    require(image.save(path, "PNG"), "Could not encode real PNG evidence");
    QImageReader reader(path, "PNG");
    auto decoded = reader.read();
    require(!decoded.isNull() && decoded.size() == image.size(), "Could not decode complete PNG evidence");
    decoded = decoded.convertToFormat(image.format());
    // These evidence fixtures are opaque, so premultiplication does not round
    // away color bits on PNG encoding/decoding. Alpha fixtures are tested below
    // directly against raw bytes in every supported source format.
    verifySourcePreserved(image, decoded);
}

Renderer::Plan runCase(const QDir& directory, const QString& name,
    const std::vector<AnnotationNote>& notes, const Renderer::Options& options)
{
    auto source = makeSource();
    source.setDevicePixelRatio(2.25);
    source.setDotsPerMeterX(5669);
    source.setDotsPerMeterY(5669);
    const auto sourceKey = source.cacheKey();
    Renderer::Plan plan;
    checked(Renderer::makePlan(source.size(), notes, options, plan), "Plan complete annotation export");
    require(plan.isValid(), "Successful planning returned an invalid plan");
    QImage output;
    checked(Renderer::render(source, plan, output), "Render complete annotation export");
    require(source.cacheKey() == sourceKey && source.devicePixelRatio() == 2.25,
        "Export mutated its input or input metadata");
    require(output.width() == static_cast<int>(plan.layout().width) &&
        output.height() == static_cast<int>(plan.layout().height), "Output ignored planned dimensions");
    require(output.devicePixelRatio() == 1 && output.logicalDpiX() == 96 && output.logicalDpiY() == 96,
        "Output depends on display DPI or source DPR");
    require(output.colorSpace() == source.colorSpace(), "Export lost the source color-space tag");
    verifySourcePreserved(source, output);
    if (notes.empty()) {
        require(output.size() == source.size() && plan.layout().columns == 0, "Empty notes added an unwanted rail");
    } else {
        verifyNotes(plan, notes, options, output);
        if (output.height() > source.height())
            require(output.pixel(0, source.height()) == qRgb(255, 255, 255), "Extra image canvas is not white");
    }
    saveAndVerifyPng(output, directory, name);
    std::cout << "PASS " << name.toStdString() << ": " << notes.size() << " complete notes, "
        << output.width() << 'x' << output.height() << " PNG, " << plan.layout().columns << " columns\n";
    return plan;
}

void verifyAlphaAndBorrowedStride(const Renderer::Options& options)
{
    const std::array formats{QImage::Format_RGB32, QImage::Format_ARGB32,
        QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBX8888,
        QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied};
    const std::vector<AnnotationNote> notes{{9, L"Exact original alpha and pixel bytes"}};
    Renderer::Plan plan, emptyPlan;
    checked(Renderer::makePlan(QSize(3, 2), notes, options, plan), "Plan alpha fixture");
    checked(Renderer::makePlan(QSize(3, 2), {}, options, emptyPlan), "Plan image-only fixture");
    for (const auto format : formats) {
        std::array<uchar, 40> bytes{};
        bytes.fill(0xAC); // The 8 padding bytes per row must not be treated as pixels.
        const std::array<uchar, 24> pixels{0, 0, 0, 0, 24, 48, 64, 128, 255, 128, 64, 255,
            1, 2, 3, 4, 7, 0, 4, 12, 0, 0, 0, 0};
        std::memcpy(bytes.data(), pixels.data(), 12);
        std::memcpy(bytes.data() + 20, pixels.data() + 12, 12);
        if (format == QImage::Format_ARGB32 || format == QImage::Format_RGBA8888) {
            bytes[0] = 17;
            bytes[1] = 29;
            bytes[2] = 43; // Hidden color under alpha zero must also survive.
        }
        if (format == QImage::Format_RGB32 || format == QImage::Format_RGBX8888)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 3; ++x) bytes[static_cast<std::size_t>(y * 20 + x * 4 + 3)] = 255;
        const auto originalBytes = bytes;
        QImage source(bytes.data(), 3, 2, 20, format);
        source.setDevicePixelRatio(3);
        source.setColorSpace(QColorSpace::DisplayP3);
        QImage result;
        checked(Renderer::render(source, plan, result), "Render padded alpha source");
        verifySourcePreserved(source, result);
        require(bytes == originalBytes, "Renderer wrote through the borrowed input buffer");
        require(result.colorSpace() == source.colorSpace(), "Renderer converted the original color space");

        QImage alias = source;
        checked(Renderer::render(alias, emptyPlan, alias), "Render aliased image-only output");
        require(alias.size() == source.size(), "Image-only alias changed dimensions");
        verifySourcePreserved(source, alias);
    }
    std::cout << "PASS six 32-bit pixel formats, premultiplied/straight alpha, hidden RGB, padded source stride, aliasing\n";
}

void verifyFailurePreservesOutputs(const Renderer::Options& options)
{
    const std::vector<AnnotationNote> notes{{3, L"Keep the previous successful export"}};
    auto source = makeSource();
    Renderer::Plan plan;
    checked(Renderer::makePlan(source.size(), notes, options, plan), "Plan rejection fixture");
    const auto* savedNotes = plan.notes().data();
    const auto savedWidth = plan.layout().width;
    const auto failedPlan = [&](QSize size, const std::vector<AnnotationNote>& input,
                               const Renderer::Options& testOptions) {
        require(Renderer::makePlan(size, input, testOptions, plan) != Renderer::Status::Ok,
            "Invalid or incomplete layout was accepted");
        require(plan.isValid() && plan.notes().data() == savedNotes && plan.layout().width == savedWidth,
            "A failed plan replaced previous valid output");
    };
    failedPlan(QSize(0, 10), notes, options);
    failedPlan(QSize(-1, 10), notes, options);
    failedPlan(source.size(), {{0, L"Invalid identity"}}, options);
    auto limited = options;
    limited.maximumBytes = 32;
    failedPlan(source.size(), notes, limited);
    limited = options;
    limited.maximumDimension = 400;
    failedPlan(source.size(), notes, limited);
    limited = options;
    limited.preferredFontSize = (std::numeric_limits<float>::quiet_NaN)();
    failedPlan(source.size(), notes, limited);
    limited = options;
    limited.padding = (std::numeric_limits<float>::infinity)();
    failedPlan(source.size(), notes, limited);
    limited = options;
    limited.panelWidth = 0;
    failedPlan(source.size(), notes, limited);
    limited = options;
    limited.maximumDimension = 512;
    std::wstring tooTall;
    for (int index = 0; index < 200; ++index) tooTall += L"A complete line\n";
    failedPlan(QSize(16, 16), {{8, tooTall}}, limited);
    std::vector<AnnotationNote> tooMany;
    for (std::uint64_t id = 1; id <= 100; ++id) tooMany.push_back({id, L"Full row"});
    failedPlan(QSize(16, 16), tooMany, limited);

    QImage output;
    checked(Renderer::render(source, plan, output), "Render preserved plan after failures");
    const auto savedKey = output.cacheKey();
    const auto failedRender = [&](const QImage& input, const Renderer::Plan& inputPlan) {
        require(Renderer::render(input, inputPlan, output) != Renderer::Status::Ok, "Invalid render input was accepted");
        require(output.cacheKey() == savedKey, "A failed render replaced previous valid output");
    };
    failedRender(QImage(), plan);
    failedRender(QImage(10, 10, QImage::Format_ARGB32), plan);
    failedRender(QImage(source.size(), QImage::Format_Grayscale8), plan);
    const Renderer::Plan invalid;
    failedRender(source, invalid);
    QPainter inactive;
    require(Renderer::paintNotes(inactive, plan) == Renderer::Status::InvalidInput, "Inactive painter was accepted");

    Renderer::Status otherThreadStatus = Renderer::Status::Ok;
    std::thread otherThread([&] { otherThreadStatus = Renderer::render(source, plan, output); });
    otherThread.join();
    require(otherThreadStatus == Renderer::Status::InvalidInput && output.cacheKey() == savedKey,
        "A plan migrated a Qt font engine to another thread");
    std::cout << "PASS invalid dimensions/IDs/fonts/formats, complete-row overflow, allocation limit, thread ownership, atomic failure\n";
}

void verifyPreviewState(const Renderer::Options& options)
{
    const std::vector<AnnotationNote> notes{{1, L"Preview uses the same full-text plan"}};
    const auto source = makeSource();
    Renderer::Plan plan;
    checked(Renderer::makePlan(source.size(), notes, options, plan), "Plan preview fixture");
    QImage image(static_cast<int>(plan.layout().width), static_cast<int>(plan.layout().height), QImage::Format_ARGB32);
    image.fill(qRgb(8, 17, 29));
    QPainter painter(&image);
    require(painter.isActive(), "Could not start preview painter");
    painter.setWorldTransform(QTransform::fromScale(0.75, 1.25).translate(11, 17));
    painter.setPen(QPen(Qt::magenta, 3));
    painter.setBrush(QBrush(Qt::green, Qt::Dense4Pattern));
    painter.setBrushOrigin(3, 7);
    painter.setOpacity(0.6);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, false);
    painter.setClipRect(QRect(5, 7, image.width() - 10, image.height() - 14));
    const auto transform = painter.worldTransform();
    const auto pen = painter.pen();
    const auto brush = painter.brush();
    const auto origin = painter.brushOrigin();
    const auto opacity = painter.opacity();
    const auto composition = painter.compositionMode();
    const auto hints = painter.renderHints();
    const auto clip = painter.clipRegion();
    checked(Renderer::paintNotes(painter, plan), "Paint transformed preview");
    require(painter.worldTransform() == transform && painter.pen() == pen && painter.brush() == brush &&
        painter.brushOrigin() == origin && painter.opacity() == opacity && painter.compositionMode() == composition &&
        painter.renderHints() == hints && painter.clipRegion() == clip,
        "Preview leaked painter state into the editor");
    require(painter.end(), "Could not finish preview painter");

    image.fill(qRgb(8, 17, 29));
    require(painter.begin(&image), "Could not restart preview painter");
    checked(Renderer::paintNotes(painter, plan), "Paint untransformed preview");
    require(painter.end(), "Could not finish untransformed preview");
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x)
            require(image.pixel(x, y) == qRgb(8, 17, 29), "Rail preview painted into the original image area");
    std::cout << "PASS shared preview/export plan, preview transform/clip/paint-state restoration, untouched image area\n";
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Could not create fixture directory");
        const auto arguments = application.arguments();
        require(arguments.size() <= 2, "Usage: annotation_render_tests [render-output-directory]");
        const QDir directory(arguments.size() == 2 ? arguments[1] : temporary.path());
        require(QDir().mkpath(directory.absolutePath()), "Could not create render-output directory");
        Renderer::Options options;
        options.title = QString::fromUtf8("标注备注 / Annotation notes");
        options.emptyNoteText = QString::fromUtf8("（尚未填写备注）");
        options.maximumDimension = 4096;
        options.maximumBytes = 64u * 1024u * 1024u;
        std::cout << "Qt runtime " << qVersion() << ", platform " << QGuiApplication::platformName().toStdString() << '\n';

        runCase(directory, QStringLiteral("01-no-annotations"), {}, options);
        const auto cjk = runCase(directory, QStringLiteral("02-cjk-and-empty-note"), {
            {1, L"中文备注：点击这里查看完整截图。"},
            {2, L"English note with natural wrapping and preserved numbering."}, {5, L""}}, options);
        verifyRequiredCjkFonts(cjk.notes()[0].body);

        std::wstring longText;
        for (int index = 0; index < 12; ++index)
            longText += L"这是较长的备注，换行后的文字也必须完整导出。Long notes remain readable and complete.\n";
        const auto longPlan = runCase(directory, QStringLiteral("03-long-multiline-notes"),
            {{1, longText}, {7, L"最后一条备注仍然存在。\nThe final note remains."}}, options);
        require(longPlan.layout().height > longPlan.layout().imageHeight, "Long notes did not expand the canvas");

        std::vector<AnnotationNote> many;
        for (std::uint64_t id = 1; id <= 45; ++id)
            many.push_back({id, L"完整备注 " + std::to_wstring(id) + L" / complete note"});
        auto columnsOptions = options;
        columnsOptions.maximumDimension = 1400;
        const auto columns = runCase(directory, QStringLiteral("04-many-notes-multiple-columns"), many, columnsOptions);
        require(columns.layout().columns >= 2, "Dimension-limited notes did not flow into more columns");

        const std::wstring url = L"https://example.invalid/reports/" + std::wstring(1200, L'W') +
            L"?token=" + std::wstring(300, L'x');
        const auto unbroken = runCase(directory, QStringLiteral("05-unbroken-url"), {{3, url}}, options);
        require(unbroken.notes()[0].body.lineCount > 1, "Long URL did not wrap");

        std::vector<AnnotationNote> compactNotes;
        for (std::uint64_t id = 1; id <= 5; ++id)
            compactNotes.push_back({id, L"Compact note #" + std::to_wstring(id)});
        const auto compact = runCase(directory, QStringLiteral("06-compact-short-notes"), compactNotes, options);
        require(compact.layout().fontSize < options.preferredFontSize && compact.layout().height == 280,
            "Short notes did not compact into the original image height");

        runCase(directory, QStringLiteral("07-large-identifiers"),
            {{(std::numeric_limits<std::uint64_t>::max)(), L"All 20 identifier digits remain visible."}}, options);
        const std::wstring unicode = L"第一行 / first line\r\n\r\n"
            L"Emoji \U0001F600 and combining e\u0301 / a\u0301\u0323\n"
            L"العربية / עברית / Ελληνικά\u2029Last complete paragraph\n";
        const auto paragraphs = runCase(directory, QStringLiteral("08-unicode-and-blank-lines"), {{11, unicode}}, options);
        require(paragraphs.notes()[0].body.paragraphs.size() == 6, "Explicit empty or final paragraphs disappeared");

        auto fractional = options;
        fractional.preferredFontSize = 18.5f;
        fractional.minimumFontSize = 12.5f;
        fractional.title = QStringLiteral("A complete title that deliberately wraps into several header lines without ellipsis");
        const auto fractionalPlan = runCase(directory, QStringLiteral("09-fractional-size-and-wrapped-title"),
            {{1, L"Fractional font size stays independent of the screen DPI."}}, fractional);
        require(fractionalPlan.title().lineCount > 1 &&
            std::abs(fractionalPlan.notes()[0].body.fontSize - fractionalPlan.layout().fontSize) < 0.01,
            "Title wrapping or fractional font measurement changed");
        auto fallback = options;
        fallback.title.clear();
        fallback.emptyNoteText.clear();
        runCase(directory, QStringLiteral("10-no-title-and-fallback-placeholder"), {{2, L""}}, fallback);

        verifyAlphaAndBorrowedStride(options);
        verifyFailurePreservesOutputs(options);
        verifyPreviewState(options);
        std::cout << "Qt annotation renderer checks passed with real QTextLayout, QPainter, QImage and PNG encoding.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
