#pragma once

#include "platform/FileIdentity.h"
#include "services/desktop/DesktopResult.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <optional>
#include <functional>
#include <memory>
#include <stop_token>

namespace HuhuCapture::Platform::Linux {

inline constexpr char captureActionDesktopId[] = "io.github.NAMEWTA.huhucapture.desktop";
inline constexpr char captureActionName[] = "FinishCapture";
inline constexpr char captureActionProbeEnvironment[] = "HUHUCAPTURE_DESKTOP_CAPTURE_PROBE";

// GIO caches the effective desktop ID map. Subscribe on the GUI's actual GLib
// context before lookup so an asynchronously discovered higher-priority entry
// revokes a proof even if it appeared before QFileSystemWatcher was installed.
class CaptureActionMonitor final {
public:
    explicit CaptureActionMonitor(std::function<void()> changed);
    ~CaptureActionMonitor();
    CaptureActionMonitor(const CaptureActionMonitor&) = delete;
    CaptureActionMonitor& operator=(const CaptureActionMonitor&) = delete;
    bool available() const noexcept;
private:
    struct Private;
    std::unique_ptr<Private> d;
};

struct CaptureActionRegistration {
    QString desktopPath;
    QByteArray desktopSha256;
    FileSnapshot desktopIdentity;
    QString executable;
    FileSnapshot executableIdentity;
    QString portableDirectory;
    QString actionLabel;
    QStringList watchPaths;
    bool operator==(const CaptureActionRegistration&) const = default;
};
struct CaptureActionNativeResult {
    std::optional<CaptureActionRegistration> registration;
    Desktop::Failure failure;
};

// Worker-only bounded filesystem/GIO lookup. These functions neither create nor
// edit a user's desktop entries. The effective action must target the current
// persistent executable and exact profile; arbitrary wrapper scripts are not
// interpreted or trusted as equivalent executables.
CaptureActionNativeResult inspectCaptureAction(const QString& executable,
    const QString& portableDirectory, std::stop_token stop = {});
CaptureActionNativeResult launchCaptureActionProbe(const QString& executable,
    const QString& portableDirectory, const QByteArray& challenge, std::stop_token stop);

} // namespace HuhuCapture::Platform::Linux
