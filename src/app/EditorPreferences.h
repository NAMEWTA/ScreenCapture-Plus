#pragma once

#include "services/config/SettingsStore.h"
#include "ui/qt/EditorTypes.h"

namespace HuhuCapture::Application {

// The retained ToolSub/Setting toolPin schema is shared with the Qt editor.
// This adapter owns no settings, shapes or defaults. Reads do not rewrite the
// user's file; writes change only the attributes actually adjusted by a view.
UI::Qt::EditorToolStyle readEditorPreference(const Config::SettingsStore& settings,
    UI::Qt::EditorTool tool);
Config::SettingsResult writeEditorPreference(Config::SettingsStore& settings,
    UI::Qt::EditorTool tool, UI::Qt::EditorToolStyle previous,
    UI::Qt::EditorToolStyle current);

} // namespace HuhuCapture::Application
