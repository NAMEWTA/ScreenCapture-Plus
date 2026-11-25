#pragma once

#include "recording/RecordingBackend.h"

namespace HuhuCapture::Platform::Windows {

// Synchronous worker entry point shared by the old Windows host and the Qt
// RecordingSession. Owns COM/MF and all native capture/audio objects. Neither
// this entry point nor its callbacks may touch a window or wait for the GUI.
Recording::BackendResult runNativeMp4(const Recording::BackendRequest& request,
    FileHandle writer, const Recording::BackendEvents& events = {},
    std::stop_token stop = {});

} // namespace HuhuCapture::Platform::Windows
