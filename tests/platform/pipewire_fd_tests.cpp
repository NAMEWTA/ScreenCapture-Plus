#include "platform/linux/UniqueFd.h"
#include "platform/linux/pipewire/PipeWireCapture.h"

#include <pipewire/pipewire.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>

using namespace HuhuCapture::Platform::Linux;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Runtime {
    Runtime() { pw_init(nullptr, nullptr); }
    ~Runtime() { pw_deinit(); }
};

struct NativeContext {
    pw_main_loop* mainLoop{pw_main_loop_new(nullptr)};
    pw_context* context{};
    NativeContext()
    {
        require(mainLoop, "cannot create a real PipeWire main loop");
        context = pw_context_new(pw_main_loop_get_loop(mainLoop),
            pw_properties_new("module.rt", "false", nullptr), 0);
        if (!context) {
            pw_main_loop_destroy(mainLoop);
            mainLoop = nullptr;
            throw std::runtime_error("cannot load the built PipeWire client modules");
        }
    }
    ~NativeContext()
    {
        if (context) pw_context_destroy(context);
        if (mainLoop) pw_main_loop_destroy(mainLoop);
    }
};

struct SocketPair {
    UniqueFd client;
    UniqueFd peer;
    SocketPair()
    {
        int sockets[2];
        require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) == 0,
            "cannot create real AF_UNIX socket pair");
        client.reset(sockets[0]);
        peer.reset(sockets[1]);
    }
};

void checkClosed(int fd, int peer)
{
    errno = 0;
    require(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "owned remote FD was not closed on connection error");
    char pending[4096];
    ssize_t count;
    do { count = recv(peer, pending, sizeof(pending), MSG_DONTWAIT); } while (count > 0);
    require(count == 0,
        "the failed connection retained a duplicate of the socket");
}

// Deliberately break only this test's private loop epoll FD. Restore its exact
// open file description before context destruction. This exercises a genuine
// Linux EINVAL from epoll_ctl, without replacing PipeWire methods or fake peers.
class UnpollableLoop {
public:
    explicit UnpollableLoop(pw_loop* loop) : original_(pw_loop_get_fd(loop)),
        backup_(fcntl(original_, F_DUPFD_CLOEXEC, 0)), replacement_(open("/dev/null", O_RDONLY | O_CLOEXEC))
    {
        require(original_ >= 0 && backup_ && replacement_, "cannot preserve this test's private loop FD");
        require(dup3(replacement_.get(), original_, O_CLOEXEC) == original_, "cannot install the real epoll fault");
    }
    ~UnpollableLoop()
    {
        if (dup3(backup_.get(), original_, O_CLOEXEC) != original_) std::terminate();
    }
private:
    int original_;
    UniqueFd backup_;
    UniqueFd replacement_;
};

void beforeProtocolCreation()
{
    NativeContext native;
    SocketPair socket;
    const int fd = socket.client.release();
    const auto* core = pw_context_connect_fd(native.context, fd,
        pw_properties_new(PW_KEY_PROTOCOL, "huhucapture-test-no-such-protocol", nullptr), 0);
    const int savedError = errno;
    require(!core && savedError == ENOTSUP, "missing protocol must preserve its native initialization error");
    checkClosed(fd, socket.peer.get());
}

void beforeLoopSourceOwnership()
{
    NativeContext native;
    SocketPair socket;
    const int fd = socket.client.release();
    {
        UnpollableLoop fault(pw_main_loop_get_loop(native.mainLoop));
        const auto* core = pw_context_connect_fd(native.context, fd, nullptr, 0);
        const int savedError = errno;
        require(!core && savedError == EINVAL, "real epoll source failure must preserve EINVAL");
        checkClosed(fd, socket.peer.get());
    }
}

void failedWorkerSources()
{
    NativeContext native;
    auto* loop = pw_main_loop_get_loop(native.mainLoop);
    {
        UnpollableLoop fault(loop);
        const auto neverEvent = [](void*, std::uint64_t) { std::terminate(); };
        const auto neverIdle = [](void*) { std::terminate(); };
        const auto* event = pw_loop_add_event(loop, neverEvent, nullptr);
        require(!event && errno == EINVAL, "event registration must report the real epoll error");
        const auto* timer = pw_loop_add_timer(loop, neverEvent, nullptr);
        require(!timer && errno == EINVAL, "timer registration must report the real epoll error");
        const auto* idle = pw_loop_add_idle(loop, false, neverIdle, nullptr);
        require(!idle && errno == EINVAL, "idle registration must report the real epoll error");
    }
    // NativeContext teardown must not follow any freed source-list entries.
}

void immediateCancellationAndBadArguments()
{
    for (bool cancel : {false, true}) {
        SocketPair socket;
        const int fd = socket.client.get();
        PipeWireCaptureOptions options;
        options.sessionId = 1;
        options.sourceId = 1;
        PipeWireCaptureCallbacks callbacks;
        callbacks.frame = [](auto) { throw std::runtime_error("no frames can exist before connect"); return false; };
        bool inactive{};
        callbacks.activityChanged = [&](bool streaming) {
            require(!streaming, "pre-connect failure/cancellation must never announce actual Streaming");
            inactive = true;
        };
        std::stop_source stop;
        if (cancel) stop.request_stop();
        const auto result = runPipeWireCapture(std::move(socket.client), {cancel ? 42u : 0u, {}},
            options, std::move(callbacks), stop.get_token());
        require(cancel ? result.stopped && !result.failure :
            result.failure && result.failure->code == PipeWireFailure::Code::InvalidArgument,
            "pre-connect cancellation and bad arguments must remain distinguishable");
        require(inactive, "terminal cancellation or rejected input reports inactivity");
        checkClosed(fd, socket.peer.get());
    }
}

void silentTransportDeadlineAndCancellation()
{
    for (bool cancel : {false, true}) {
        SocketPair socket;
        const int fd = socket.client.get();
        PipeWireCaptureOptions options;
        options.sessionId = 1;
        options.sourceId = 1;
        options.firstFrameTimeout = cancel ? std::chrono::seconds(5) : std::chrono::milliseconds(200);
        PipeWireCaptureCallbacks callbacks;
        callbacks.frame = [](auto) { throw std::runtime_error("a silent transport cannot supply media"); return false; };
        bool inactive{};
        callbacks.activityChanged = [&](bool streaming) {
            require(!streaming, "a silent socket cannot authorize a Streaming state");
            inactive = true;
        };
        std::stop_source stop;
        std::jthread cancellation;
        if (cancel) cancellation = std::jthread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
        // An actual socket that never answers is a failed transport, not a fake
        // successful daemon or a replacement for the real Portal frame probe.
        const auto result = runPipeWireCapture(std::move(socket.client), {42u, {}},
            options, std::move(callbacks), stop.get_token());
        require(cancel ? result.stopped && !result.failure :
            result.failure && result.failure->code == PipeWireFailure::Code::FirstFrameTimeout,
            "silent transport must stop via its actual loop timer or cross-thread event");
        require(result.stats.framesDelivered == 0, "failure tests must not invent successful media");
        require(inactive, "actual transport timeout/cancellation leaves no Streaming health");
        checkClosed(fd, socket.peer.get());
    }
}

} // namespace

int main()
{
    try {
        Runtime runtime;
        require(std::strcmp(pw_get_library_version(), "1.6.11") == 0,
            "this validation must execute the fixed PipeWire 1.6.11 client");
        beforeProtocolCreation();
        beforeLoopSourceOwnership();
        failedWorkerSources();
        immediateCancellationAndBadArguments();
        silentTransportDeadlineAndCancellation();
        std::cout << "real PipeWire/socket/epoll failures, deadline and cancellation preserve ownership/errors\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
