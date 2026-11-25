#pragma once

#include "core/MediaFrame.h"

#include <stop_token>
#include <string>

namespace HuhuCapture::Core {

enum class FrameSourceKind { DesktopDuplication, Gdi, PipeWire, X11 };
enum class FrameReadStatus { Frame, Idle, Stopped, Failed };
struct FrameRead {
    FrameReadStatus status{FrameReadStatus::Failed};
    FrameLease frame;
    std::int64_t error{};
    std::string detail;
};

// One consumer calls read(). Idle means the actual source is still alive, so
// a recorder may reuse its last pixels for an unchanged desktop. Unexpected
// source/Portal closure returns Failed, never endless Idle. Active consumers
// stop native producers on a worker before codec finalization. A reader that
// was never consumed may be released without waiting on its GUI owner.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual FrameSourceKind kind() const noexcept = 0;
    virtual FrameRead read(MediaClock::time_point deadline, std::stop_token stop) = 0;
    // Worker-only barrier. A threaded source waits for native teardown, never
    // for a Qt delivery callback. Synchronous polling sources have no producer.
    virtual void stopAndWait() noexcept {}
};

} // namespace HuhuCapture::Core
