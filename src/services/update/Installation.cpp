#include "services/update/Installation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>

namespace HuhuCapture::Updates {

Installation readInstallation(const QString& applicationDirectory, const QString& currentVersion,
    QStringView platform, QStringView architecture)
{
    Installation result;
    if (!QDir::isAbsolutePath(applicationDirectory) || applicationDirectory.contains(QChar::Null) ||
        !Core::canonicalUpdateVersion(currentVersion.toStdWString())) {
        result.error = InstallationError::Metadata; return result;
    }
    const auto directory = QDir(applicationDirectory).canonicalPath();
    if (directory.isEmpty()) return result;
    const std::array candidates{
        QDir(directory).filePath(QStringLiteral("installation.json")),
        QDir(directory).filePath(QStringLiteral("../share/huhucapture/installation.json"))};
    for (const auto& path : candidates) {
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) continue;
        result.markerPath = QDir::cleanPath(path);
        result.error = InstallationError::Metadata;
        if (!info.isFile() || info.isSymLink()) return result;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { result.error = InstallationError::Read; return result; }
        constexpr qsizetype limit = 16 * 1024;
        if (file.size() <= 0 || file.size() > limit) return result;
        const auto bytes = file.read(limit + 1);
        if (file.error() != QFileDevice::NoError) { result.error = InstallationError::Read; return result; }
        if (bytes.isEmpty() || bytes.size() > limit || !file.atEnd()) return result;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return result;
        const auto object = document.object();
        if (object.value(QStringLiteral("schemaVersion")) != QJsonValue(1) ||
            object.value(QStringLiteral("product")).toString() != QStringLiteral("huhucapture") ||
            object.value(QStringLiteral("platform")).toString() != platform ||
            object.value(QStringLiteral("architecture")).toString() != architecture) return result;
        const auto version = object.value(QStringLiteral("version"));
        if (!version.isUndefined() && (!version.isString() || version.toString() != currentVersion)) return result;
        const auto packageKind = object.value(QStringLiteral("packageKind")).toString();
        for (const auto kind : {Core::UpdatePackageKind::WindowsInstaller, Core::UpdatePackageKind::WindowsPortableZip,
                              Core::UpdatePackageKind::LinuxDeb, Core::UpdatePackageKind::LinuxAppImage}) {
            const auto policy = Core::updatePackagePolicy(kind);
            const auto string = [](std::string_view value) {
                return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
            };
            if (packageKind != string(policy.kind) || platform != string(policy.platform) ||
                architecture != string(policy.architecture)) continue;
            result.packageKind = kind; result.error = InstallationError::None; return result;
        }
        return result;
    }
    return result;
}

} // namespace HuhuCapture::Updates
