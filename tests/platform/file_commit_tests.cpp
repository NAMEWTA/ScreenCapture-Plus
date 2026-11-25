#include "platform/FileCommitter.h"

#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace platform = HuhuCapture::Platform;

#if defined(HUHUCAPTURE_TEST_WRAP_FSYNC)
namespace {
// Faults are injected at the OS seam by the test linker, never by a production
// setting or alternate file committer. Normal cases still exercise real I/O.
std::atomic<int> syncFault{};
std::atomic<int> directorySyncs{};
std::atomic<bool> swapFailed{};
fs::path swapSource;
fs::path swapSaved;
}
extern "C" int __real_fsync(int descriptor);
extern "C" int __wrap_fsync(int descriptor)
{
    struct stat info{};
    if (::fstat(descriptor, &info) == 0) {
        const auto fault = syncFault.load();
        const bool fail = (fault == 1 && S_ISREG(info.st_mode)) ||
            (fault == 2 && S_ISDIR(info.st_mode)) ||
            (fault == 4 && S_ISDIR(info.st_mode) && ++directorySyncs == 2);
        if (fail) { errno = EIO; return -1; }
        if (fault == 3 && S_ISREG(info.st_mode) && syncFault.exchange(0) == 3) {
            if (::rename(swapSource.c_str(), swapSaved.c_str()) != 0) swapFailed = true;
            const auto replacement = ::open(swapSource.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (replacement < 0) swapFailed = true;
            else {
                constexpr char payload[] = "replacement";
                if (::write(replacement, payload, sizeof(payload) - 1) != sizeof(payload) - 1) swapFailed = true;
                if (::close(replacement) != 0) swapFailed = true;
            }
        }
    }
    return __real_fsync(descriptor);
}
#endif

namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(const fs::path& parent = fs::temp_directory_path())
    {
        std::random_device random;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto candidate = parent / ("huhucapture-file-tests-" + std::to_string(random()) +
                "-" + std::to_string(random()));
            std::error_code error;
            if (fs::create_directory(candidate, error)) { path = std::move(candidate); return; }
            if (error && error != std::errc::file_exists) throw std::system_error(error);
        }
        throw std::runtime_error("could not create isolated test directory");
    }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    fs::path path;
};

void write(const fs::path& path, const std::string& value)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    require(!stream.fail(), "write fixture");
}

std::string read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "read fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

fs::path stage(const fs::path& directory, const std::string& bytes)
{
    std::error_code error;
    auto path = platform::createExclusiveTemporaryFile(directory, error);
    require(!error && !path.empty() && fs::is_regular_file(path), "exclusive staging creation");
    require(fs::file_size(path) == 0, "new staging file starts empty");
    write(path, bytes);
    return path;
}

void normalCommit(const fs::path& root)
{
    auto directory = root / fs::path(u8"中文目录");
    fs::create_directory(directory);
    const std::string bytes("complete\0media", 14);
    auto source = stage(directory, bytes);
    auto destination = directory / "20261125-121314.png";
    const auto result = platform::commitFileNoReplace(source, destination);
    require(result.durable() && !result.error, "normal commit is durable");
    require(result.destination == destination, "actual committed path returned");
    require(!result.stagingRetained && !fs::exists(source), "successful source cleanup");
    require(read(destination) == bytes, "binary contents preserved");
}

void collision(const fs::path& root)
{
    const auto source = stage(root, "new capture");
    const auto destination = root / "existing.png";
    write(destination, "user's existing image");
    const auto result = platform::commitFileNoReplace(source, destination);
    require(!result.committed() && result.destination.empty(), "collision is not a commit");
    require(result.error == std::errc::file_exists, "collision is distinguishable");
    require(read(destination) == "user's existing image", "existing destination never overwritten");
    require(read(source) == "new capture", "failed commit retains source");
    const auto same = platform::commitFileNoReplace(source, source);
    require(!same.committed() && read(source) == "new capture", "same path is not a successful move");
}

void invalidInputs(const fs::path& root)
{
    std::error_code error;
    require(platform::createExclusiveTemporaryFile(root / "missing", error).empty() && error,
        "missing parent is not silently created");
    const auto source = stage(root, "retained");
    require(!platform::commitFileNoReplace(source, root / "missing" / "target.png").committed(),
        "missing target directory fails");
    require(read(source) == "retained", "missing target retains source");
    require(!platform::commitFileNoReplace(root, root / "directory.png").committed(),
        "directory cannot be committed as media");
    require(!platform::commitFileNoReplace(root / "absent", root / "absent.png").committed(),
        "missing source fails");
    require(!platform::commitFileNoReplace(source, {}).committed(), "empty target fails");
}

void concurrentCommits(const fs::path& root)
{
    constexpr int count = 12;
    std::vector<fs::path> sources;
    for (int index = 0; index < count; ++index) sources.push_back(stage(root, std::to_string(index)));
    const auto destination = root / "contended.png";
    std::vector<platform::CommitResult> results(count);
    std::barrier start(count);
    std::vector<std::thread> workers;
    for (int index = 0; index < count; ++index) {
        workers.emplace_back([&, index] {
            start.arrive_and_wait();
            results[index] = platform::commitFileNoReplace(sources[index], destination);
        });
    }
    for (auto& worker : workers) worker.join();
    int committed = 0;
    for (int index = 0; index < count; ++index) {
        if (results[index].committed()) {
            ++committed;
            require(results[index].durable(), "concurrent winner durable");
            require(read(destination) == std::to_string(index), "winner's exact bytes published");
        } else {
            require(results[index].error == std::errc::file_exists, "loser reports collision");
            require(read(sources[index]) == std::to_string(index), "loser's source retained");
        }
    }
    require(committed == 1, "exactly one concurrent commit wins");
}

#if defined(__linux__)
void linksAndPermissions(const fs::path& root)
{
    const auto source = stage(root, "protected");
    struct stat info{};
    require(::stat(source.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
        "staging permissions are private");
    const auto sourceLink = root / "source-link";
    fs::create_symlink(source, sourceLink);
    require(!platform::commitFileNoReplace(sourceLink, root / "linked.png").committed(),
        "source symlink rejected");
    require(read(source) == "protected", "source symlink target retained");
    const auto targetLink = root / "dangling-link.png";
    fs::create_symlink(root / "absent-target", targetLink);
    const auto collisionResult = platform::commitFileNoReplace(source, targetLink);
    require(!collisionResult.committed() && collisionResult.error == std::errc::file_exists,
        "dangling destination symlink is never replaced");
    require(fs::is_symlink(targetLink), "destination symlink remains");
    const auto parentLink = root / "parent-link";
    fs::create_directory_symlink(root, parentLink);
    std::error_code error;
    require(platform::createExclusiveTemporaryFile(parentLink, error).empty() && error,
        "symlink staging parent rejected");
    require(!platform::commitFileNoReplace(source, parentLink / "unexpected.png").committed(),
        "symlink target parent rejected");
    const auto fifo = root / "fifo";
    require(::mkfifo(fifo.c_str(), 0600) == 0, "create fifo fixture");
    require(!platform::commitFileNoReplace(fifo, root / "fifo.png").committed(),
        "non-regular source rejected without blocking");
}

void crossFilesystem(const fs::path& root)
{
    struct stat sourceInfo{}, targetInfo{};
    if (::stat(root.c_str(), &sourceInfo) != 0 || ::stat("/dev/shm", &targetInfo) != 0 ||
        sourceInfo.st_dev == targetInfo.st_dev || ::access("/dev/shm", W_OK) != 0) {
        std::cout << "NOT RUN: distinct writable /dev/shm filesystem unavailable\n";
        return;
    }
    TemporaryDirectory target("/dev/shm");
    const auto source = stage(root, "source preserved across EXDEV");
    const auto destination = target.path / "cross-device.png";
    const auto result = platform::commitFileNoReplace(source, destination);
    require(!result.committed() && result.error == std::errc::cross_device_link,
        "cross-filesystem publication is explicitly rejected");
    require(!fs::exists(destination) && read(source) == "source preserved across EXDEV",
        "cross-filesystem failure never loses original");
}
#endif

#if defined(HUHUCAPTURE_TEST_WRAP_FSYNC)
void injectedFailures(const fs::path& root)
{
    for (const int fault : {1, 2, 4}) {
        const auto source = stage(root, "recoverable media");
        const auto destination = root / ("sync-failure-" + std::to_string(fault) + ".png");
        directorySyncs = 0;
        syncFault = fault;
        const auto result = platform::commitFileNoReplace(source, destination);
        syncFault = 0;
        require(result.error == std::errc::io_error, "sync error preserved");
        if (fault == 1) {
            require(!result.committed() && !fs::exists(destination), "file sync failure precedes publication");
            require(read(source) == "recoverable media", "file sync failure retains staging");
        } else {
            require(result.state == platform::CommitState::CommittedDurabilityUnknown,
                "directory sync failure distinguishes committed from durable");
            require(result.destination == destination && read(destination) == "recoverable media",
                "directory sync failure retains actual archive path and bytes");
            require(result.stagingRetained == (fault == 2), "cleanup state remains accurate");
        }
    }
    swapSource = stage(root, "validated inode");
    swapSaved = root / "original-inode";
    const auto destination = root / "source-race.png";
    swapFailed = false;
    syncFault = 3;
    const auto result = platform::commitFileNoReplace(swapSource, destination);
    syncFault = 0;
    require(!swapFailed, "source replacement fixture ran");
    require(result.durable() && read(destination) == "validated inode",
        "source name replacement cannot substitute the published inode");
    require(result.stagingRetained && result.cleanupError,
        "replaced source reports retained cleanup state");
    require(read(swapSource) == "replacement", "cleanup never deletes observed replacement");
}
#endif
} // namespace

int main()
{
    try {
        TemporaryDirectory directory;
        normalCommit(directory.path);
        collision(directory.path);
        invalidInputs(directory.path);
        concurrentCommits(directory.path);
#if defined(__linux__)
        linksAndPermissions(directory.path);
        crossFilesystem(directory.path);
#endif
#if defined(HUHUCAPTURE_TEST_WRAP_FSYNC)
        injectedFailures(directory.path);
#else
        std::cout << "NOT RUN: Linux fsync fault injection requires --wrap=fsync\n";
#endif
        std::cout << "File commit real-I/O checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "File commit check failed: " << error.what() << '\n';
        return 1;
    }
}
