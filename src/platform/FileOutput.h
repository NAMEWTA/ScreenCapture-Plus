#pragma once

#include <cstdint>
#include <span>
#include <system_error>

namespace HuhuCapture::Platform {

// Borrows the caller's already opened regular-file descriptor. These
// I/O operations never reopen a name or close the descriptor. One worker owns
// its access; each call reports real short-write/flush failures.
std::error_code writeFileDescriptor(int descriptor, std::span<const std::uint8_t> bytes) noexcept;
std::error_code flushFileDescriptor(int descriptor) noexcept;
std::error_code seekFileDescriptor(int descriptor, std::uint64_t absolutePosition) noexcept;

} // namespace HuhuCapture::Platform
