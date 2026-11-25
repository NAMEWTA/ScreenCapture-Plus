#pragma once

#include "editor/ToolSettings.h"
#include "services/rendering/QtAnnotationRenderer.h"

#include <QColor>
#include <QMetaType>
#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace HuhuCapture::UI::Qt {

enum class EditorTool {
    None, Rectangle, Ellipse, Arrow, Number, Annotation, Line, Text, Mosaic, Eraser
};

inline constexpr std::array editorTools{
    EditorTool::Rectangle, EditorTool::Ellipse, EditorTool::Arrow,
    EditorTool::Number, EditorTool::Annotation, EditorTool::Line,
    EditorTool::Text, EditorTool::Mosaic, EditorTool::Eraser
};

std::wstring_view toolId(EditorTool tool) noexcept;
QString toolLanguageKey(EditorTool tool);
EditorTool toolFromId(QStringView id);

// Preferences are logical pixels and 0xRRGGBBAA, matching the existing config.
// A renderable shape captures source-pixel sizes; editing preferences never
// retroactively restyles old shapes except the text currently being edited.
struct EditorToolStyle {
    float value{2.f};
    std::uint32_t rgba{Editor::toolColors.front()};
    bool filled{}, semiTransparent{}, bold{}, italic{}, rectangleMode{};
    bool operator==(const EditorToolStyle&) const = default;
};

EditorToolStyle defaultToolStyle(EditorTool tool) noexcept;
EditorToolStyle normalizedToolStyle(EditorTool tool, EditorToolStyle style) noexcept;
QColor editorColor(std::uint32_t rgba) noexcept;

enum class OutputIntent { Archive, SaveAs, Clipboard, Pin };
enum class OutputResult { Succeeded, Cancelled, Failed };

struct EditorFailure {
    enum class Code { None, InvalidImage, ImageAlreadyLoaded, RenderFailed, InvalidOperation, OutOfMemory };
    Code code{Code::None};
    QtAnnotationRenderer::Status rendering{QtAnnotationRenderer::Status::Ok};
    explicit operator bool() const noexcept { return code != Code::None; }
};

using EditorTranslator = std::function<QString(QStringView key)>;

} // namespace HuhuCapture::UI::Qt

Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::EditorTool)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::EditorToolStyle)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::OutputIntent)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::OutputResult)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::EditorFailure)
