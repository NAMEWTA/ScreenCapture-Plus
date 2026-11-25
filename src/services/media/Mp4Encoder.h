#pragma once

#include "core/MediaFrame.h"
#include "core/RecordingResult.h"
#include "platform/FileIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace HuhuCapture::Media {

// Audio reaches this seam after source clocks, resampling and mixing have been
// resolved. Every block is finite interleaved float PCM at the declared rate.
struct Mp4AudioFormat {
    int sampleRate{44100};
    int channels{2};
};

struct Mp4EncoderOptions {
    int width{}, height{};
    std::uint32_t framesPerSecond{30};
    std::optional<Mp4AudioFormat> audio{};
    std::uint64_t maximumFrameBytes{256ull * 1024 * 1024};
    std::uint64_t maximumQueuedAudioBytes{16ull * 1024 * 1024};
    std::uint64_t maximumOutputBytes{32ull * 1024 * 1024 * 1024};
};

struct Mp4EncoderStatistics {
    std::uint64_t videoPackets{}, audioPackets{}, audioSamples{};
    Core::MediaTime videoEnd{};
};

// Software recording encoder: the fixed libx264 + FFmpeg native AAC path.
// One recording worker owns it, including add/finish/destruction. It consumes
// an empty owned file descriptor and uses custom seek/write AVIO; no pathname,
// subprocess, GUI object or second Windows recording backend is involved.
// All video input must be normalized, opaque 8-bit SDR; addFrame borrows only
// for the call. First PTS is zero; duration is explicit and strictly positive.
class Mp4Encoder final {
public:
    Mp4Encoder(Mp4EncoderOptions options, Platform::FileHandle writer);
    ~Mp4Encoder();
    Mp4Encoder(const Mp4Encoder&) = delete;
    Mp4Encoder& operator=(const Mp4Encoder&) = delete;

    bool ready() const noexcept;
    bool addFrame(const Core::FrameLease& frame, Core::MediaTime time, Core::MediaTime duration) noexcept;
    bool addBgra(std::span<const std::uint8_t> pixels, std::size_t stride,
        Core::MediaTime time, Core::MediaTime duration) noexcept;
    // Only after the first video frame. firstSample is an exact contiguous
    // sample index, initially zero, not an unrelated OS timestamp. Missing,
    // reordered or nonfinite audio is rejected rather than silently padded.
    bool addAudio(std::span<const float> interleaved, std::uint64_t firstSample) noexcept;
    const Core::RecordingResult& result() const noexcept;
    const Mp4EncoderStatistics& statistics() const noexcept;
    Core::RecordingResult finish() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Media
