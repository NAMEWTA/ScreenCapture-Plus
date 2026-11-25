#pragma once

#include "core/MediaClock.h"
#include "core/FrameQueue.h"
#include "core/LiveSource.h"
#include "core/PixelBuffer.h"
#include "core/RecordingResult.h"
#include "platform/FileIdentity.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>

namespace HuhuCapture::Recording {

// The request's explicit coordinate domain determines the meaning of pixels.
// Desktop pixels may have negative monitor origins. Authorized source pixels
// refer to the normalized first frame of the same live source, never to an
// earlier screenshot or an inferred desktop DPR.
struct PixelRegion {
    std::int32_t x{}, y{}, width{}, height{};
    bool operator==(const PixelRegion&) const = default;
};

enum class Format { Mp4, Gif };
enum class Codec { H264, Hevc, Gif };
enum class CaptureSource { DesktopDuplication, Gdi, PipeWire, X11 };
enum class CoordinateSpace { DesktopPixels, AuthorizedSourcePixels };
enum class StopIntent { Save, Copy, Discard };

struct BackendRequest {
    Format format{Format::Mp4};
    PixelRegion region;
    std::uint32_t framesPerSecond{30};
    bool systemAudio{true};
    bool microphone{};
    bool cursor{true};
    std::chrono::milliseconds maximumDuration{std::chrono::minutes(120)};
    std::chrono::milliseconds firstFrameTimeout{5000};
    std::uint64_t maximumFrameBytes{256ull * 1024 * 1024};
    // Combined raw PCM pending in all selected input queues. This is not a
    // promise about the codec's private working set or every mixer allocation.
    std::uint64_t maximumQueuedAudioBytes{16ull * 1024 * 1024};
    CoordinateSpace coordinates{CoordinateSpace::DesktopPixels};
    std::uint64_t sourceId{}, generation{};
};

struct PreparedRecording {
    PixelRegion region;
    Codec codec{Codec::H264};
    CaptureSource source{CaptureSource::DesktopDuplication};
    Core::MediaClock::time_point started;
    bool systemAudio{}, microphone{};
    CoordinateSpace coordinates{CoordinateSpace::DesktopPixels};
    std::uint64_t sourceId{}, generation{};
};

struct BackendEvents {
    // Worker notifications contain values only. They may enqueue an owner-thread
    // update, but must never call a widget or wait for an owner-thread callback.
    std::function<void(const PreparedRecording&)> prepared;
    std::function<void()> finalizing;
};

struct BackendResult {
    Core::RecordingResult media;
    std::optional<PreparedRecording> prepared;
    Core::MediaTime elapsed{};
    bool stopRequested{};
    bool durationLimitReached{};
    std::optional<Core::FrameSourceTerminal> sourceTerminal;
    // First failure detail, separate from diagnostic terminal metadata.
    std::string detail;
};

// The writer is an already claimed empty regular file. The worker consumes it,
// closes every writer before returning, and never opens a caller-supplied path.
// Archive naming, publication, user intent and recovery belong to one shared
// RecordingSession. A backend cannot declare a file durably archived.
using Backend = std::function<BackendResult(const BackendRequest&, Platform::FileHandle,
    const BackendEvents&, std::stop_token)>;
// Application composition supplies the platform factory; shared UI/session
// code only transfers a once-claimed source into its task-specific provider.
using LiveBackendFactory = std::function<Backend(Core::LiveSourceConsumer)>;

// Preserve the native MP4 alignment without extending a small/out-of-bounds
// selection. The returned right/bottom never pass the requested rectangle.
inline std::optional<PixelRegion> alignedMp4Region(PixelRegion requested,
    std::uint64_t maximumBytes = 256ull * 1024 * 1024) noexcept
{
    if (requested.width <= 0 || requested.height <= 0) return {};
    const auto alignUp = [](std::int64_t value, std::int64_t alignment) {
        const auto remainder = (value % alignment + alignment) % alignment;
        return value + (alignment - remainder) % alignment;
    };
    const std::int64_t right = std::int64_t{requested.x} + requested.width;
    const std::int64_t bottom = std::int64_t{requested.y} + requested.height;
    const auto left = alignUp(requested.x, 4);
    const auto top = alignUp(requested.y, 2);
    const auto width = (right - left) / 4 * 4;
    const auto height = (bottom - top) / 2 * 2;
    constexpr auto maximum = (std::numeric_limits<std::int32_t>::max)();
    if (right > maximum || bottom > maximum || width < 4 || height < 2 ||
        width > 16384 || height > 16384) return {};
    const auto pixels = Core::pixelLayout(static_cast<int>(width), static_cast<int>(height), 4);
    if (!pixels || pixels->bytes > maximumBytes) return {};
    return PixelRegion{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

} // namespace HuhuCapture::Recording
