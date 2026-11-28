#include "platform/DesktopPointer.h"

#include <QGuiApplication>
#include <QThread>
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <limits>

namespace HuhuCapture::Platform {
bool requestDesktopPointerMove(QPoint point)
{
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread() ||
        QGuiApplication::platformName() != QStringLiteral("xcb") ||
        point.x() < 0 || point.y() < 0 || point.x() > (std::numeric_limits<qint16>::max)() ||
        point.y() > (std::numeric_limits<qint16>::max)()) return false;
    auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!native || !native->connection() || !native->display()) return false;
    const auto root = static_cast<xcb_window_t>(DefaultRootWindow(native->display()));
    const auto request = xcb_warp_pointer(native->connection(), XCB_NONE, root, 0, 0, 0, 0,
        static_cast<qint16>(point.x()), static_cast<qint16>(point.y()));
    return request.sequence != 0 && xcb_flush(native->connection()) > 0;
}
}
