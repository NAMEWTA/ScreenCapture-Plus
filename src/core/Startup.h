#pragma once

#include <array>
#include <span>
#include <string_view>

namespace HuhuCapture::Core {

enum class CaptureEntry { Capture, Pin, Scroll, Recording, Qr };

struct StartupRequest {
    CaptureEntry entry{CaptureEntry::Capture};
    bool oneShot{};
    bool background{};
    friend constexpr bool operator==(const StartupRequest&, const StartupRequest&) = default;
};

namespace Detail {
template<class Character>
constexpr bool startupEquals(std::basic_string_view<Character> value, std::string_view ascii) noexcept
{
    if (value.size() != ascii.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (value[i] != static_cast<Character>(ascii[i])) return false;
    return true;
}
} // namespace Detail

// The shipped App / WinCap rules: only literal "true" enables a flag;
// one-shot capture takes precedence over auto-start and the tray entry.
// Unknown entry names retain the ordinary screenshot workflow.
template<class Character>
constexpr StartupRequest startupRequestFromValues(std::basic_string_view<Character> autoQuit,
    std::basic_string_view<Character> autoStart, std::basic_string_view<Character> entry) noexcept
{
    using Detail::startupEquals;
    StartupRequest result;
    result.oneShot = startupEquals(autoQuit, "true");
    result.background = !result.oneShot &&
        (startupEquals(autoStart, "true") || startupEquals(entry, "tray"));
    if (startupEquals(entry, "pin")) result.entry = CaptureEntry::Pin;
    else if (startupEquals(entry, "long")) result.entry = CaptureEntry::Scroll;
    else if (startupEquals(entry, "video")) result.entry = CaptureEntry::Recording;
    else if (startupEquals(entry, "qr") || startupEquals(entry, "qrcode")) result.entry = CaptureEntry::Qr;
    return result;
}

// Arguments exclude argv[0]. Preserve the fixed Ling::App::initArgs behavior:
// an explicit key=value replaces an earlier value; a later bare flag only
// inserts when that key is absent. Unknown arguments remain harmless. Qt's
// default command-line error policy would change these existing semantics.
template<class Character>
constexpr StartupRequest parseStartupArguments(
    std::span<const std::basic_string_view<Character>> arguments) noexcept
{
    using View = std::basic_string_view<Character>;
    constexpr std::array<std::string_view, 3> keys{"--auto-quit", "--auto-start", "--enter"};
    std::array<View, 3> values{};
    std::array<bool, 3> present{};
    constexpr Character trueText[]{'t', 'r', 'u', 'e'};
    for (const auto argument : arguments) {
        const auto separator = argument.find(static_cast<Character>('='));
        const bool explicitValue = separator != View::npos;
        const auto key = argument.substr(0, separator);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (!Detail::startupEquals(key, keys[index])) continue;
            if (explicitValue || !present[index]) {
                values[index] = explicitValue ? argument.substr(separator + 1) : View(trueText, 4);
                present[index] = true;
            }
            break;
        }
    }
    return startupRequestFromValues(values[0], values[1], values[2]);
}

} // namespace HuhuCapture::Core
