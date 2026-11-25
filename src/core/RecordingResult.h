#pragma once

#include <cstdint>
#include <optional>

namespace HuhuCapture::Core {

enum class RecordingStage { Prepare, Capture, Encode, Finalize };
enum class RecordingCompletion { Completed, Cancelled, Failed, RecoverableFailure };

struct RecordingError {
    RecordingStage stage;
    std::int64_t code;
    bool operator==(const RecordingError&) const = default;
};

// One writer (the recording worker), read by the owner only after joining it.
// Successful finalization must never erase an earlier capture/encoding error.
// An error with some frames is recoverable data, not a successful archive item.
struct RecordingResult {
    std::uint64_t framesWritten{};
    std::optional<RecordingError> firstError;
    std::optional<RecordingError> finalizationError;
    bool finalized{};

    void fail(RecordingStage stage, std::int64_t code) noexcept
    {
        if (!firstError) firstError = RecordingError{stage, code};
        if (stage == RecordingStage::Finalize && !finalizationError)
            finalizationError = RecordingError{stage, code};
    }

    void finish(bool success, std::int64_t code = 0) noexcept
    {
        // A later success cannot undo a failed first finalization attempt.
        if (!success) fail(RecordingStage::Finalize, code);
        finalized = success && !finalizationError;
    }

    bool canArchive() const noexcept
    {
        return framesWritten != 0 && finalized && !firstError;
    }

    RecordingCompletion completion(bool stopRequested) const noexcept
    {
        if (canArchive()) return RecordingCompletion::Completed;
        if (!firstError && framesWritten == 0 && stopRequested)
            return RecordingCompletion::Cancelled;
        return framesWritten == 0 ? RecordingCompletion::Failed
                                  : RecordingCompletion::RecoverableFailure;
    }
};

} // namespace HuhuCapture::Core
