#pragma once

#include "recording/RecordingBackend.h"

namespace HuhuCapture::Platform::Windows {

// Retained full-desktop GDI source, shared fixed gifski encoder and the same
// already-owned writer contract as runNativeMp4. Called on a recording worker.
Recording::BackendResult runNativeGif(const Recording::BackendRequest& request,
    FileHandle writer, const Recording::BackendEvents& events, std::stop_token stop);

} // namespace HuhuCapture::Platform::Windows
