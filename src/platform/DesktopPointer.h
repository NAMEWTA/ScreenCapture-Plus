#pragma once

#include <QPoint>

namespace HuhuCapture::Platform {
// GUI-thread native desktop seam. The caller supplies a verified physical
// desktop point, never Portal coordinates or a Qt DIP rounded through DPR.
// True means the OS request was submitted; actual pointer delivery is native.
bool requestDesktopPointerMove(QPoint desktopPixels);
}
