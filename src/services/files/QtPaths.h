#pragma once

#include <QDir>
#include <QString>
#include <filesystem>

namespace HuhuCapture::Files {

// The same UTF-16 Windows / UTF-8 Linux mapping already used by SettingsStore.
inline std::filesystem::path nativePath(const QString& path)
{
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}

inline QString qtPath(const std::filesystem::path& path)
{
#ifdef _WIN32
    // Qt paths use forward separators. Keep this representation consistent
    // with QDir/QFileInfo values while retaining the exact Unicode filename.
    return QDir::fromNativeSeparators(QString::fromStdWString(path.native()));
#else
    return QString::fromUtf8(path.native().data(), static_cast<qsizetype>(path.native().size()));
#endif
}

} // namespace HuhuCapture::Files
