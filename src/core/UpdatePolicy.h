#pragma once

#include "core/ReleaseVersion.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace HuhuCapture::Core {

// Common package naming and limits for the Qt updater and the retained Windows
// bridge adapter. Both consume this policy and Core::UpdateManifest; release
// acceptance remains a separate gate. JSON/HTTP adapters must not redefine package identity.
// Extracted from the project's reviewed Qt updater in PR #7, 251bc007.
enum class UpdatePackageKind { WindowsInstaller, WindowsPortableZip, LinuxDeb, LinuxAppImage };
struct UpdatePackagePolicy {
    std::string_view kind, platform, architecture, suffix;
    std::uint64_t maximumBytes{};
};
inline constexpr std::uint64_t updateMetadataLimit = 1024 * 1024;
inline constexpr std::uint64_t updateBuildInfoLimit = 8 * 1024 * 1024;
inline constexpr std::uint64_t updateMaximumPackage = 2ull * 1024 * 1024 * 1024;

constexpr UpdatePackagePolicy updatePackagePolicy(UpdatePackageKind kind) noexcept
{
    switch (kind) {
    case UpdatePackageKind::WindowsInstaller:
        return {"installer", "windows", "x64", "-windows-x64-setup.exe", 1024ull * 1024 * 1024};
    case UpdatePackageKind::WindowsPortableZip:
        return {"portable-zip", "windows", "x64", "-windows-x64.zip", updateMaximumPackage};
    case UpdatePackageKind::LinuxDeb:
        return {"deb", "linux", "amd64", "-linux-amd64.deb", 1024ull * 1024 * 1024};
    case UpdatePackageKind::LinuxAppImage:
        return {"appimage", "linux", "amd64", "-linux-amd64.AppImage", updateMaximumPackage};
    }
    return {};
}

inline bool canonicalUpdateVersion(std::wstring_view value, bool tag = false)
{
    const auto parsed = parseReleaseVersion(value);
    if (!parsed) return false;
    const auto expected = std::to_wstring((*parsed)[0]) + L'.' + std::to_wstring((*parsed)[1]) + L'.' + std::to_wstring((*parsed)[2]);
    return value == (tag ? L'v' + expected : expected);
}

inline bool updateAssetName(std::string_view value) noexcept
{
    return !value.empty() && value.size() <= 256 && value != "." && value != ".." &&
        std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        });
}

inline bool updateInventoryPath(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 4096 || value.front() == '/' ||
        value.find_first_of("\\:") != std::string_view::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) return false;
    for (;;) {
        const auto separator = value.find('/');
        const auto part = value.substr(0, separator);
        if (part.empty() || part == "." || part == "..") return false;
        if (separator == std::string_view::npos) return true;
        value.remove_prefix(separator + 1);
    }
}

} // namespace HuhuCapture::Core
