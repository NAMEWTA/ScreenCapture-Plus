#pragma once

#include <cstdint>

namespace HuhuCapture::Core {

// Existing complete-GIF validation budget: six minutes at the retained 16 FPS,
// with final-frame headroom. Producers must fit the same bounded archive policy.
inline constexpr std::uint32_t maximumValidatedGifFrames = 6000;

} // namespace HuhuCapture::Core
