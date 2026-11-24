#pragma once
#include <cstdint>
#include <string_view>

// Runtime identity is kept together so settings, links and updates stay consistent.
// Windows version resources and release packaging use the same public product name.
namespace HuhuCapture::Brand {

inline constexpr std::wstring_view name{ L"huhucapture" };
inline constexpr std::wstring_view author{ L"NAMEWTA" };
inline constexpr std::wstring_view repositoryLabel{ L"NAMEWTA/huhucapture" };
inline constexpr std::wstring_view repositoryUrl{ L"https://github.com/NAMEWTA/huhucapture" };
inline constexpr std::wstring_view authorUrl{ L"https://github.com/NAMEWTA" };
inline constexpr std::wstring_view languagesUrl{ L"https://github.com/NAMEWTA/huhucapture/tree/main/Lang" };
inline constexpr std::wstring_view releasesUrl{ L"https://github.com/NAMEWTA/huhucapture/releases" };
inline constexpr std::wstring_view latestReleaseApiUrl{ L"https://api.github.com/repos/NAMEWTA/huhucapture/releases/latest" };
inline constexpr std::wstring_view releaseAssetPrefix{ L"https://github.com/NAMEWTA/huhucapture/releases/download/" };
inline constexpr std::wstring_view executableName{ L"huhucapture.exe" };
inline constexpr std::wstring_view updateExecutableName{ L"huhucapture.update.exe" };
inline constexpr std::wstring_view updateProbeName{ L"huhucapture.update.probe" };

// Ling colors use RRGGBBAA. These are product chrome colors, separate from annotation colors.
inline constexpr std::uint32_t inkRgba{ 0x67381FFF };
inline constexpr std::uint32_t creamRgba{ 0xFFF2DEFF };
inline constexpr std::uint32_t surfaceRgba{ 0xFFFCF7FF };
inline constexpr std::uint32_t hoverRgba{ 0xFFE4BCFF };
inline constexpr std::uint32_t borderRgba{ 0x67381F66 };

} // namespace HuhuCapture::Brand
