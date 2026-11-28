#pragma once

#include <algorithm>

namespace HuhuCapture::UI {

struct AnnotationPanelGeometry {
    float listTop{}, listBottom{};
    float editorHeadingTop{}, editorTop{}, editorHeight{};
    float footerTop{};
};

// Physical coordinates within the rail. Its owner guarantees at least 240
// logical pixels of height, plus any footer growth beyond the original 22.
// Even there, the 40-pixel list, delete heading and 72-pixel editor have separate
// hit areas; a row can never trigger Delete. The optional footer is in DIP.
inline AnnotationPanelGeometry annotationPanelGeometry(float height, float dpi,
    float footerLogicalHeight = 22.f)
{
    const auto footerHeight = (std::max)(22.f, footerLogicalHeight) * dpi;
    const auto contentHeight = height - (footerHeight - 22.f * dpi);
    const auto editorHeight = std::clamp(contentHeight * .27f, 72.f * dpi, 150.f * dpi);
    const auto editorTop = height - editorHeight - footerHeight - 10.f * dpi;
    const auto headingTop = editorTop - 34.f * dpi;
    return {58.f * dpi, headingTop - 4.f * dpi, headingTop,
        editorTop, editorHeight, height - footerHeight - 3.f * dpi};
}

} // namespace HuhuCapture::UI
