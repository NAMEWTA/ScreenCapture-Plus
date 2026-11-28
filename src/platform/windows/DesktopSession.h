#pragma once

#include "services/desktop/DesktopResult.h"

namespace HuhuCapture::Platform::Windows {

struct DesktopSessionResult {
    QString key;
    Desktop::Failure failure;
};

// Borrow the current process's station/desktop handles; never close them or
// switch desktops. A per-user pipe still needs the real login session scope.
DesktopSessionResult desktopSession();

} // namespace HuhuCapture::Platform::Windows
