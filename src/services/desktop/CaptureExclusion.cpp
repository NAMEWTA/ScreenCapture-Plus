#include "services/desktop/CaptureExclusion.h"

#include <QGuiApplication>
#include <QThread>
#include <QWindow>

#ifdef Q_OS_WIN
#include "platform/windows/CaptureExclusion.h"
#endif

namespace HuhuCapture::Desktop {

CaptureExclusionResult excludeFromCapture(QWindow* window)
{
    if (!qGuiApp || QThread::currentThread() != qGuiApp->thread() || !window ||
        window->thread() != qGuiApp->thread() || !window->handle())
        return {false, {Error::Unavailable, 0, QStringLiteral("The capture control has no native window")}};
#ifdef Q_OS_WIN
    const auto result = Platform::Windows::excludeFromCapture(reinterpret_cast<HWND>(window->winId()));
    if (result.status == Platform::Windows::CaptureExclusionStatus::Applied) return {true, {}};
    return {false, {Error::Unavailable, static_cast<qint64>(result.nativeError),
        QStringLiteral("This Windows desktop could not exclude the recording control")}};
#else
    return {false, {Error::Unavailable, 0,
        QStringLiteral("This desktop has no application window exclusion capability")}};
#endif
}

} // namespace HuhuCapture::Desktop
