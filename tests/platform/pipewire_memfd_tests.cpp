#include "platform/linux/UniqueFd.h"
#include "platform/linux/pipewire/PipeWireMemFdMappings.h"

#include <pipewire/pipewire.h>
#include <pipewire/mem.h>
#include <spa/buffer/buffer.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

using HuhuCapture::Platform::Linux::UniqueFd;
using HuhuCapture::Platform::Linux::Detail::PipeWireMemFdMappings;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Runtime {
    Runtime() { pw_init(nullptr, nullptr); }
    ~Runtime() { pw_deinit(); }
};

struct NativePool {
    pw_mempool* value{pw_mempool_new(nullptr)};
    spa_hook listener{};
    std::size_t removed{};
    NativePool()
    {
        require(value != nullptr, "Create an actual PipeWire memory pool");
        static const pw_mempool_events events = [] {
            pw_mempool_events result{};
            result.version = PW_VERSION_MEMPOOL_EVENTS;
            result.removed = [](void* data, pw_memblock*) {
                ++static_cast<NativePool*>(data)->removed;
            };
            return result;
        }();
        pw_mempool_add_listener(value, &listener, &events, this);
    }
    ~NativePool() { destroy(); }
    void destroy()
    {
        if (!value) return;
        spa_hook_remove(&listener);
        pw_mempool_destroy(value);
        value = nullptr;
    }
    pw_memblock* importFd(int fd, std::uint32_t type = SPA_DATA_MemFd, unsigned extraFlags = 0)
    {
        // The test keeps its original FD, exactly as a separate SHM producer
        // does. The memory pool and mapping implementations are not replaced.
        auto* block = pw_mempool_import(value,
            static_cast<pw_memblock_flags>(PW_MEMBLOCK_FLAG_DONT_CLOSE | extraFlags), type, fd);
        require(block != nullptr, "Import the actual owned FD into a native memory pool");
        return block;
    }
};

UniqueFd memoryFile(std::uint64_t size)
{
    UniqueFd fd(memfd_create("huhucapture-pw-memfd-test", MFD_CLOEXEC));
    require(static_cast<bool>(fd), "Create an actual Linux memfd");
    require(size <= static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)()) &&
        ftruncate(fd.get(), static_cast<off_t>(size)) == 0, "Size the owned sparse memfd");
    return fd;
}

spa_data descriptor(int fd, std::uint32_t size, std::uint32_t offset = 0)
{
    spa_data data{};
    data.type = SPA_DATA_MemFd;
    data.fd = fd;
    data.maxsize = size;
    data.mapoffset = offset;
    // Match the observed legacy contract: no SPA flags and no mapped pointer.
    return data;
}

void writeBytes(int fd, std::uint64_t offset, const std::array<std::uint8_t, 4>& bytes)
{
    require(pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset)) ==
        static_cast<ssize_t>(bytes.size()), "Write actual shared-memory bytes");
}

bool readOnlyMapping(const void* pointer)
{
    // Inspect only this process's mapping permissions. Never print addresses.
    std::ifstream maps("/proc/self/maps");
    require(maps.good(), "Read the actual process memory-map permissions");
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream row(line);
        std::string range, permissions;
        row >> range >> permissions;
        const auto delimiter = range.find('-');
        if (delimiter == std::string::npos) continue;
        const auto first = std::stoull(range.substr(0, delimiter), nullptr, 16);
        const auto end = std::stoull(range.substr(delimiter + 1), nullptr, 16);
        if (address >= first && address < end)
            return permissions.size() >= 2 && permissions[0] == 'r' && permissions[1] == '-';
    }
    return false;
}

void realLegacyBufferAndReuse(std::uint32_t page)
{
    NativePool pool;
    auto fd = memoryFile(1'920'000);
    const std::array<std::uint8_t, 4> first{9, 31, 87, 255}, last{71, 19, 2, 255};
    writeBytes(fd.get(), 0, first);
    writeBytes(fd.get(), 1'920'000 - last.size(), last);
    auto* block = pool.importFd(fd.get());
    require(block->size == 0, "The real import has unknown size; do not rely on block.size");
    PipeWireMemFdMappings mappings(32u * 1024u * 1024u);
    pw_buffer buffer{};
    const auto data = descriptor(fd.get(), 1'920'000);
    const auto result = mappings.read(pool.value, &buffer, data);
    require(static_cast<bool>(result), "Map the observed 800x600 legacy MemFd descriptor");
    require(std::memcmp(result.pixels, first.data(), first.size()) == 0 &&
        std::memcmp(result.pixels + data.maxsize - last.size(), last.data(), last.size()) == 0,
        "Read actual first and last shared-memory bytes");
    require(readOnlyMapping(result.pixels), "The new native mapping has read-only kernel permissions");
    pw_map_range range{};
    require(pw_map_range_init(&range, data.mapoffset, data.maxsize, page) == 0 &&
        mappings.mappedBytes() == range.size && mappings.count() == 1,
        "Account for the actual page-aligned mapping extent");
    const int references = block->ref;
    const auto reused = mappings.read(pool.value, &buffer, data);
    require(reused && reused.pixels == result.pixels && block->ref == references && mappings.count() == 1,
        "Repeated frames reuse the existing native mapping without another reference");
    require(data.flags == 0 && data.data == nullptr, "Do not rewrite producer SPA flags or data");
    std::array<std::uint8_t, 4> owned{};
    std::memcpy(owned.data(), result.pixels, owned.size());
    require(mappings.remove(&buffer) == 0 && mappings.count() == 0 && mappings.mappedBytes() == 0 &&
        block->ref == 1, "remove_buffer releases the map and its native memory reference");
    require(owned == first && fcntl(fd.get(), F_GETFD) >= 0,
        "An owned pixel copy and the producer FD outlive mapping release");
}

void offsetAndDescriptorChanges(std::uint32_t page)
{
    NativePool pool;
    auto fd = memoryFile(std::uint64_t{page} * 4);
    pool.importFd(fd.get());
    PipeWireMemFdMappings mappings(std::size_t{page} * 2);
    pw_buffer buffer{};
    auto data = descriptor(fd.get(), page + 19, page + 37);
    const std::array<std::uint8_t, 4> expected{33, 88, 54, 11};
    writeBytes(fd.get(), data.mapoffset, expected);
    const auto mapped = mappings.read(pool.value, &buffer, data);
    require(mapped && std::memcmp(mapped.pixels, expected.data(), expected.size()) == 0 &&
        mappings.mappedBytes() == std::uint64_t{page} * 2,
        "Unaligned nonzero offsets use the official aligned range and adjusted pointer");
    ++data.mapoffset;
    require(!mappings.read(pool.value, &buffer, data) && mappings.count() == 1,
        "A live buffer cannot silently change its mapped extent");
    require(mappings.clear() == 0 && mappings.count() == 0, "Format retirement releases the old map");
    require(static_cast<bool>(mappings.read(pool.value, &buffer, data)),
        "A new generation can map its new descriptor after explicit retirement");
    require(ftruncate(fd.get(), page) == 0, "Truncate only the test's own memfd");
    require(!mappings.read(pool.value, &buffer, data),
        "A reused mapping rechecks actual file extent before reading pixels");
}

void typeAccessAndMembership(std::uint32_t page)
{
    auto fd = memoryFile(page);
    pw_buffer buffer{};
    auto data = descriptor(fd.get(), page);
    {
        NativePool pool;
        PipeWireMemFdMappings mappings(page);
        require(!mappings.read(pool.value, &buffer, data) && mappings.count() == 0,
            "An unknown FD is rejected instead of imported into the core pool");
        pool.importFd(fd.get(), SPA_DATA_DmaBuf);
        require(!mappings.read(pool.value, &buffer, data), "The existing block must also be MemFd");
    }
    for (unsigned flags : {unsigned(PW_MEMBLOCK_FLAG_UNMAPPABLE), unsigned(PW_MEMBLOCK_FLAG_DONT_NOTIFY)}) {
        NativePool pool;
        pool.importFd(fd.get(), SPA_DATA_MemFd, flags);
        PipeWireMemFdMappings mappings(page);
        require(!mappings.read(pool.value, &buffer, data) && mappings.count() == 0,
            "Explicitly unmappable or unobservable blocks cannot enter the fallback");
    }
    {
        NativePool pool;
        pool.importFd(fd.get());
        PipeWireMemFdMappings mappings(page);
        data.type = SPA_DATA_DmaBuf;
        data.flags = SPA_DATA_FLAG_MAPPABLE | SPA_DATA_FLAG_READABLE;
        require(!mappings.read(pool.value, &buffer, data), "DMA-BUF never enters the MemFd fallback");
        data.type = SPA_DATA_MemPtr;
        require(!mappings.read(pool.value, &buffer, data), "An empty MemPtr is not treated as a MemFd");
        data = descriptor(fd.get(), page);
    }
    for (int access : {O_RDONLY, O_WRONLY, O_PATH}) {
        const auto ownedPath = "/proc/self/fd/" + std::to_string(fd.get());
        UniqueFd reopened(open(ownedPath.c_str(), access | O_CLOEXEC));
        require(static_cast<bool>(reopened), "Reopen only the test's own memfd with actual access rights");
        NativePool pool;
        pool.importFd(reopened.get());
        PipeWireMemFdMappings mappings(page);
        const auto result = mappings.read(pool.value, &buffer, descriptor(reopened.get(), page));
        require(static_cast<bool>(result) == (access == O_RDONLY),
            "The actual open-file mode must permit reads; O_WRONLY and O_PATH fail");
    }
    {
        UniqueFd device(open("/dev/null", O_RDONLY | O_CLOEXEC));
        require(static_cast<bool>(device), "Open the real null device for non-regular FD rejection");
        NativePool pool;
        pool.importFd(device.get());
        PipeWireMemFdMappings mappings(page);
        require(!mappings.read(pool.value, &buffer, descriptor(device.get(), 1)),
            "A non-regular FD is rejected even when labelled MemFd");
    }
}

void rangeAndBudget(std::uint32_t page)
{
    NativePool pool;
    auto fd = memoryFile(std::uint64_t{page} * 3);
    pool.importFd(fd.get());
    std::array<pw_buffer, 5> buffers{};
    PipeWireMemFdMappings mappings(page);
    auto data = descriptor(fd.get(), page);
    for (std::size_t i = 0; i < 4; ++i)
        require(static_cast<bool>(mappings.read(pool.value, &buffers[i], data)), "Four bounded native buffers fit");
    require(!mappings.read(pool.value, &buffers[4], data) && mappings.count() == 4 &&
        mappings.mappedBytes() == std::uint64_t{page} * 4, "A fifth fallback buffer is rejected");
    require(mappings.remove(&buffers[0]) == 0 && mappings.read(pool.value, &buffers[4], data),
        "Removing a buffer makes exactly one bounded mapping slot available");
    require(mappings.clear() == 0, "Release the first bounded generation");
    data.mapoffset = 1;
    require(mappings.read(pool.value, &buffers[0], data) && mappings.read(pool.value, &buffers[1], data),
        "Two unaligned mappings use four pages of the four-page budget");
    require(!mappings.read(pool.value, &buffers[2], data) && mappings.count() == 2,
        "Aligned-byte budget rejects excess before the four-buffer count is reached");
    require(mappings.clear() == 0, "Release aligned-budget maps");
    data.maxsize = page + 1;
    require(!mappings.read(pool.value, &buffers[0], data), "Per-plane budget is independently enforced");
    data = descriptor(fd.get(), 2, 3 * page - 1);
    require(!mappings.read(pool.value, &buffers[0], data), "Reject a range beyond actual fstat length");
    data = descriptor(fd.get(), 2, (std::numeric_limits<std::uint32_t>::max)());
    require(!mappings.read(pool.value, &buffers[0], data), "A wide end calculation cannot wrap at UINT32_MAX");
    PipeWireMemFdMappings zeroBudget(0);
    require(!zeroBudget.read(pool.value, &buffers[0], descriptor(fd.get(), 1)), "Zero mapping budget fails safely");

    // Sparse length only: no pages are populated or mapped by this rejected
    // input. Exercise the fixed API's page-rounding overflow boundary.
    auto large = memoryFile(std::uint64_t{(std::numeric_limits<std::uint32_t>::max)()} + 1);
    NativePool largePool;
    largePool.importFd(large.get());
    PipeWireMemFdMappings largeLimit((std::numeric_limits<std::uint32_t>::max)());
    const auto overflow = largeLimit.read(largePool.value, &buffers[0],
        descriptor(large.get(), (std::numeric_limits<std::uint32_t>::max)(), 1));
    require(!overflow && overflow.error == -EOVERFLOW && largeLimit.count() == 0,
        "Official page-alignment overflow is rejected before creating a mapping");
}

void nativeRevocation(std::uint32_t page)
{
    auto fd = memoryFile(page);
    const auto data = descriptor(fd.get(), page);
    std::array<pw_buffer, 2> buffers{};
    {
        NativePool pool;
        auto* block = pool.importFd(fd.get());
        PipeWireMemFdMappings mappings(page);
        require(static_cast<bool>(mappings.read(pool.value, &buffers[0], data)), "Map before real remove_id");
        require(pw_mempool_remove_id(pool.value, block->id) == 0,
            "Revoke the actual native memory ID while a map still holds a reference");
        require(!mappings.read(pool.value, &buffers[0], data), "A removed native ID cannot supply another frame");
        require(mappings.remove(&buffers[0]) == 0 && mappings.count() == 0 && pool.removed == 1,
            "Last-map release handles synchronous pool.removed without recursive double free");
    }
    {
        NativePool pool;
        auto* block = pool.importFd(fd.get());
        PipeWireMemFdMappings mappings(page);
        require(mappings.read(pool.value, &buffers[0], data) && mappings.read(pool.value, &buffers[1], data),
            "Hold two actual maps before forced block revocation");
        pw_memblock_free(block);
        require(mappings.count() == 0 && mappings.mappedBytes() == 0 && pool.removed == 1 &&
            mappings.remove(&buffers[0]) == 0 && mappings.remove(&buffers[1]) == 0,
            "Native block removal clears all maps before the library forcibly frees them");
    }
    {
        NativePool pool;
        pool.importFd(fd.get());
        PipeWireMemFdMappings mappings(page);
        require(static_cast<bool>(mappings.read(pool.value, &buffers[0], data)), "Map before real pool destruction");
        pool.destroy();
        require(mappings.count() == 0 && mappings.mappedBytes() == 0 && mappings.clear() == 0,
            "Pool destroy detaches and clears mappings before helper destruction");
    }
    require(fcntl(fd.get(), F_GETFD) >= 0, "Mapping revocation never closes the producer-owned FD");
}
} // namespace

int main()
{
    try {
        Runtime runtime;
        require(std::strcmp(pw_get_library_version(), "1.6.11") == 0,
            "These checks must execute the fixed PipeWire 1.6.11 client");
        const auto nativePage = sysconf(_SC_PAGESIZE);
        require(nativePage > 0 && nativePage <= (std::numeric_limits<std::uint32_t>::max)(),
            "Read the actual Linux page size");
        const auto page = static_cast<std::uint32_t>(nativePage);
        realLegacyBufferAndReuse(page);
        offsetAndDescriptorChanges(page);
        typeAccessAndMembership(page);
        rangeAndBudget(page);
        nativeRevocation(page);
        std::cout << "PASS real MemFd bytes, read-only mapping, bounds, reuse, budgets and native pool revocation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
