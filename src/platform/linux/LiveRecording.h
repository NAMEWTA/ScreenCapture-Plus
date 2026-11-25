#pragma once

#include "core/LiveSource.h"
#include "recording/RecordingBackend.h"

namespace HuhuCapture::Platform::Linux {

// Consume one already claimed, selected live source. Copies of this std::function
// share one admission; it cannot reauthorize or restart a native source. The
// GUI owner still owns Portal and calls consumerFinished after the nonzero
// RecordingSession token returns. A rejected session does not invoke this
// provider, so the owner must cancel/release that unused claim itself.
Recording::Backend makeLiveRecordingBackend(Core::LiveSourceConsumer source);

} // namespace HuhuCapture::Platform::Linux
