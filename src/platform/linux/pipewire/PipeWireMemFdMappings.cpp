/* MemFd ownership uses PipeWire 1.6.11's public mempool API and its existing
 * mapping/range implementation (src/pipewire/mem.c and mem.h, MIT).
 * Fixed source, copyright and adaptation boundaries: docs/linux-pipewire.md.
 */
#include "platform/linux/pipewire/PipeWireMemFdMappings.h"

#include <pipewire/mem.h>
#include <spa/buffer/buffer.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace HuhuCapture::Platform::Linux::Detail {
namespace {
int nativeError() noexcept { return errno > 0 ? -errno : -EIO; }
}

PipeWireMemFdMappings::PipeWireMemFdMappings(std::size_t maximumPlaneBytes) noexcept
{
    if (maximumPlaneBytes <= (std::numeric_limits<std::uint32_t>::max)()) {
        maximumPlaneBytes_ = static_cast<std::uint32_t>(maximumPlaneBytes);
        maximumMappedBytes_ = std::uint64_t{maximumPlaneBytes_} * maximumBuffers;
    }
    const auto pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize > 0 && static_cast<unsigned long>(pageSize) <=
            (std::numeric_limits<std::uint32_t>::max)())
        pageSize_ = static_cast<std::uint32_t>(pageSize);
}

PipeWireMemFdMappings::~PipeWireMemFdMappings()
{
    // Capture explicitly checks clear() before destroying the core/pool. This
    // last-resort RAII cleanup also protects partial construction and tests.
    (void)clear();
    if (pool_) spa_hook_remove(&poolListener_);
}

PipeWireMemFdMappings::Result PipeWireMemFdMappings::read(
    pw_mempool* pool, const pw_buffer* buffer, const spa_data& data) noexcept
{
    if (cleanupError_ < 0)
        return {{}, cleanupError_, "PipeWire failed to release a previous MemFd mapping"};
    if (data.type != SPA_DATA_MemFd)
        return {{}, -ENOTSUP, "Only MemFd can use the legacy CPU mapping path"};
    if (!pool || !buffer || data.data || data.maxsize == 0 || data.fd < 0 ||
        data.fd > (std::numeric_limits<int>::max)())
        return {{}, -EINVAL, "Invalid unmapped PipeWire MemFd descriptor"};
    if (maximumPlaneBytes_ == 0 || pageSize_ == 0)
        return {{}, -EINVAL, "Invalid PipeWire MemFd mapping limits"};
    if (data.maxsize > maximumPlaneBytes_)
        return {{}, -ENOBUFS, "PipeWire MemFd exceeds the per-plane byte limit"};

    const int fd = static_cast<int>(data.fd);
    auto* block = pw_mempool_find_fd(pool, fd);
    if (!block || block->pool != pool || block->id == SPA_ID_INVALID ||
        block->fd != fd || block->type != SPA_DATA_MemFd)
        return {{}, -EINVAL, "PipeWire MemFd does not belong to the authorized core pool"};
    if ((block->flags & (PW_MEMBLOCK_FLAG_UNMAPPABLE | PW_MEMBLOCK_FLAG_DONT_NOTIFY)) != 0)
        return {{}, -ENOTSUP, "PipeWire MemFd is unmappable or cannot notify mapping revocation"};
    if (pool_ && pool_ != pool)
        return {{}, -EINVAL, "PipeWire MemFd belongs to a different core pool"};

    const int access = fcntl(fd, F_GETFL);
    if (access < 0) return {{}, nativeError(), "Cannot inspect PipeWire MemFd access"};
    if ((access & O_ACCMODE) == O_WRONLY || (access & O_PATH) != 0)
        return {{}, -EACCES, "PipeWire MemFd is not readable"};
    struct stat status{};
    if (fstat(fd, &status) != 0)
        return {{}, nativeError(), "Cannot inspect PipeWire MemFd extent"};
    if (!S_ISREG(status.st_mode) || status.st_size < 0)
        return {{}, -ENOTSUP, "PipeWire MemFd is not a regular shared-memory file"};
    const auto end = std::uint64_t{data.mapoffset} + data.maxsize;
    if (end > static_cast<std::uint64_t>(status.st_size))
        return {{}, -EINVAL, "PipeWire MemFd range exceeds the actual file extent"};

    auto existing = std::ranges::find_if(entries_, [buffer](const Entry& entry) {
        return entry.buffer == buffer;
    });
    if (existing != entries_.end()) {
        if (existing->pool != pool || existing->mapping->block != block || existing->fd != fd ||
            existing->offset != data.mapoffset || existing->size != data.maxsize)
            return {{}, -EINVAL, "PipeWire changed a live buffer's MemFd mapping descriptor"};
        return {static_cast<const std::uint8_t*>(existing->mapping->ptr), 0, nullptr};
    }

    pw_map_range range{};
    const int rangeError = pw_map_range_init(&range, data.mapoffset, data.maxsize, pageSize_);
    if (rangeError < 0)
        return {{}, rangeError, "PipeWire MemFd page-aligned range cannot be represented"};
    if (count_ == maximumBuffers || range.size > maximumMappedBytes_ - mappedBytes_)
        return {{}, -ENOBUFS, "PipeWire MemFd mappings exceed their buffer or aligned-byte limit"};

    if (!pool_) {
        static const pw_mempool_events events = [] {
            pw_mempool_events result{};
            result.version = PW_VERSION_MEMPOOL_EVENTS;
            result.removed = blockRemoved;
            result.destroy = poolDestroyed;
            return result;
        }();
        pw_mempool_add_listener(pool, &poolListener_, &events, this);
        pool_ = pool;
    }

    // The upstream API also checks fstat + a wide offset/size sum before mapping.
    // Our checks additionally restrict the FD kind/access and our own budget,
    // and are repeated when reusing a map before any pixel read.
    auto* mapping = pw_memblock_map(block, PW_MEMMAP_FLAG_READ, data.mapoffset, data.maxsize, nullptr);
    if (!mapping)
        return {{}, nativeError(), "PipeWire could not map the existing MemFd block read-only"};
    if (!mapping->ptr) {
        const int error = pw_memmap_free(mapping);
        return {{}, error < 0 ? error : -EFAULT, "PipeWire returned an empty MemFd mapping"};
    }
    const auto empty = std::ranges::find_if(entries_, [](const Entry& entry) { return !entry.buffer; });
    *empty = {buffer, pool, mapping, fd, data.mapoffset, data.maxsize, range.size};
    ++count_;
    mappedBytes_ += range.size;
    return {static_cast<const std::uint8_t*>(mapping->ptr), 0, nullptr};
}

int PipeWireMemFdMappings::release(Entry& entry) noexcept
{
    if (!entry.mapping) return 0;
    auto* mapping = entry.mapping;
    mappedBytes_ -= entry.mappedSize;
    --count_;
    entry = {};
    // Freeing the final map may synchronously emit pool.removed. Clear our
    // ownership first so that this callback cannot release the same map twice.
    const int result = pw_memmap_free(mapping);
    if (result < 0 && cleanupError_ == 0) cleanupError_ = result;
    return result;
}

void PipeWireMemFdMappings::blockRemoved(void* value, pw_memblock* block) noexcept
{
    auto& self = *static_cast<PipeWireMemFdMappings*>(value);
    // pw_memblock_free emits removed while block and maps are still valid, then
    // forcibly destroys any maps left behind, regardless of their references.
    for (auto& entry : self.entries_)
        if (entry.mapping && entry.mapping->block == block) (void)self.release(entry);
}

void PipeWireMemFdMappings::poolDestroyed(void* value) noexcept
{
    auto& self = *static_cast<PipeWireMemFdMappings*>(value);
    (void)self.clear();
    spa_hook_remove(&self.poolListener_);
    self.pool_ = nullptr;
}

int PipeWireMemFdMappings::remove(const pw_buffer* buffer) noexcept
{
    for (auto& entry : entries_)
        if (entry.buffer == buffer) return release(entry);
    return 0;
}

int PipeWireMemFdMappings::clear() noexcept
{
    for (auto& entry : entries_) (void)release(entry);
    return cleanupError_;
}

} // namespace HuhuCapture::Platform::Linux::Detail
