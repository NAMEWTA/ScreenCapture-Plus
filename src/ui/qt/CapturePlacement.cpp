#include "ui/qt/CapturePlacement.h"
#include "ui/qt/EditorWindow.h"

#include <QApplication>
#include <QThread>
#include <cmath>

namespace HuhuCapture::UI::Qt {

bool placeCapturedEditor(EditorWindow* editor, const Capture::ActionRequest& request)
{
    if (!editor || !qApp || QThread::currentThread() != qApp->thread() ||
        request.coordinates != Capture::CoordinateSpace::DesktopPixels ||
        !request.desktopRect || request.sourceRect.isEmpty() ||
        request.desktopRect->size() != request.sourceRect.size()) return false;
    const Capture::ScreenBinding* chosen{};
    qint64 largest{};
    for (const auto& binding : request.screens) {
        if (!binding.screen || binding.screen->name().compare(binding.nativeId,
#ifdef Q_OS_WIN
            ::Qt::CaseInsensitive
#else
            ::Qt::CaseSensitive
#endif
            ) != 0 || binding.logicalGeometry != QRectF(binding.screen->geometry()) ||
            binding.logicalWorkArea != QRectF(binding.screen->availableGeometry()) ||
            binding.logicalWorkArea.isEmpty() || binding.sourcePixels.isEmpty() ||
            !std::isfinite(binding.pixelsPerDipX) || binding.pixelsPerDipX <= 0 ||
            !std::isfinite(binding.pixelsPerDipY) || binding.pixelsPerDipY <= 0) continue;
        const auto intersection = request.sourceRect.intersected(binding.sourcePixels);
        const qint64 area = qint64(intersection.width()) * intersection.height();
        if (area <= 0) continue;
        if (binding.sourcePixels.contains(request.sourceRect.topLeft())) { chosen = &binding; break; }
        if (area > largest) { largest = area; chosen = &binding; }
    }
    if (!chosen) return false;
    const auto logical = chosen->logicalGeometry.topLeft() +
        Capture::screenLocalFromSource(*chosen, request.sourceRect.topLeft());
    if (!std::isfinite(logical.x()) || !std::isfinite(logical.y())) return false;
    return editor->applyInitialPlacement(chosen->screen, logical.toPoint());
}

} // namespace HuhuCapture::UI::Qt
