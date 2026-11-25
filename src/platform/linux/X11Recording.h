#pragma once

#include "recording/RecordingBackend.h"

namespace HuhuCapture::Platform::Linux {

// Native X11 only. Construction opens no display or media file. Each admitted
// task owns an X11Capture on its producer worker and the same bounded live
// queue/normalizer/codecs as an authorized source recording. The caller checks
// its desktop selection generation before admission and stops on invalidation.
// This provider additionally rejects XWayland and native layout changes.
Recording::Backend makeX11RecordingBackend();

} // namespace HuhuCapture::Platform::Linux
