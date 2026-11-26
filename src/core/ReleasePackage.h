#pragma once

#include "core/ReleaseVersion.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace HuhuCapture::Core::ReleasePackage {

// Transport-independent contract shared by the temporary WinRT bridge and the
// Qt updater. JSON parsing remains in their mature native JSON adapters.
inline constexpr std::uint64_t metadataBudget = 1024 * 1024;
inline constexpr std::uint64_t buildInfoBudget = 8 * 1024 * 1024;
inline constexpr std::uint64_t legacyBudget = 64 * 1024 * 1024;
inline constexpr std::uint64_t installerBudget = 1024ull * 1024 * 1024;
inline constexpr std::uint64_t archiveBudget = 2ull * 1024 * 1024 * 1024;

inline bool canonicalVersion(std::wstring_view value, bool tag = false)
{
    const auto parsed = parseReleaseVersion(value);
    if (!parsed) return false;
    const auto expected = std::to_wstring((*parsed)[0]) + L"." +
        std::to_wstring((*parsed)[1]) + L"." + std::to_wstring((*parsed)[2]);
    return value == (tag ? L"v" + expected : expected);
}

inline bool lowerHex(std::wstring_view value, std::size_t length)
{
    return value.size() == length && std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}

inline bool assetName(std::wstring_view value)
{
    return !value.empty() && value.size() <= 256 && value != L"." && value != L".." &&
        std::all_of(value.begin(), value.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_';
        });
}

inline bool relativeFile(std::wstring_view value)
{
    if (value.empty() || value.size() > 4096 || value.starts_with(L'/') ||
        value.find_first_of(L"\\:") != value.npos || value.find(L'\0') != value.npos) return false;
    while (true) {
        const auto slash = value.find(L'/');
        const auto part = value.substr(0, slash);
        if (part.empty() || part == L"." || part == L"..") return false;
        if (slash == value.npos) return true;
        value.remove_prefix(slash + 1);
    }
}

inline bool byteCount(double number, std::uint64_t maximum, std::uint64_t& result, bool empty = false)
{
    if (!std::isfinite(number) || number < (empty ? 0 : 1) ||
        number > static_cast<double>(maximum) || std::floor(number) != number) return false;
    result = static_cast<std::uint64_t>(number);
    return true;
}

} // namespace HuhuCapture::Core::ReleasePackage
