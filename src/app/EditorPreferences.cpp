#include "app/EditorPreferences.h"

#include <algorithm>
#include <cmath>

namespace HuhuCapture::Application {
namespace {
namespace ui = UI::Qt;

QString name(std::wstring_view value)
{ return QString::fromWCharArray(value.data(), static_cast<qsizetype>(value.size())); }

bool hasColor(ui::EditorTool tool)
{
    return tool != ui::EditorTool::Mosaic && tool != ui::EditorTool::Eraser &&
        Editor::findToolSlider(ui::toolId(tool));
}

// These are the exact keys used by ToolSub::makeToggleBtn. A member pointer
// joins the existing UI value to its persisted name without a second model.
using Flag = bool ui::EditorToolStyle::*;
template<class Function> void flags(ui::EditorTool tool, Function function)
{
    switch (tool) {
    case ui::EditorTool::Rectangle: case ui::EditorTool::Ellipse:
    case ui::EditorTool::Arrow: case ui::EditorTool::Number:
        function(u"fill", &ui::EditorToolStyle::filled); break;
    case ui::EditorTool::Line:
        function(u"semiTransparent", &ui::EditorToolStyle::semiTransparent); break;
    case ui::EditorTool::Text:
        function(u"bold", &ui::EditorToolStyle::bold);
        function(u"italic", &ui::EditorToolStyle::italic); break;
    case ui::EditorTool::Mosaic: case ui::EditorTool::Eraser:
        function(u"rect", &ui::EditorToolStyle::rectangleMode); break;
    default: break;
    }
}

Config::SettingsResult invalid(const QString& detail)
{ return {Config::SettingsError::InvalidKey, detail, {}, {}}; }
}

ui::EditorToolStyle readEditorPreference(const Config::SettingsStore& settings, ui::EditorTool tool)
{
    auto result = ui::defaultToolStyle(tool);
    const auto* slider = Editor::findToolSlider(ui::toolId(tool));
    if (!slider) return result; // Annotation has no native ToolSub preferences.
    const auto stored = settings.value({QStringLiteral("toolPin"), name(slider->id)}).toObject();
    const auto value = stored.value(name(slider->key));
    if (value.isDouble() && std::isfinite(value.toDouble()))
        result.value = static_cast<float>(std::clamp(value.toDouble(),
            static_cast<double>(slider->min), static_cast<double>(slider->max)));
    if (hasColor(tool)) {
        const auto index = stored.value(QStringLiteral("colorIndex"));
        // Legacy colors are an index, not an RGBA number. Its float conversion
        // and truncation are retained for the valid palette range, with a
        // defined fallback before any out-of-range integer conversion.
        const auto numeric = index.isDouble() ? index.toDouble() : -1.;
        const auto number = std::isfinite(numeric) && numeric >= 0 && numeric < Editor::toolColors.size()
            ? static_cast<float>(numeric) : -1.f;
        if (std::isfinite(number) && number >= 0 && number < Editor::toolColors.size())
            result.rgba = Editor::toolColors[static_cast<std::size_t>(number)];
    }
    flags(tool, [&](QStringView key, Flag member) {
        const auto value = stored.value(key);
        if (value.isBool()) result.*member = value.toBool();
    });
    return result;
}

Config::SettingsResult writeEditorPreference(Config::SettingsStore& settings,
    ui::EditorTool tool, ui::EditorToolStyle previous, ui::EditorToolStyle current)
{
    const auto* slider = Editor::findToolSlider(ui::toolId(tool));
    if (!slider) return {};
    current = ui::normalizedToolStyle(tool, current);
    QJsonObject changes;
    if (previous.value != current.value) changes.insert(name(slider->key), current.value);
    if (hasColor(tool) && previous.rgba != current.rgba) {
        const auto color = std::find(Editor::toolColors.begin(), Editor::toolColors.end(), current.rgba);
        if (color == Editor::toolColors.end())
            return invalid(QStringLiteral("The selected tool color is outside the existing palette"));
        changes.insert(QStringLiteral("colorIndex"), static_cast<int>(color - Editor::toolColors.begin()));
    }
    flags(tool, [&](QStringView key, Flag member) {
        if (previous.*member != current.*member) changes.insert(key.toString(), current.*member);
    });
    if (changes.isEmpty()) return {};

    const auto group = settings.value({QStringLiteral("toolPin")}, QJsonValue::Undefined);
    if (!group.isUndefined() && !group.isObject())
        return invalid(QStringLiteral("The existing toolPin configuration is not an object; its original value was retained"));
    const auto id = name(slider->id);
    const auto stored = group.toObject().value(id);
    if (!stored.isUndefined() && !stored.isObject())
        return invalid(QStringLiteral("The existing tool preference is not an object; its original value was retained"));
    auto object = stored.toObject();
    for (auto item = changes.cbegin(); item != changes.cend(); ++item) object.insert(item.key(), item.value());
    return settings.setValue({QStringLiteral("toolPin"), id}, object);
}

} // namespace HuhuCapture::Application
