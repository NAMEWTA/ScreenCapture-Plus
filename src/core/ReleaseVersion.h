#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace HuhuCapture::Core {

// Windows VERSIONINFO stores each component in 16 bits. Stable releases use
// vMAJOR.MINOR.PATCH (or the same tag without v); prereleases are not auto-updated.
inline std::optional<std::array<int, 3>> parseReleaseVersion(std::wstring_view tag)
{
    if (tag.starts_with(L"v")) tag.remove_prefix(1);
    std::array<int, 3> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto separator = tag.find(L'.');
        const auto part = tag.substr(0, separator);
        if (part.empty() || part.size() > 5) return std::nullopt;
        int value = 0;
        for (const auto digit : part) {
            if (digit < L'0' || digit > L'9') return std::nullopt;
            value = value * 10 + static_cast<int>(digit - L'0');
        }
        if (value > 65535) return std::nullopt;
        result[i] = value;
        if (i + 1 < result.size()) {
            if (separator == std::wstring_view::npos) return std::nullopt;
            tag.remove_prefix(separator + 1);
        }
        else if (separator != std::wstring_view::npos) return std::nullopt;
    }
    return result;
}

} // namespace HuhuCapture::Core
