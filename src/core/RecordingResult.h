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

// Actual native sink counters collected after synchronous finalization. These
// are diagnostics, not a replacement for complete media inspection. A converter
// may change the number of samples between WriteSample and encoder output.
// All timestamps and durations use signed 100 ns media units.
struct RecordingSinkStatistics {
    std::int64_t queryStatus{};
    std::uint64_t received{}, encoded{}, processed{};
    std::int64_t lastReceived{}, lastEncoded{}, lastProcessed{};
    std::int64_t lastSubmitted{}, lastSubmittedDuration{};
    std::uint32_t queuedBytes{};
};

// One writer (the recording worker); the owner reads only after synchronized
// worker completion, through a join or its completed future.
// Successful finalization must never erase an earlier capture/encoding error.
// An error with some frames is recoverable data, not a successful archive item.
struct RecordingResult {
    // Successfully submitted input frames. This is not the encoded output frame
    // count: MF frame-rate conversion and GIF frame merging can change it.
    std::uint64_t framesWritten{};
    std::optional<RecordingError> firstError;
    std::optional<RecordingError> finalizationError;
    bool finalized{};
    std::optional<RecordingSinkStatistics> sinkStatistics;

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
