#pragma once

#include <atomic>
#include <cstdint>

namespace HuhuCapture::Capture {
// One identity domain for frozen screenshots and authorized live sources.
// Neither an OS node/display ID nor an application's request token is a source.
inline std::uint64_t nextSourceIdentity() noexcept
{
    static std::atomic<std::uint64_t> sequence{};
    auto value = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!value) value = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    return value;
}
} // namespace HuhuCapture::Capture
