#pragma once

// Reused from this project's PR #7 at 251bc0071278c6b711c840bf5188d16870bc7349.
// MIT, Copyright (c) 2026 NAMEWTA; source provenance: docs/scroll-capture.md.
#include <pthread.h>
#include <signal.h>
#include <cerrno>

namespace HuhuCapture::Platform::Linux {
class X11SignalMask {
public:
    X11SignalMask() noexcept
    {
        sigemptyset(&pipe_); sigaddset(&pipe_, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &pipe_, &previous_) != 0) return;
        valid_ = true;
        // A caller that already blocks SIGPIPE owns its pending-signal policy.
        // Nested capture operations likewise leave the outer guard in charge.
        changed_ = sigismember(&previous_, SIGPIPE) == 0;
        if (!changed_) return;
        sigset_t pending;
        if (sigpending(&pending) != 0) {
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
            changed_ = valid_ = false; return;
        }
        pendingBefore_ = sigismember(&pending, SIGPIPE) != 0;
    }
    ~X11SignalMask()
    {
        if (!changed_) return;
        const int previousError = errno;
        if (!pendingBefore_) {
            // libxcb may use writev without MSG_NOSIGNAL. Consume a SIGPIPE
            // generated during this operation before restoring this worker's
            // mask; never replace the process-wide signal disposition.
            const timespec immediate{};
            while (sigtimedwait(&pipe_, nullptr, &immediate) < 0 && errno == EINTR) {}
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
        errno = previousError;
    }
    bool valid() const noexcept { return valid_; }
    X11SignalMask(const X11SignalMask&) = delete;
    X11SignalMask& operator=(const X11SignalMask&) = delete;
private:
    sigset_t pipe_{}, previous_{};
    bool valid_{}, changed_{}, pendingBefore_{};
};
} // namespace HuhuCapture::Platform::Linux
