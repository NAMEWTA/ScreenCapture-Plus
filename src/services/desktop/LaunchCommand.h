#pragma once

#include <QString>
#include <QStringList>

namespace HuhuCapture::Desktop {

// The host supplies the persistent outer APPIMAGE path when applicable.
// Invalid/mounted/relative paths never become a desktop or startup command.
bool persistentLaunchExecutable(const QString& executable);
QString windowsLaunchCommand(const QString& executable, const QStringList& arguments);
// Exec quoting before the Desktop Entry string escape layer. Pass this value
// to GKeyFile::set_string; it performs that second escaping layer exactly once.
QString desktopExecCommand(const QString& executable, const QStringList& arguments);
// Copyable command for a desktop's custom shortcut editor. No shell is run by
// this function or by the settings controller.
QString shortcutLaunchCommand(const QString& executable);

} // namespace HuhuCapture::Desktop
