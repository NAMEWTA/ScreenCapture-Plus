#include "platform/DesktopPointer.h"

#include <QGuiApplication>
#include <QThread>
#include <windows.h>

namespace HuhuCapture::Platform {
bool requestDesktopPointerMove(QPoint point)
{
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread() ||
        QGuiApplication::platformName() != QStringLiteral("windows")) return false;
    // Qt's per-monitor-aware native desktop uses physical SetCursorPos units.
    return SetCursorPos(point.x(), point.y()) != FALSE;
}
}
