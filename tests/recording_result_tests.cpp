#include "core/MediaClock.h"
#include "core/RecordingResult.h"

#include <cstdlib>
#include <iostream>

using namespace HuhuCapture::Core;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    static_assert(MediaClock::is_steady);
    static_assert(mediaTicks(1s) == 10'000'000);
    static_assert(mediaTicks(1ms) == 10'000);
    static_assert(mediaTicks(100ns) == 1);
    static_assert(mediaTicks(99ns) == 0);
    static_assert(mediaTicks(120min) == 72'000'000'000);
    static_assert(mediaTicks(-1ms) == -10'000);
    const auto first = MediaClock::time_point{};
    check(mediaTicks((first + 3s) - first) == 30'000'000,
        "elapsed steady timestamps must use 100ns units");

    RecordingResult result;
    check(!result.canArchive(), "an unstarted recording is not an archive item");
    check(result.completion(false) == RecordingCompletion::Failed,
        "an empty result without user cancellation cannot report success");
    check(result.completion(true) == RecordingCompletion::Cancelled,
        "an immediate clean stop is cancelled");
    result.finish(true);
    check(!result.canArchive(), "successful finalize of an empty file is not media");

    result = {};
    result.framesWritten = 3;
    check(!result.canArchive(), "written frames still require finalization");
    result.finish(true);
    check(result.canArchive(), "complete frames and finalization are archivable");
    check(result.completion(false) == RecordingCompletion::Completed,
        "encoder completion does not depend on UI stop flag");

    result = {};
    result.framesWritten = 3;
    result.fail(RecordingStage::Capture, -101);
    result.finish(true);
    check(!result.canArchive(), "successful finalize cannot hide capture failure");
    check(result.completion(true) == RecordingCompletion::RecoverableFailure,
        "a user stop after failure cannot promote partial media to success");
    check(result.firstError == RecordingError{RecordingStage::Capture, -101},
        "preserve the first failure");
    result.finish(false, -202);
    check(result.firstError == RecordingError{RecordingStage::Capture, -101},
        "finalization failure must not replace the capture failure");
    check(result.finalizationError == RecordingError{RecordingStage::Finalize, -202},
        "retain the separate finalization failure");
    result.finish(true);
    check(!result.finalized && !result.canArchive(),
        "retry success cannot erase a failed finalization attempt");

    result = {};
    result.framesWritten = 1;
    result.finish(false, 12);
    check(result.completion(false) == RecordingCompletion::RecoverableFailure,
        "GIF writer failure after add_frame is recoverable, not successful");
    check(result.firstError == RecordingError{RecordingStage::Finalize, 12},
        "GIF finish status must remain visible");

    result = {};
    result.fail(RecordingStage::Prepare, -3);
    check(result.completion(true) == RecordingCompletion::Failed,
        "a stop request cannot hide initialization failure");
    result.fail(RecordingStage::Encode, -4);
    check(result.firstError == RecordingError{RecordingStage::Prepare, -3},
        "later failures must not replace preparation errors");
    std::cout << "recording completion and explicit monotonic media time passed\n";
}
