#include "platform/NativeScrollSource.h"
#include "platform/linux/X11Capture.h"

#include <xcb/xcb.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace platform = HuhuCapture::Platform;
namespace linuxPlatform = HuhuCapture::Platform::Linux;
using namespace std::chrono_literals;
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }

bool pipeBlocked()
{
    sigset_t current;
    require(pthread_sigmask(SIG_SETMASK, nullptr, &current) == 0, "Read the current worker's signal mask");
    return sigismember(&current, SIGPIPE) != 0;
}

std::set<int> descriptors()
{
    struct CloseDirectory { void operator()(DIR* value) const noexcept { closedir(value); } };
    std::unique_ptr<DIR, CloseDirectory> directory(opendir("/proc/self/fd"));
    require(bool(directory), "Read the probe's actual descriptor table");
    std::set<int> result;
    while (const auto* entry = readdir(directory.get())) {
        char* end{};
        const auto value = std::strtol(entry->d_name, &end, 10);
        if (*entry->d_name && !*end && value >= 0 && value != dirfd(directory.get())) result.insert(static_cast<int>(value));
    }
    return result;
}

std::set<int> ownedSegments()
{
    std::ifstream stream("/proc/sysvipc/shm");
    require(bool(stream), "Read actual System V SHM ownership");
    std::string line;
    require(bool(std::getline(stream, line)), "Read the System V SHM header");
    std::istringstream header(line);
    std::vector<std::string> columns;
    for (std::string column; header >> column;) columns.push_back(std::move(column));
    const auto creator = std::find(columns.begin(), columns.end(), "cpid");
    const auto identifier = std::find(columns.begin(), columns.end(), "shmid");
    require(creator != columns.end() && identifier != columns.end(), "SHM data has explicit creator and segment identifiers");
    const auto creatorIndex = static_cast<std::size_t>(creator - columns.begin());
    const auto idIndex = static_cast<std::size_t>(identifier - columns.begin());
    std::set<int> result;
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::vector<std::string> values;
        for (std::string value; row >> value;) values.push_back(std::move(value));
        require(values.size() == columns.size(), "Read a complete kernel SHM row");
        if (std::stoll(values[creatorIndex]) == getpid()) result.insert(std::stoi(values[idIndex]));
    }
    return result;
}

void command(const std::string& expected)
{
    std::string line;
    require(bool(std::getline(std::cin, line)) && line == expected, "The private server owner did not acknowledge " + expected);
}

bool blockedInNativePoll(int threadId)
{
    // This worker has one XCB reader and performs no other poll/select after
    // entering capture. Observe the actual blocked syscall; a scheduling delay
    // or a stop requested before capture starts is not cancellation evidence.
    std::ifstream stream("/proc/self/task/" + std::to_string(threadId) + "/syscall");
    require(bool(stream), "Inspect the owned worker's actual blocking syscall");
    std::string operation;
    stream >> operation;
    if (operation.empty() || operation == "running") return false;
    const auto number = std::stol(operation);
    return number == SYS_poll || number == SYS_ppoll || number == SYS_select || number == SYS_pselect6;
}

class ControlConnection {
public:
    ControlConnection() : connection_(xcb_connect(nullptr, nullptr), &xcb_disconnect)
    {
        require(connection_ && !xcb_connection_has_error(connection_.get()), "Connect the separate real X11 control client");
        const auto roots = xcb_setup_roots_iterator(xcb_get_setup(connection_.get()));
        require(roots.rem != 0, "The owned X server has a root window");
        root = roots.data->root;
    }
    void roundTrip()
    {
        const auto cookie = xcb_get_input_focus(connection_.get());
        std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply(
            xcb_get_input_focus_reply(connection_.get(), cookie, nullptr), &std::free);
        require(bool(reply) && !xcb_connection_has_error(connection_.get()), "Cancellation must not close another XCB client's socket");
    }
    xcb_window_t root{};
private:
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection_;
};

void waitForServerCleanup(ControlConnection& control, const std::set<int>& baseline)
{
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (ownedSegments() != baseline) {
        require(std::chrono::steady_clock::now() < until, "The resumed owned server releases the removed capture SHM segment");
        control.roundTrip(); std::this_thread::sleep_for(2ms);
    }
}

void pausedCase(const std::string& name, ControlConnection& control)
{
    const auto initialDescriptors = descriptors();
    const auto initialSegments = ownedSegments();
    std::stop_source stop;
    std::promise<void> initialized, begin;
    auto ready = initialized.get_future();
    auto permission = begin.get_future();
    std::atomic<int> entered{};
    auto worker = std::async(std::launch::async, [&] {
        try {
            const bool initialPipeBlocked = pipeBlocked();
            if (name == "native-capture" || name == "native-scroll") {
                platform::NativeScrollSource source(0, 0, 128, 96, stop.get_token());
                require(source.capture() && !source.pixels().empty(), "Prepare actual native scroll capture pixels and SHM");
                initialized.set_value(); permission.wait();
                entered.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
                const bool succeeded = name == "native-capture" ? source.capture() :
                    source.scroll() == platform::ScrollInputResult::Scrolled;
                require(!succeeded && !source.error().empty(), "Interrupted native scrolling returns failure, never stale success");
                require(!source.capture(), "A stopped native scroll source cannot reconnect while the server is paused");
            } else {
                linuxPlatform::X11Capture source(stop.get_token());
                require(source.capture(0, 0, 128, 96, true), source.error());
                initialized.set_value(); permission.wait();
                entered.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
                linuxPlatform::X11Desktop desktop;
                linuxPlatform::X11Rectangle rectangle;
                const bool succeeded = name == "capture" ? source.capture(0, 0, 128, 96, true) :
                    name == "desktop" ? source.desktop(desktop) : source.windowRectangle(control.root, rectangle);
                require(!succeeded && !source.error().empty(), "Interrupted XCB query returns failure, never stale success");
                source.reset();
                require(!source.initialize(), "Reset does not clear cancellation or reconnect a stopped X11 source");
            }
            require(pipeBlocked() == initialPipeBlocked, "XCB operations restore this worker's original SIGPIPE mask");
            // The owned adapter and callback have actually been destroyed
            // before this future can become ready.
        } catch (...) {
            try { initialized.set_exception(std::current_exception()); } catch (const std::future_error&) {}
            throw;
        }
    });
    struct CancelOnExit {
        std::stop_source& stop;
        std::promise<void>& begin;
        ~CancelOnExit()
        {
            stop.request_stop();
            try { begin.set_value(); } catch (const std::future_error&) {}
        }
    } cancelOnExit{stop, begin};
    require(ready.wait_for(10s) == std::future_status::ready, "Prepare the established native capture connection");
    ready.get();
    require(ownedSegments().size() > initialSegments.size(), "This regression uses actual MIT-SHM, not a fabricated frame");
    std::cout << "READY " << name << '\n' << std::flush;
    command("PAUSED " + name); // Python has observed its child's SIGSTOP state.
    begin.set_value();
    const auto enteredUntil = std::chrono::steady_clock::now() + 3s;
    while (!entered.load(std::memory_order_acquire) || !blockedInNativePoll(entered.load(std::memory_order_acquire))) {
        require(worker.wait_for(0ms) != std::future_status::ready, "The paused-server operation must genuinely block before cancellation");
        require(std::chrono::steady_clock::now() < enteredUntil, "Observe the actual XCB poll blocked against the paused server");
        std::this_thread::sleep_for(1ms);
    }
    const auto requested = std::chrono::steady_clock::now();
    require(stop.request_stop(), "Request native cancellation once");
    const auto callbackDuration = std::chrono::steady_clock::now() - requested;
    require(callbackDuration < 1s, "The socket interruption callback cannot join the native worker or wait for XCB locks");
    require(worker.wait_for(2s) == std::future_status::ready, "Established XCB I/O and adapter destruction drain while the server is still paused");
    worker.get();
    const auto drainDuration = std::chrono::steady_clock::now() - requested;
    require(descriptors() == initialDescriptors, "Actual worker completion releases the owned connection descriptor");
    std::cout << "DRAINED " << name << ' '
        << std::chrono::duration_cast<std::chrono::microseconds>(callbackDuration).count() << ' '
        << std::chrono::duration_cast<std::chrono::microseconds>(drainDuration).count() << '\n' << std::flush;
    command("RESUMED " + name);
    control.roundTrip();
    // SIGSTOP also freezes the server's own SHM mapping. The worker detached
    // while paused; the server can detach only after its owner resumes it.
    waitForServerCleanup(control, initialSegments);
}

void callbackLifetime(ControlConnection& control)
{
    const auto initialDescriptors = descriptors();
    const auto initialSegments = ownedSegments();
    std::stop_source lateStop;
    int previousSocket{-1};
    {
        linuxPlatform::X11Capture source(lateStop.get_token());
        require(source.capture(0, 0, 128, 96), source.error());
        const auto current = descriptors();
        std::vector<int> added;
        std::set_difference(current.begin(), current.end(), initialDescriptors.begin(), initialDescriptors.end(), std::back_inserter(added));
        require(added.size() == 1, "A capture adapter owns exactly one native connection FD");
        previousSocket = added.front();
    }
    require(descriptors() == initialDescriptors, "Normal destruction releases its real connection FD");
    int sockets[2]{-1, -1};
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0, "Allocate unrelated real sockets after capture destruction");
    struct Close { int* descriptors; ~Close() { close(descriptors[0]); close(descriptors[1]); } } closeSockets{sockets};
    require(sockets[0] == previousSocket || sockets[1] == previousSocket, "The unrelated socket actually reuses the old capture FD number");
    require(lateStop.request_stop(), "Request the old token after native destruction");
    constexpr char sent = 'x'; char received{};
    require(send(sockets[0], &sent, 1, MSG_NOSIGNAL) == 1 && recv(sockets[1], &received, 1, MSG_DONTWAIT) == 1 && received == sent,
        "A deregistered callback cannot shutdown a recycled descriptor");
    control.roundTrip(); waitForServerCleanup(control, initialSegments);
}

void cancellationRace(ControlConnection& control)
{
    const auto initialDescriptors = descriptors();
    const auto initialSegments = ownedSegments();
    const bool initialMask = pipeBlocked();
    struct sigaction previousDisposition{};
    require(sigaction(SIGPIPE, nullptr, &previousDisposition) == 0, "Inspect the real process SIGPIPE disposition");
    for (int repeat = 0; repeat < 32; ++repeat) {
        std::stop_source stop;
        linuxPlatform::X11Capture source(stop.get_token());
        require(source.capture(0, 0, 128, 96), source.error());
        std::atomic<bool> entering{};
        std::jthread cancelling([&] {
            entering.wait(false);
            std::this_thread::sleep_for(std::chrono::microseconds(repeat % 16));
            stop.request_stop();
        });
        entering.store(true); entering.notify_one();
        // A real server is running here. Race stop against query submission,
        // not only a read already observed in poll, to exercise write/SIGPIPE
        // and callback-destruction ordering without changing global signals.
        static_cast<void>(source.capture(0, 0, 128, 96, true));
        cancelling.join(); source.reset();
        require(!source.initialize(), "Concurrent stop keeps the same instance terminal");
        require(pipeBlocked() == initialMask, "Concurrent native I/O restores the caller's signal mask");
    }
    struct sigaction finalDisposition{};
    require(sigaction(SIGPIPE, nullptr, &finalDisposition) == 0 &&
        finalDisposition.sa_handler == previousDisposition.sa_handler &&
        finalDisposition.sa_flags == previousDisposition.sa_flags,
        "Capture cancellation never changes the process SIGPIPE disposition");
    require(descriptors() == initialDescriptors, "Concurrent cancellation releases every actual capture FD");
    control.roundTrip(); waitForServerCleanup(control, initialSegments);
}

void existingPendingSignal()
{
    sigset_t onlyPipe, previous, pending;
    sigemptyset(&onlyPipe); sigaddset(&onlyPipe, SIGPIPE);
    require(pthread_sigmask(SIG_BLOCK, &onlyPipe, &previous) == 0, "Create a private per-thread blocked signal contract");
    struct Restore {
        sigset_t previous, onlyPipe;
        ~Restore()
        {
            const timespec immediate{};
            static_cast<void>(sigtimedwait(&onlyPipe, nullptr, &immediate));
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
    } restore{previous, onlyPipe};
    require(pthread_kill(pthread_self(), SIGPIPE) == 0, "Queue an actual caller-owned blocked SIGPIPE");
    std::stop_source stop; stop.request_stop();
    {
        linuxPlatform::X11Capture source(stop.get_token());
        require(!source.initialize(), "A pre-cancelled source performs no handshake");
    }
    require(pipeBlocked() && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1,
        "The native guard preserves a caller's blocked, already-pending signal");
}

void preCancelled()
{
    const auto before = descriptors();
    const bool initialMask = pipeBlocked();
    std::stop_source stop; stop.request_stop();
    {
        linuxPlatform::X11Capture capture(stop.get_token());
        linuxPlatform::X11Desktop desktop;
        linuxPlatform::X11Rectangle rectangle;
        require(!capture.initialize() && !capture.desktop(desktop) && !capture.capture(0, 0, 128, 96) &&
            !capture.windowRectangle(1, rectangle) && !capture.scroll(10, 10, 1),
            "Every pre-cancelled native operation rejects work before any initial connection");
        capture.reset();
        require(!capture.initialize() && capture.pixels().empty(), "Reset cannot revive a cancelled source or publish pixels");
        platform::NativeScrollSource scroll(0, 0, 128, 96, stop.get_token());
        require(!scroll.capture() && scroll.scroll() == platform::ScrollInputResult::Failed && scroll.pixels().empty(),
            "The real scrolling adapter forwards pre-cancellation without creating a source");
    }
    existingPendingSignal();
    require(descriptors() == before && pipeBlocked() == initialMask, "Pre-cancellation preserves real descriptors and the caller's signal mask");
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--pre-cancel") {
        try {
            preCancelled();
            std::cout << "PASS real pre-cancelled adapters, no connection admission, descriptor and pending-signal ownership\n";
            return 0;
        } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    }
    const auto* isolated = std::getenv("HUHUCAPTURE_TEST_X11_ISOLATED");
    const auto* paused = std::getenv("HUHUCAPTURE_TEST_X11_PAUSED_SERVER");
    if (argc != 1 || !isolated || std::string(isolated) != "1" || !paused || std::string(paused) != "1") {
        std::cerr << "Use scripts/test-x11-capture.py --paused-server with its owned private display\n"; return 1;
    }
    try {
        ControlConnection control;
        for (const auto* name : {"capture", "desktop", "window", "native-capture", "native-scroll"}) pausedCase(name, control);
        callbackLifetime(control); cancellationRace(control); existingPendingSignal();
        std::cout << "PASS established-XCB cancellation, real worker drain, independent connection and recycled-FD safety\n" << std::flush;
        std::cerr << "NOT COVERED: initial xcb_connect authentication/setup before the owned socket is exposed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
