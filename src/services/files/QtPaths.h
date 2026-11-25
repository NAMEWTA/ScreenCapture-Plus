#pragma once

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
    return QString::fromStdWString(path.native());
#else
    return QString::fromUtf8(path.native().data(), static_cast<qsizetype>(path.native().size()));
#endif
}

} // namespace HuhuCapture::Files
