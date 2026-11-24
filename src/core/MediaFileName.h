#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace HuhuCapture::Core {

struct MediaFileName {
    std::wstring timestamp;
    std::uint32_t sequence{};
    std::wstring extension;
};

// Only this narrow, application-owned namespace is listed or cleared. Paths,
// nested folders, temporary files and unrelated user documents are excluded.
inline std::optional<MediaFileName> parseMediaFileName(std::wstring_view name)
{
    const auto dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos || dot < 15 || name[8] != L'-') return {};
    auto extension = std::wstring(name.substr(dot + 1));
    if (extension != L"png" && extension != L"gif" && extension != L"mp4") return {};
    const auto digits = [](std::wstring_view value) {
        return !value.empty() && std::all_of(value.begin(), value.end(),
            [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    };
    if (!digits(name.substr(0, 8)) || !digits(name.substr(9, 6))) return {};
    const auto number = [&](std::size_t at, std::size_t count) {
        unsigned value = 0;
        for (auto c : name.substr(at, count)) value = value * 10 + unsigned(c - L'0');
        return value;
    };
    const auto year = number(0, 4), month = number(4, 2), day = number(6, 2);
    if (!year || month < 1 || month > 12 || day < 1 || number(9, 2) > 23 ||
        number(11, 2) > 59 || number(13, 2) > 59) return {};
    constexpr unsigned days[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month - 1] + (month == 2 && leap ? 1 : 0)) return {};
    std::uint32_t sequence = 0;
    if (dot != 15) {
        if (dot < 19 || dot > 26 || name[15] != L'-' || !digits(name.substr(16, dot - 16))) return {};
        for (auto c : name.substr(16, dot - 16)) {
            const auto digit = static_cast<unsigned>(c - L'0');
            if (sequence > (UINT32_MAX - digit) / 10) return {};
            sequence = sequence * 10 + digit;
        }
        if (sequence == 0) return {};
    }
    return MediaFileName{std::wstring(name.substr(0, 15)), sequence, std::move(extension)};
}

inline std::wstring makeMediaFileName(std::wstring_view timestamp,
    std::wstring_view extension, std::uint32_t sequence = 0)
{
    std::wostringstream result;
    result << timestamp;
    if (sequence) result << L'-' << std::setfill(L'0') << std::setw(3) << sequence;
    result << L'.' << extension;
    const auto name = result.str();
    return parseMediaFileName(name) ? name : std::wstring{};
}

inline bool newerMediaFileName(std::wstring_view left, std::wstring_view right)
{
    const auto a = parseMediaFileName(left), b = parseMediaFileName(right);
    if (!a || !b) return left > right;
    if (a->timestamp != b->timestamp) return a->timestamp > b->timestamp;
    if (a->sequence != b->sequence) return a->sequence > b->sequence;
    return a->extension > b->extension;
}

} // namespace HuhuCapture::Core
