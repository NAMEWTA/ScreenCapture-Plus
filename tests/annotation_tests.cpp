#include "core/Annotation.h"
#include "core/HistoryStack.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>

using namespace ScreenCapture::Core;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

double luminance(std::uint32_t rgb)
{
    const auto linear = [](std::uint32_t byte) {
        const double value = static_cast<double>(byte) / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear((rgb >> 16) & 255) + 0.7152 * linear((rgb >> 8) & 255) + 0.0722 * linear(rgb & 255);
}

void checkRows(const AnnotationLayout& layout, std::size_t count)
{
    require(layout.rows.size() == count, "layout lost an annotation");
    require(layout.width >= layout.imageWidth && layout.height >= layout.imageHeight,
            "layout reduced the source image");
    std::vector<float> previousBottom(layout.columns, 0.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& row = layout.rows[i];
        require(row.index == i, "layout reordered or duplicated an annotation");
        require(row.column < layout.columns, "row has an invalid column");
        require(row.x >= layout.imageWidth && row.y >= 0 && row.width > 0 && row.height > 0,
                "row overlaps the original image");
        require(row.x + row.width <= layout.width && row.y + row.height <= layout.height,
                "row extends outside the exported bitmap");
        require(row.y >= previousBottom[row.column], "annotation rows overlap");
        previousBottom[row.column] = row.y + row.height;
    }
}

} // namespace

int main()
{
    AnnotationSequence sequence;
    HistoryStack<AnnotationNote> history;
    const auto addNote = [&](std::wstring text) {
        const auto id = sequence.allocate();
        require(id.has_value(), "identifier allocation unexpectedly failed");
        return history.append(std::make_unique<AnnotationNote>(AnnotationNote{*id, std::move(text)}));
    };
    auto* first = addNote(L"first");
    auto* second = addNote(L"\u4e2d\u6587 note");
    auto* third = addNote(L"third");
    require(first->number == 1 && second->number == 2 && third->number == 3,
            "new notes do not start with monotonic IDs");
    const auto secondColor = annotationColor(second->number);
    require(history.undo() == third, "undo did not hide the last note");
    require(history.redo() == third && third->note == L"third", "redo lost the original note");
    history.undo();
    auto* fourth = addNote(L"new branch");
    require(fourth->number == 4 && history.redo() == nullptr, "a discarded note's ID was reused");
    require(history.erase(first), "deleting a pin failed");
    require(second->number == 2 && second->note == L"\u4e2d\u6587 note" && annotationColor(2) == secondColor,
            "deletion renumbered or recolored a surviving annotation");
    AnnotationSequence imported;
    imported.observe(800);
    imported.observe(12);
    require(imported.allocate() == 801, "observing an older ID moved the high water mark backward");
    imported.observe((std::numeric_limits<std::uint64_t>::max)() - 1);
    require(imported.allocate() == (std::numeric_limits<std::uint64_t>::max)(), "last valid ID was lost");
    require(!imported.allocate(), "identifier overflow reused an ID");

    std::set<std::uint32_t> colors;
    for (std::uint64_t id = 1; id <= 256; ++id) {
        const auto color = annotationColor(id);
        require(color == annotationColor(id), "annotation color is not deterministic");
        require(colors.insert(color.backgroundRgb).second, "palette repeated a color in the first 256 notes");
        const double background = luminance(color.backgroundRgb);
        const double foreground = luminance(color.textRgb);
        const double contrast = ((std::max)(background, foreground) + 0.05) /
            ((std::min)(background, foreground) + 0.05);
        require(contrast >= 4.5, "badge numeral does not have readable color contrast");
    }
    require(annotationColor(2).textRgb != 0xFFFFFF, "amber badge must use dark text");

    AnnotationLayoutOptions options;
    const auto fixed = [](std::size_t, float, float) -> std::optional<float> { return 26.0f; };
    const auto empty = layoutAnnotations(640, 480, 0, options, fixed);
    require(empty && empty->width == 640 && empty->height == 480 && empty->rows.empty(),
            "an image with no notes gained a rail or changed size");
    const auto tiny = layoutAnnotations(9, 7, 1, options, fixed);
    require(tiny && tiny->height >= options.minimumCanvasHeight && tiny->fontSize == options.preferredFontSize,
            "tiny image did not grow to a readable notes canvas");
    checkRows(*tiny, 1);
    require(tiny->imageWidth == 9 && tiny->imageHeight == 7 && tiny->width == 9 + options.panelWidth,
            "tiny image was resized or the notes rail was overlaid on it");

    const auto adaptive = layoutAnnotations(640, 260, 4, options,
        [](std::size_t, float size, float) -> std::optional<float> { return size * 2.0f; });
    require(adaptive && adaptive->fontSize == 16.0f && adaptive->height == 260,
            "font fitting did not choose the largest fitting whole-pixel size");
    checkRows(*adaptive, 4);
    // These ten one-line notes cannot fit 480px with the original 32px badges
    // and 16px gaps, regardless of body font. Compact geometry must let a
    // smaller but legible font keep the export at the source image's height.
    const auto dense = layoutAnnotations(640, 480, 10, options,
        [](std::size_t, float size, float) -> std::optional<float> { return size * 1.2f; });
    require(dense && dense->height == 480 && dense->columns == 1 &&
            dense->fontSize < options.preferredFontSize && dense->fontSize >= options.minimumFontSize,
            "dense short notes expanded the image before compact font fitting");
    require(dense->badgeDiameter < options.badgeDiameter && dense->badgeDiameter >= 24.0f &&
            dense->badgeFontSize >= 12.0f && dense->rowGap < options.rowGap && dense->rowGap >= 8.0f,
            "compact fitting did not preserve readable badges and row spacing");
    checkRows(*dense, 10);
    const auto tall = layoutAnnotations(640, 120, 20, options,
        [](std::size_t i, float size, float) -> std::optional<float> {
            return size * (i % 3 == 0 ? 8.0f : 2.0f);
        });
    require(tall && tall->fontSize == options.minimumFontSize && tall->height > 240 && tall->columns == 1,
            "long notes were clipped instead of growing the canvas at the minimum font");
    checkRows(*tall, 20);
    for (const auto& row : tall->rows)
        require(row.height >= tall->fontSize * (row.index % 3 == 0 ? 8.0f : 2.0f),
                "variable-height note was assigned too little space");

    auto limited = options;
    limited.maximumDimension = 1024;
    const auto columns = layoutAnnotations(200, 100, 30, limited, fixed);
    require(columns && columns->columns == 2 && columns->width <= 1024 && columns->height <= 1024,
            "notes exceeding a GPU height limit did not continue in another column");
    checkRows(*columns, 30);
    const auto many = layoutAnnotations(640, 360, 512, options, fixed);
    require(many && many->columns > 1, "a large note count was arbitrarily rejected");
    checkRows(*many, 512);

    auto budget = options;
    budget.maximumBytes = 460u * 180u * 4u;
    const auto compact = layoutAnnotations(100, 100, 1, budget, fixed);
    require(compact && compact->height <= 180 && compact->height >= 100,
            "preferred padding height rejected an otherwise complete budget-fitting rail");
    checkRows(*compact, 1);
    budget.maximumBytes = 640u * 480u * 4u;
    require(!layoutAnnotations(640, 480, 1, budget, fixed), "pixel allocation budget was ignored");
    require(!layoutAnnotations(0, 10, 1, options, fixed), "zero-width source was accepted");
    require(!layoutAnnotations((std::numeric_limits<std::uint32_t>::max)(), 1, 1, options, fixed),
            "overflowing source dimensions were accepted");
    require(!layoutAnnotations(100, 100, 1, options,
        [](std::size_t, float, float) -> std::optional<float> { return (std::numeric_limits<float>::infinity)(); }),
            "nonfinite measurement was accepted");
    require(!layoutAnnotations(100, 100, 1, options,
        [](std::size_t, float, float) -> std::optional<float> { return 100000.0f; }),
            "one unrenderably tall note was silently clipped");
    // A row too tall at the preferred size can still become renderable at the
    // minimum size; the first measurement must not prematurely abort fitting.
    auto shrink = options;
    shrink.maximumDimension = 1000;
    const auto recovered = layoutAnnotations(100, 100, 1, shrink,
        [](std::size_t, float size, float) -> std::optional<float> { return size * 70.0f; });
    require(recovered && recovered->fontSize == 12.0f, "oversized preferred text was not retried at smaller fonts");
    checkRows(*recovered, 1);

    std::cout << "annotations: stable history identities, 256 unique readable colors, adaptive/extended layouts and limits passed\n";
}
