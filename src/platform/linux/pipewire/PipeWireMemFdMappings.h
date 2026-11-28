#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <spa/utils/hook.h>

struct pw_buffer;
struct pw_memblock;
struct pw_memmap;
struct pw_mempool;
struct spa_data;

namespace HuhuCapture::Platform::Linux::Detail {

// Private capture-worker ownership for legacy MemFd descriptors. The actual
// mappings and their references belong to the fixed PipeWire memory-pool API.
class PipeWireMemFdMappings {
public:
    static constexpr std::size_t maximumBuffers = 4;

    struct Result {
        const std::uint8_t* pixels{};
        int error{};
        const char* message{};
        explicit operator bool() const noexcept { return pixels != nullptr; }
    };

    explicit PipeWireMemFdMappings(std::size_t maximumPlaneBytes) noexcept;
    ~PipeWireMemFdMappings();
    PipeWireMemFdMappings(const PipeWireMemFdMappings&) = delete;
    PipeWireMemFdMappings& operator=(const PipeWireMemFdMappings&) = delete;

    // Only called while the buffer is dequeued on the owning PW worker. Neither
    // the data/chunk descriptor nor returned pixels may escape that process().
    Result read(pw_mempool* pool, const pw_buffer* buffer, const spa_data& data) noexcept;
    int remove(const pw_buffer* buffer) noexcept;
    int clear() noexcept;

    std::size_t count() const noexcept { return count_; }
    std::uint64_t mappedBytes() const noexcept { return mappedBytes_; }

private:
    struct Entry {
        const pw_buffer* buffer{}; // Identity only; never dereferenced after remove_buffer.
        pw_mempool* pool{};
        pw_memmap* mapping{};
        int fd{-1};
        std::uint32_t offset{};
        std::uint32_t size{};
        std::uint32_t mappedSize{};
    };

    int release(Entry& entry) noexcept;
    static void blockRemoved(void* value, pw_memblock* block) noexcept;
    static void poolDestroyed(void* value) noexcept;
    std::array<Entry, maximumBuffers> entries_{};
    pw_mempool* pool_{};
    spa_hook poolListener_{};
    std::uint32_t maximumPlaneBytes_{};
    std::uint32_t pageSize_{};
    std::uint64_t maximumMappedBytes_{};
    std::uint64_t mappedBytes_{};
    std::size_t count_{};
    int cleanupError_{};
};

} // namespace HuhuCapture::Platform::Linux::Detail
