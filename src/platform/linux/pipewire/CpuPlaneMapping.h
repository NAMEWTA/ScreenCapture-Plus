/* Read-only MemFd mapping adapted from PipeWire 1.6.11 stream.c::map_data
 * and unmap_data; range arithmetic calls the official pw_map_range_init.
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 * SPDX-License-Identifier: MIT
 * Exact sources and additional extent checks: docs/linux-pipewire.md.
 */
#pragma once

#include <pipewire/mem.h>
#include <spa/buffer/buffer.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace HuhuCapture::Platform::Linux {

// A view valid only while the caller owns the dequeued native buffer. Neither
// the producer's descriptor nor spa_data is modified. Only this view's optional
// mmap is owned; destruction must precede pw_stream_queue_buffer().
class CpuPlaneMapping final {
public:
    CpuPlaneMapping(const spa_data& data, std::size_t maximumBytes) noexcept
    {
        if (data.type != SPA_DATA_MemFd && data.type != SPA_DATA_MemPtr) {
            reject(-ENOTSUP, "The native plane is not CPU MemFd or MemPtr storage");
            return;
        }
        if (data.maxsize == 0 || data.maxsize > maximumBytes) {
            reject(-EOVERFLOW, "The native CPU plane exceeds its per-frame byte limit");
            return;
        }
        if (data.type == SPA_DATA_MemPtr) {
            if (!data.data) reject(-EINVAL, "The native MemPtr plane has no address");
            else pixels_ = static_cast<const std::uint8_t*>(data.data);
            return;
        }
        if (data.fd < 0 || data.fd > (std::numeric_limits<int>::max)()) {
            reject(-EBADF, "The native MemFd plane has no valid descriptor");
            return;
        }
        // Check the very same borrowed open file description, not a path or a
        // descriptor reopened after validation. POSIX SHM (used by portal-wlr)
        // need not support memfd seals; stability while dequeued is the native
        // buffer ownership contract, not a claim about hostile file mutation.
        struct stat status{};
        if (::fstat(static_cast<int>(data.fd), &status) != 0) {
            reject(-errno, "Cannot inspect the native MemFd storage");
            return;
        }
        if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
            static_cast<std::uint64_t>(data.mapoffset) + data.maxsize >
                static_cast<std::uint64_t>(status.st_size)) {
            reject(-EOVERFLOW, "The native MemFd range extends beyond its actual storage");
            return;
        }
        if (data.data) {
            // MAP_BUFFERS may already have supplied a library-owned mapping.
            // Borrow it without unmapping it or changing producer flags.
            pixels_ = static_cast<const std::uint8_t*>(data.data);
            return;
        }
        const long pageSize = ::sysconf(_SC_PAGESIZE);
        if (pageSize <= 0 || static_cast<unsigned long>(pageSize) > UINT32_MAX ||
            (pageSize & (pageSize - 1)) != 0) {
            reject(-EINVAL, "Cannot determine a valid native mapping page size");
            return;
        }
        pw_map_range range{};
        const int result = pw_map_range_init(&range, data.mapoffset, data.maxsize,
            static_cast<std::uint32_t>(pageSize));
        if (result < 0) {
            reject(result, "The native MemFd page-aligned mapping range overflows");
            return;
        }
        mapping_ = ::mmap(nullptr, range.size, PROT_READ, MAP_SHARED,
            static_cast<int>(data.fd), range.offset);
        if (mapping_ == MAP_FAILED) {
            reject(-errno, "Cannot map the native MemFd plane read-only");
            return;
        }
        mappingBytes_ = range.size;
        pixels_ = static_cast<const std::uint8_t*>(mapping_) + range.start;
    }

    ~CpuPlaneMapping()
    {
        if (mapping_ != MAP_FAILED) ::munmap(mapping_, mappingBytes_);
    }
    CpuPlaneMapping(const CpuPlaneMapping&) = delete;
    CpuPlaneMapping& operator=(const CpuPlaneMapping&) = delete;

    const std::uint8_t* pixels() const noexcept { return pixels_; }
    int error() const noexcept { return error_; }
    const char* message() const noexcept { return message_; }

private:
    void reject(int error, const char* message) noexcept
    { error_ = error; message_ = message; }

    void* mapping_{MAP_FAILED};
    std::size_t mappingBytes_{};
    const std::uint8_t* pixels_{};
    int error_{};
    const char* message_{};
};

} // namespace HuhuCapture::Platform::Linux
