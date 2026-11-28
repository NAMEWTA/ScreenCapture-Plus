#pragma once

#include "recording/FrameSource.h"
#include <memory>
#include <utility>

namespace HuhuCapture::Recording {

enum class ControlsExclusion { Excluded, Unsupported, Failed };
// GUI-thread capability call for an actual native top-level window. Unsupported
// is explicit: a Wayland source has no portable per-window exclusion contract.
ControlsExclusion excludeControlsFromCapture(std::uintptr_t nativeWindow);
struct DesktopRecordingControls {
    std::uintptr_t nativeWindow{};
    // A verified GUI screen binding positioned the controls outside the region.
    // The native worker checks actual physical placement around each readback.
    bool expectedOutside{};
};

// The selected platform supplies one production implementation. Windows uses
// its retained MF/WASAPI MP4 pipeline; GIF uses the shared aggregate-library
// encoder. Native X11 capture must not be selected for an XWayland session.
Backend desktopRecordingBackend(DesktopRecordingControls controls = {});
#ifndef _WIN32
// Consumes the selected authorized source once, on the recording worker. The
// caller keeps its Portal owner alive until that source reports it has stopped.
Backend frameRecordingBackend(std::unique_ptr<Core::FrameSource> source,
    FrameSourceContract expected);
#endif

} // namespace HuhuCapture::Recording
