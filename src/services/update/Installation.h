#pragma once

#include "core/UpdatePolicy.h"

#include <QString>
#include <QStringView>
#include <optional>

namespace HuhuCapture::Updates {

enum class InstallationError { None, NotFound, Read, Metadata };
struct Installation {
    std::optional<Core::UpdatePackageKind> packageKind;
    InstallationError error{InstallationError::NotFound};
    QString markerPath;
};

// Adapted from PR #7's installedPackage: inspect only the actual executable's
// deployment tree. Never discover a global marker or infer a package from cwd,
// APPIMAGE, the profile directory, a downloaded file, or a different install.
// An adjacent invalid marker stops discovery; it cannot silently fall through
// to a different marker. A supplied version must match the generated build.
Installation readInstallation(const QString& applicationDirectory, const QString& currentVersion,
    QStringView platform, QStringView architecture);

} // namespace HuhuCapture::Updates
