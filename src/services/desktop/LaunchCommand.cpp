#include "services/desktop/LaunchCommand.h"

#include <QDir>
#include <QFileInfo>

namespace HuhuCapture::Desktop {
namespace {
bool validText(const QString& value)
{
    return !value.isEmpty() && !value.contains(QChar(u'\0')) &&
        !value.contains(QChar(u'\n')) && !value.contains(QChar(u'\r'));
}
QString windowsArgument(const QString& argument)
{
    QString result(1, QChar(u'"')); qsizetype slashes{};
    for (const auto ch : argument) {
        if (ch == QChar(u'\\')) { ++slashes; continue; }
        if (ch == QChar(u'"')) result += QString(slashes * 2 + 1, QChar(u'\\'));
        else result += QString(slashes, QChar(u'\\'));
        slashes = 0; result += ch;
    }
    result += QString(slashes * 2, QChar(u'\\')); result += QChar(u'"');
    return result;
}
QString desktopArgument(const QString& argument)
{
    QString result(1, QChar(u'"'));
    for (const auto ch : argument) {
        if (ch == QChar(u'"') || ch == QChar(u'`') || ch == QChar(u'$') || ch == QChar(u'\\')) result += QChar(u'\\');
        result += ch;
        if (ch == QChar(u'%')) result += ch;
    }
    result += QChar(u'"'); return result;
}
}

bool persistentLaunchExecutable(const QString& executable)
{
    if (!validText(executable) || !QDir::isAbsolutePath(executable)) return false;
    const QFileInfo file(executable);
    if (!file.isFile() || !file.isExecutable()) return false;
#ifndef Q_OS_WIN
    // '=' is not allowed in the Exec executable token by Desktop Entry 1.5.
    if (executable.contains(QChar(u'='))) return false;
    const auto outer = qEnvironmentVariable("APPIMAGE");
    if (!outer.isEmpty()) {
        const QFileInfo image(outer);
        if (!QDir::isAbsolutePath(outer) || !image.isFile() || !image.isExecutable() ||
            image.canonicalFilePath() != file.canonicalFilePath()) return false;
    }
    const auto appDir = qEnvironmentVariable("APPDIR");
    if (!appDir.isEmpty()) {
        const auto mount = QFileInfo(appDir).canonicalFilePath();
        if (!mount.isEmpty() && file.canonicalFilePath().startsWith(mount + QChar(u'/'))) return false;
    }
#endif
    return true;
}

QString windowsLaunchCommand(const QString& executable, const QStringList& arguments)
{
    if (!validText(executable)) return {};
    QStringList encoded{windowsArgument(executable)};
    for (const auto& argument : arguments) {
        if (argument.contains(QChar(u'\0')) || argument.contains(QChar(u'\n')) || argument.contains(QChar(u'\r'))) return {};
        encoded.push_back(windowsArgument(argument));
    }
    return encoded.join(QChar(u' '));
}
QString desktopExecCommand(const QString& executable, const QStringList& arguments)
{
    if (!validText(executable) || executable.contains(QChar(u'='))) return {};
    QStringList encoded{desktopArgument(executable)};
    for (const auto& argument : arguments) {
        if (argument.contains(QChar(u'\0')) || argument.contains(QChar(u'\n')) || argument.contains(QChar(u'\r'))) return {};
        encoded.push_back(desktopArgument(argument));
    }
    return encoded.join(QChar(u' '));
}
QString shortcutLaunchCommand(const QString& executable)
{
    if (!persistentLaunchExecutable(executable)) return {};
#ifdef Q_OS_WIN
    return windowsLaunchCommand(QDir::toNativeSeparators(executable), {});
#else
    // The existing normal-start CLI requests a screenshot and forwards it to
    // the running instance. Single quotes make the displayed shell form safe
    // for spaces, Unicode, '$', '`' and literal percent signs.
    auto escaped = executable; escaped.replace(QChar(u'\''), QStringLiteral("'\\''"));
    return QChar(u'\'') + escaped + QChar(u'\'');
#endif
}

} // namespace HuhuCapture::Desktop

