#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HuhuCapture::Core {

inline bool isShortcutModifier(std::wstring_view key) noexcept
{
    return key == L"Ctrl" || key == L"Alt" || key == L"Shift" ||
        key == L"Win" || key == L"LWin" || key == L"RWin";
}

inline void normalizeShortcutKeys(std::vector<std::wstring>& keys)
{
    // Both physical Windows/Meta keys denote the same registration modifier.
    // Canonicalizing them before validation also prevents Win+Win+L bypassing
    // the existing Win+L reservation when both physical keys are held.
    for (auto& key : keys) if (key == L"LWin" || key == L"RWin") key = L"Win";
    const auto rank = [](std::wstring_view key) {
        if (key == L"Ctrl") return 0;
        if (key == L"Alt") return 1;
        if (key == L"Shift") return 2;
        if (key == L"Win") return 3;
        return 4;
    };
    std::stable_sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    keys.erase(std::unique(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
        return isShortcutModifier(a) && a == b;
    }), keys.end());
}

inline std::wstring joinShortcutKeys(std::span<const std::wstring> values)
{
    std::vector<std::wstring> keys(values.begin(), values.end()); normalizeShortcutKeys(keys);
    std::wstring joined;
    for (const auto& key : keys) { if (!joined.empty()) joined += L'+'; joined += key; }
    return joined;
}

enum class ShortcutValidity { Valid, MissingKey, MultipleKeys, NeedsModifier, UnsupportedKey, Reserved };

// Original WinSettingShortcut policy. The adapter reports whether its actual
// registration API understands the normal key; pure policy never claims an OS
// shortcut is registered or available. Conflicts remain a service result.
inline ShortcutValidity validateShortcutKeys(std::span<const std::wstring> values, bool normalKeySupported)
{
    std::vector<std::wstring> keys(values.begin(), values.end()); normalizeShortcutKeys(keys);
    bool hasModifier = false;
    std::wstring_view normal;
    for (const auto& key : keys) {
        if (key.empty()) return ShortcutValidity::UnsupportedKey;
        if (isShortcutModifier(key)) { hasModifier = true; continue; }
        if (!normal.empty()) return ShortcutValidity::MultipleKeys;
        normal = key;
    }
    if (normal.empty()) return ShortcutValidity::MissingKey;
    const bool functionKey = normal.size() >= 2 && normal.front() == L'F' &&
        std::all_of(normal.begin() + 1, normal.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
    if (!hasModifier && !functionKey && normal != L"PrintScreen" && normal != L"ScrollLock" && normal != L"Pause")
        return ShortcutValidity::NeedsModifier;
    if (!normalKeySupported) return ShortcutValidity::UnsupportedKey;
    constexpr std::array<std::wstring_view, 18> reserved{
        L"Ctrl+C", L"Ctrl+V", L"Ctrl+X", L"Ctrl+Z", L"Ctrl+Y", L"Ctrl+A", L"Ctrl+S",
        L"Ctrl+Alt+Delete", L"Ctrl+Shift+Esc", L"Ctrl+Esc", L"Alt+Tab", L"Alt+Esc", L"Alt+F4",
        L"Win+L", L"Win+D", L"Win+E", L"Win+R", L"Win+Tab"};
    const auto canonical = joinShortcutKeys(keys);
    return std::find(reserved.begin(), reserved.end(), canonical) == reserved.end() ? ShortcutValidity::Valid : ShortcutValidity::Reserved;
}

} // namespace HuhuCapture::Core
