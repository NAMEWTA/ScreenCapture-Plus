#include "ui/AnnotationPanelLayout.h"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "annotation_panel_layout_tests:" << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check(!!(expression), #expression, __LINE__)

void compactAndTallPanelsKeepIndependentHitAreas()
{
    for (const auto dpi : {1.f, 1.25f, 1.5f, 1.75f, 2.f, 2.5f, 3.f}) {
        for (int logicalHeight = 240; logicalHeight <= 1200; ++logicalHeight) {
            const auto height = static_cast<float>(logicalHeight) * dpi;
            const auto geometry = HuhuCapture::UI::annotationPanelGeometry(height, dpi);
            CHECK(geometry.listTop >= 48.f * dpi);
            CHECK(geometry.listBottom - geometry.listTop + .001f >= 40.f * dpi);
            CHECK(geometry.listBottom < geometry.editorHeadingTop);
            CHECK(geometry.editorHeadingTop < geometry.editorTop);
            CHECK(geometry.editorHeight + .001f >= 72.f * dpi);
            CHECK(geometry.editorTop + geometry.editorHeight < geometry.footerTop);
            CHECK(geometry.footerTop < height);
            // A pointer anywhere within the list, including the last visible
            // pixel of a clipped row, must never reach the Delete heading.
            CHECK(geometry.listBottom - 1.f < geometry.editorHeadingTop);
        }
    }
}

} // namespace

int main()
{
    compactAndTallPanelsKeepIndependentHitAreas();
    for (const auto dpi : {1.f, 1.25f, 2.f, 3.f}) {
        float previous = 0.f;
        for (int logicalHeight = 240; logicalHeight <= 1200; ++logicalHeight) {
            const auto geometry = HuhuCapture::UI::annotationPanelGeometry(
                static_cast<float>(logicalHeight) * dpi, dpi);
            const auto listHeight = geometry.listBottom - geometry.listTop;
            CHECK(listHeight >= previous);
            previous = listHeight;
        }
    }
    std::cout << "annotation_panel_layout_tests: compact bounds and continuous resizing passed\n";
}
