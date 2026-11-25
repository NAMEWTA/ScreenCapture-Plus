#pragma once

#include <cstdint>

// The original settings/history values are shared by the migration baseline
// and Qt presentation. These are logical pixels; native adapters scale once.
namespace HuhuCapture::UI::SettingsMetrics {
inline constexpr std::uint32_t canvas = 0xF5F5F7FF;
inline constexpr std::uint32_t surface = 0xFFFFFFFF;
inline constexpr std::uint32_t ink = 0x1D1D1FFF;
inline constexpr std::uint32_t secondary = 0x636366FF;
inline constexpr std::uint32_t hairline = 0xE0E0E0FF;
inline constexpr std::uint32_t blue = 0x0066CCFF;
inline constexpr std::uint32_t focusBlue = 0x0071E3FF;
inline constexpr std::uint32_t blueTint = 0xEAF3FFFF;
inline constexpr std::uint32_t warning = 0xB42318FF;
inline constexpr float bodySize = 14.f;
inline constexpr float bodyLineHeight = 20.f;
inline constexpr float captionSize = 12.f;
inline constexpr float captionLineHeight = 18.f;
inline constexpr float headingSize = 22.f;
inline constexpr float headingLineHeight = 30.f;
inline constexpr float controlHeight = 32.f;
inline constexpr float controlRadius = 8.f;
inline constexpr float rowHeight = 56.f;
inline constexpr float singleRowHeight = 48.f;
inline constexpr float cardRadius = 12.f;
inline constexpr float cardInset = 18.f;
inline constexpr float groupGap = 16.f;
inline constexpr float windowWidth = 720.f;
inline constexpr float windowHeight = 500.f;
inline constexpr float sidebarWidth = 160.f;
inline constexpr float pageMaximumWidth = 560.f;
inline constexpr float pageInset = 24.f;
} // namespace HuhuCapture::UI::SettingsMetrics
