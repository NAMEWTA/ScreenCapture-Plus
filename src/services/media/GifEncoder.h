#pragma once

#include "core/MediaFrame.h"
#include "core/RecordingResult.h"
#include "platform/FileIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace HuhuCapture::Media {

struct GifEncoderOptions {
    int width{}, height{};
    std::uint64_t maximumFrameBytes{256ull * 1024 * 1024};
    std::uint64_t maximumOutputBytes{32ull * 1024 * 1024 * 1024};
};

// One recording worker owns this encoder. It consumes an already claimed empty
// regular-file writer, never reopens a path, and uses the existing fixed gifski
// C ABI in the single Rust aggregate (quality 80, fast, infinite repeat).
// add/finish may wait for real codec/disk work: never call them on the GUI.
// Destruction finishes and joins the codec before closing its callback's fd.
class GifEncoder final {
public:
    GifEncoder(GifEncoderOptions options, Platform::FileHandle writer);
    ~GifEncoder();
    GifEncoder(const GifEncoder&) = delete;
    GifEncoder& operator=(const GifEncoder&) = delete;
    GifEncoder(GifEncoder&&) = delete;
    GifEncoder& operator=(GifEncoder&&) = delete;

    bool ready() const noexcept;
    const Core::RecordingResult& result() const noexcept;
    // Caller-normalized media PTS, starting at exactly zero and strictly
    // increasing. Do not reinterpret an unrelated source clock as this clock.
    // Input is copied by gifski before return; borrowed pixels/leases may then
    // be reused. No out-of-order frame indices or unbounded service queue exist.
    bool addBgra(std::span<const std::uint8_t> pixels, std::size_t stride,
        Core::MediaTime presentationTime) noexcept;
    // CPU BGRA/RGBA, opaque SDR, already cropped and oriented. Unsupported
    // transforms/crops/color formats are rejected, never silently flattened.
    bool addFrame(const Core::FrameLease& frame, Core::MediaTime presentationTime) noexcept;
    // Normal stop drains every submitted input. finish is idempotent, checks
    // codec, actual I/O, flush and close, and keeps the first observed failure.
    // A clean empty encoder has no media; the session may classify it Cancelled.
    Core::RecordingResult finish() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Media
