#include "ui/qt/EditorTypes.h"

#include <algorithm>
#include <cmath>

namespace HuhuCapture::UI::Qt {

std::wstring_view toolId(EditorTool tool) noexcept
{
    switch (tool) {
    case EditorTool::Rectangle: return L"rect";
    case EditorTool::Ellipse: return L"ellipse";
    case EditorTool::Arrow: return L"arrow";
    case EditorTool::Number: return L"number";
    case EditorTool::Annotation: return L"annotation";
    case EditorTool::Line: return L"line";
    case EditorTool::Text: return L"text";
    case EditorTool::Mosaic: return L"mosaic";
    case EditorTool::Eraser: return L"eraser";
    default: return {};
    }
}

QString toolLanguageKey(EditorTool tool)
{
    const auto id = toolId(tool);
    return id.empty() ? QString() : QStringLiteral("tool.") + QString::fromWCharArray(id.data(), static_cast<qsizetype>(id.size()));
}

EditorTool toolFromId(QStringView id)
{
    for (const auto tool : editorTools) {
        const auto value = toolId(tool);
        if (id == QString::fromWCharArray(value.data(), static_cast<qsizetype>(value.size()))) return tool;
    }
    return EditorTool::None;
}

EditorToolStyle defaultToolStyle(EditorTool tool) noexcept
{
    EditorToolStyle style;
    if (const auto* slider = Editor::findToolSlider(toolId(tool))) style.value = slider->def;
    style.filled = tool == EditorTool::Arrow || tool == EditorTool::Number;
    return style;
}

EditorToolStyle normalizedToolStyle(EditorTool tool, EditorToolStyle style) noexcept
{
    if (const auto* slider = Editor::findToolSlider(toolId(tool)))
        style.value = std::isfinite(style.value) ? std::clamp(style.value, slider->min, slider->max) : slider->def;
    return style;
}

QColor editorColor(std::uint32_t rgba) noexcept
{
    return QColor(static_cast<int>((rgba >> 24) & 255), static_cast<int>((rgba >> 16) & 255),
        static_cast<int>((rgba >> 8) & 255), static_cast<int>(rgba & 255));
}

} // namespace HuhuCapture::UI::Qt
