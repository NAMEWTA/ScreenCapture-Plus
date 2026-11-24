#pragma once

#include <algorithm>

namespace ScreenCapture::UI {

struct AnnotationPanelGeometry {
    float listTop{}, listBottom{};
    float editorHeadingTop{}, editorTop{}, editorHeight{};
    float footerTop{};
};

// Physical coordinates within the rail. Its owner guarantees at least 240
// logical pixels of height. Even there, the 40-pixel list, delete heading and
// 72-pixel editor have separate hit areas; a row can never trigger Delete.
inline AnnotationPanelGeometry annotationPanelGeometry(float height, float dpi)
{
    const auto editorHeight = std::clamp(height * .27f, 72.f * dpi, 150.f * dpi);
    const auto editorTop = height - editorHeight - 32.f * dpi;
    const auto headingTop = editorTop - 34.f * dpi;
    return {58.f * dpi, headingTop - 4.f * dpi, headingTop,
        editorTop, editorHeight, height - 25.f * dpi};
}

} // namespace ScreenCapture::UI
