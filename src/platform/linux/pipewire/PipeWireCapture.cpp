/* Buffer negotiation and dequeue/process/queue lifecycle adapted from PipeWire
 * 1.6.11 src/examples/video-play.c and audio-capture.c.
 * SPDX-FileCopyrightText: Copyright © 2018, 2022 Wim Taymans
 * SPDX-License-Identifier: MIT
 * Fixed source and adaptation boundaries: docs/linux-pipewire.md.
 */
#include "platform/linux/pipewire/PipeWireCapture.h"
#include "platform/linux/pipewire/PipeWireMemFdMappings.h"
#include "core/PixelBuffer.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <new>
#include <utility>

namespace HuhuCapture::Platform::Linux {
namespace {
using Code = PipeWireFailure::Code;
using namespace Core;

struct Runtime {
    Runtime() { pw_init(nullptr, nullptr); }
    ~Runtime() { pw_deinit(); }
};

struct Capture {
    PipeWireCaptureOptions options;
    PipeWireCaptureCallbacks callbacks;
    FramePool pool;
    Detail::PipeWireMemFdMappings memFdMappings;
    PipeWireCaptureResult result;
    pw_main_loop* mainLoop{};
    pw_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_stream* stream{};
    spa_hook coreListener{};
    spa_hook streamListener{};
    spa_source* cancelEvent{};
    spa_source* firstFrameTimer{};
    spa_source* pauseTimer{};
    std::optional<FrameFormat> format;
    std::optional<std::int64_t> previousPts;
    std::optional<MediaClock::time_point> frameDeadline;
    std::optional<MediaClock::time_point> pauseDeadline;
    std::optional<bool> streaming;
    bool firstInFormat{true};
    bool connected{};
    bool finished{};

    Capture(PipeWireCaptureOptions values, PipeWireCaptureCallbacks handlers)
        : options(std::move(values)), callbacks(std::move(handlers)), pool(options.pool),
          memFdMappings(options.pool.slotCount == 0 ? 0 : (std::min)(
              options.pool.maximumBytes / options.pool.slotCount,
              static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()))) {}

    ~Capture()
    {
        // All callbacks and this teardown execute on the caller's worker. No PW
        // buffer has escaped process(), so remove_buffer cannot revoke a lease.
        clearMappings();
        if (stream) {
            spa_hook_remove(&streamListener);
            pw_stream_destroy(stream);
        }
        if (core) {
            spa_hook_remove(&coreListener);
            pw_core_disconnect(core);
        }
        if (firstFrameTimer) pw_loop_destroy_source(loop, firstFrameTimer);
        if (pauseTimer) pw_loop_destroy_source(loop, pauseTimer);
        if (cancelEvent) pw_loop_destroy_source(loop, cancelEvent);
        if (context) pw_context_destroy(context);
        if (mainLoop) pw_main_loop_destroy(mainLoop);
    }

    void fail(Code code, int nativeCode, const char* message) noexcept
    {
        if (!result.failure) {
            result.failure.emplace();
            result.failure->code = code;
            result.failure->nativeCode = nativeCode;
            try { result.failure->message = message ? message : ""; }
            catch (const std::bad_alloc&) {} // The code itself remains observable.
        }
        finished = true;
        activity(false);
    }

    void activity(bool active) noexcept
    {
        if (streaming && *streaming == active) return;
        streaming = active;
        if (!callbacks.activityChanged) return;
        try { callbacks.activityChanged(active); }
        catch (const std::bad_alloc&) { fail(Code::Allocation, -ENOMEM, "Activity consumer allocation failed"); }
        catch (const std::exception& error) { fail(Code::Consumer, -EIO, error.what()); }
        catch (...) { fail(Code::Consumer, -EIO, "Activity consumer threw an exception"); }
    }

    void pause(bool paused) noexcept
    {
        timespec deadline{};
        if (paused) {
            const auto now = MediaClock::now();
            if (!pauseDeadline) pauseDeadline = now + options.pauseTimeout;
            const auto nanoseconds = (std::max)(std::int64_t{1},
                std::chrono::duration_cast<std::chrono::nanoseconds>(*pauseDeadline - now).count());
            deadline = {static_cast<time_t>(nanoseconds / 1'000'000'000),
                        static_cast<long>(nanoseconds % 1'000'000'000)};
        } else pauseDeadline.reset();
        const int error = pw_loop_update_timer(loop, pauseTimer, &deadline, nullptr, false);
        if (error < 0) fail(Code::Stream, error, "Cannot update the native stream pause deadline");
    }

    bool clearMappings() noexcept
    {
        const int error = memFdMappings.clear();
        if (error < 0) fail(Code::Stream, error, "Cannot release PipeWire MemFd mappings");
        return error >= 0;
    }

    static void removeBuffer(void* value, pw_buffer* buffer) noexcept
    {
        auto& self = *static_cast<Capture*>(value);
        const int error = self.memFdMappings.remove(buffer);
        if (error < 0) self.fail(Code::Stream, error, "Cannot release a removed PipeWire buffer mapping");
    }

    bool armFirstFrame() noexcept
    {
        const auto now = MediaClock::now();
        if (!frameDeadline) frameDeadline = now + options.firstFrameTimeout;
        const auto nanoseconds = (std::max)(std::int64_t{1},
            std::chrono::duration_cast<std::chrono::nanoseconds>(*frameDeadline - now).count());
        timespec deadline{static_cast<time_t>(nanoseconds / 1'000'000'000),
                          static_cast<long>(nanoseconds % 1'000'000'000)};
        const int error = pw_loop_update_timer(loop, firstFrameTimer, &deadline, nullptr, false);
        if (error < 0) fail(Code::Initialization, error, "Cannot arm first-frame deadline");
        return error >= 0;
    }

    void disarmFirstFrame() noexcept
    {
        frameDeadline.reset();
        timespec disarmed{};
        const int error = pw_loop_update_timer(loop, firstFrameTimer, &disarmed, nullptr, false);
        if (error < 0) fail(Code::Stream, error, "Cannot disarm first-frame deadline");
    }

    static void cancel(void* value, std::uint64_t) noexcept
    {
        auto& self = *static_cast<Capture*>(value);
        self.result.stopped = true;
        self.finished = true;
        self.activity(false);
    }

    static void timedOut(void* value, std::uint64_t) noexcept
    {
        auto& self = *static_cast<Capture*>(value);
        if (!self.finished)
            self.fail(Code::FirstFrameTimeout, -ETIMEDOUT,
                      "No valid frame arrived before the negotiated-format deadline");
    }

    static void pauseTimedOut(void* value, std::uint64_t) noexcept
    {
        auto& self = *static_cast<Capture*>(value);
        if (!self.finished && self.pauseDeadline)
            self.fail(Code::PauseTimeout, -ETIMEDOUT, "The captured PipeWire stream remained paused past its recovery deadline");
    }

    static void coreError(void* value, std::uint32_t, int, int error, const char* message) noexcept
    {
        if (error < 0) static_cast<Capture*>(value)->fail(Code::Connection, error, message);
    }

    static void stateChanged(void* value, pw_stream_state, pw_stream_state state,
                             const char* error) noexcept
    {
        auto& self = *static_cast<Capture*>(value);
        if (self.finished) return;
        if (state == PW_STREAM_STATE_ERROR)
            self.fail(Code::Stream, -EIO, error);
        else if (state == PW_STREAM_STATE_UNCONNECTED && self.connected)
            self.fail(Code::Connection, -EPIPE, "The authorized PipeWire stream disconnected");
        else if (state != PW_STREAM_STATE_UNCONNECTED)
            self.connected = true;
        if (self.finished) return;
        if (state == PW_STREAM_STATE_STREAMING) {
            self.pause(false);
            if (!self.finished) self.activity(true);
        } else if (state == PW_STREAM_STATE_PAUSED) {
            self.activity(false);
            // Initial negotiation is already protected by first-frame timeout.
            // Never apply this deadline to an otherwise active static desktop.
            if (!self.finished && self.result.stats.framesDelivered) self.pause(true);
        }
    }

    static bool readColor(const spa_video_info_raw& raw, FrameColor& color) noexcept
    {
        // These fixed SPA enums and our independent enums follow the same
        // documented order. Validate every input before converting it.
        if (raw.color_primaries < SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN ||
            raw.color_primaries > SPA_VIDEO_COLOR_PRIMARIES_EBU3213 ||
            raw.transfer_function < SPA_VIDEO_TRANSFER_UNKNOWN ||
            raw.transfer_function > SPA_VIDEO_TRANSFER_BT601 ||
            raw.color_matrix < SPA_VIDEO_COLOR_MATRIX_UNKNOWN ||
            raw.color_matrix > SPA_VIDEO_COLOR_MATRIX_BT2020 ||
            raw.color_range < SPA_VIDEO_COLOR_RANGE_UNKNOWN ||
            raw.color_range > SPA_VIDEO_COLOR_RANGE_16_235)
            return false;
        color = {static_cast<ColorPrimaries>(raw.color_primaries),
                 static_cast<ColorTransfer>(raw.transfer_function),
                 static_cast<ColorMatrix>(raw.color_matrix),
                 static_cast<ColorRange>(raw.color_range)};
        return true;
    }

    void changeFormat(const spa_pod* parameter)
    {
        if (!parameter) {
            if (!clearMappings()) return;
            format.reset();
            pool.clear();
            firstInFormat = true;
            armFirstFrame();
            return;
        }
        std::uint32_t type{}, subtype{};
        spa_video_info_raw raw{};
        if (spa_format_parse(parameter, &type, &subtype) < 0 || type != SPA_MEDIA_TYPE_video ||
            subtype != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(parameter, &raw) < 0) {
            fail(Code::UnsupportedFormat, -ENOTSUP, "Only raw 8-bit RGB screen frames are supported");
            return;
        }
        FramePixelFormat pixels;
        switch (raw.format) {
        case SPA_VIDEO_FORMAT_BGRA:
        case SPA_VIDEO_FORMAT_BGRx: pixels = FramePixelFormat::Bgra8; break;
        case SPA_VIDEO_FORMAT_RGBA:
        case SPA_VIDEO_FORMAT_RGBx: pixels = FramePixelFormat::Rgba8; break;
        default:
            fail(Code::UnsupportedFormat, -ENOTSUP, "Negotiated pixel format is not a supported 8-bit RGB format");
            return;
        }
        if ((raw.flags & (SPA_VIDEO_FLAG_MODIFIER | SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED |
                          SPA_VIDEO_FLAG_PREMULTIPLIED_ALPHA)) != 0 ||
            raw.interlace_mode != SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE || raw.views > 1 ||
            (raw.multiview_mode != SPA_VIDEO_MULTIVIEW_MODE_NONE && raw.multiview_mode != SPA_VIDEO_MULTIVIEW_MODE_MONO) ||
            (raw.pixel_aspect_ratio.denom != 0 && raw.pixel_aspect_ratio.num != raw.pixel_aspect_ratio.denom)) {
            fail(Code::UnsupportedFormat, -ENOTSUP, "Modifier, premultiplied, interlaced or non-square/multiview frames are unsupported");
            return;
        }
        FrameColor color;
        if (!readColor(raw, color) || color.transfer == ColorTransfer::Pq || color.transfer == ColorTransfer::Hlg) {
            fail(Code::UnsupportedFormat, -ENOTSUP, "HDR or unknown color metadata requires an explicitly verified conversion path");
            return;
        }
        // Unspecified legacy RGB color fields remain unspecified. They are not
        // relabeled as measured sRGB, nor do we claim HDR tone mapping happened.
        if (raw.size.width == 0 || raw.size.height == 0 ||
            raw.size.width > static_cast<std::uint32_t>(options.pool.maximumDimension) ||
            raw.size.height > static_cast<std::uint32_t>(options.pool.maximumDimension)) {
            fail(Code::Budget, -EOVERFLOW, "Negotiated screen dimensions exceed the capture budget");
            return;
        }
        const auto next = packedFrameFormat(pixels, static_cast<int>(raw.size.width),
                                            static_cast<int>(raw.size.height), color);
        if (!next || next->bytes > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
            fail(Code::Budget, -EOVERFLOW, "Negotiated frame size cannot be represented safely");
            return;
        }
        // Repeated identical Format notifications do not retire a live pool or
        // create an artificial second-generation budget/first-frame deadline.
        if (!format || *format != *next) {
            if (!clearMappings()) return;
            format.reset();
            pool.clear();
            firstInFormat = true;
            if (!armFirstFrame()) return;
            const auto status = pool.configure(*next);
            if (status != FramePoolStatus::Ready) {
                fail(status == FramePoolStatus::AllocationFailed ? Code::Allocation : Code::Budget,
                     status == FramePoolStatus::AllocationFailed ? -ENOMEM : -ENOBUFS,
                     "Cannot allocate the new frame generation within the live-lease budget");
                return;
            }
            format = *next;
            ++result.stats.formatChanges;
            result.stats.peakPoolBytes = (std::max)(result.stats.peakPoolBytes, pool.allocatedBytes());
            if (callbacks.formatChanged && !callbacks.formatChanged(*format, pool.formatGeneration())) {
                fail(Code::Consumer, -ENOTSUP, "The consumer rejected the newly negotiated format");
                return;
            }
        }
        std::uint8_t bytes[1024];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, bytes, sizeof(bytes));
        const spa_pod* parameters[4];
        const auto maximumPlaneBytes = (std::min)(options.pool.maximumBytes / options.pool.slotCount,
            static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()));
        const auto maximumStride = maximumPlaneBytes / static_cast<unsigned>(format->height);
        parameters[0] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 4),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(static_cast<int>(format->bytes),
                static_cast<int>(format->bytes), static_cast<int>(maximumPlaneBytes)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(static_cast<int>(format->planes[0].stride),
                static_cast<int>(format->planes[0].stride), static_cast<int>(maximumStride)),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
        const std::array<std::pair<std::uint32_t, int>, 3> metadata{{
            {SPA_META_Header, sizeof(spa_meta_header)},
            {SPA_META_VideoCrop, sizeof(spa_meta_region)},
            {SPA_META_VideoTransform, sizeof(spa_meta_videotransform)}
        }};
        for (std::size_t i = 0; i < metadata.size(); ++i)
            parameters[i + 1] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(metadata[i].first),
                SPA_PARAM_META_size, SPA_POD_Int(metadata[i].second)));
        if (std::ranges::any_of(parameters, [](auto* p) { return p == nullptr; })) {
            fail(Code::Initialization, -ENOSPC, "PipeWire buffer negotiation exceeded its bounded builder");
            return;
        }
        const int error = pw_stream_update_params(stream, parameters, 4);
        if (error < 0) fail(Code::Stream, error, "PipeWire rejected the CPU buffer layout");
    }

    static void paramChanged(void* value, std::uint32_t id, const spa_pod* parameter) noexcept
    {
        auto& self = *static_cast<Capture*>(value);
        if (id != SPA_PARAM_Format || self.finished) return;
        try { self.changeFormat(parameter); }
        catch (const std::bad_alloc&) { self.fail(Code::Allocation, -ENOMEM, "Frame format allocation failed"); }
        catch (const std::exception& error) { self.fail(Code::Consumer, -EIO, error.what()); }
        catch (...) { self.fail(Code::Consumer, -EIO, "Frame format consumer threw an exception"); }
    }

    template<class T>
    std::optional<T> readMeta(const spa_buffer& buffer, std::uint32_t type)
    {
        const auto* meta = spa_buffer_find_meta(&buffer, type);
        if (!meta) return std::nullopt;
        if (!meta->data || meta->size < sizeof(T)) {
            fail(Code::InvalidBuffer, -EINVAL, "PipeWire frame metadata is shorter than its declared type");
            return std::nullopt;
        }
        T value;
        std::memcpy(&value, meta->data, sizeof(value));
        return value;
    }

    FrameLease copyFrame(const pw_buffer* nativeBuffer)
    {
        if (!format) {
            fail(Code::InvalidBuffer, -EINVAL, "Received a frame without a negotiated format");
            return {};
        }
        const auto* buffer = nativeBuffer ? nativeBuffer->buffer : nullptr;
        if (!buffer || buffer->n_datas != 1 || !buffer->datas ||
            (buffer->n_metas != 0 && !buffer->metas)) {
            fail(Code::InvalidBuffer, -EINVAL, "The RGB frame has an invalid plane/metadata array");
            return {};
        }
        const auto& data = buffer->datas[0];
        if (data.type != SPA_DATA_MemFd && data.type != SPA_DATA_MemPtr) {
            fail(Code::UnsupportedMemory, -ENOTSUP, "DMA-BUF and non-CPU memory are not linear frame buffers");
            return {};
        }
        if (!data.chunk) {
            fail(Code::InvalidBuffer, -EINVAL, "PipeWire did not provide a CPU chunk descriptor");
            return {};
        }
        const auto header = readMeta<spa_meta_header>(*buffer, SPA_META_Header);
        const auto crop = readMeta<spa_meta_region>(*buffer, SPA_META_VideoCrop);
        const auto transform = readMeta<spa_meta_videotransform>(*buffer, SPA_META_VideoTransform);
        if (finished) return {};
        if ((data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0 ||
            (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED) != 0)) {
            ++result.stats.corruptedDrops;
            return {};
        }
        if (data.chunk->size == 0 || (data.chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0 ||
            (header && (header->flags & SPA_META_HEADER_FLAG_GAP) != 0)) {
            ++result.stats.emptyDrops;
            return {};
        }
        if ((!data.data && data.type != SPA_DATA_MemFd) || data.maxsize == 0 ||
            data.maxsize > options.pool.maximumBytes / options.pool.slotCount) {
            // Record only bounded scalar descriptors, never pixels, addresses or
            // the FD number. Preserve the strict failure while diagnosing the
            // actual producer/daemon/client mapping contract on a real desktop.
            char detail[512]{};
            std::snprintf(detail, sizeof(detail),
                "PipeWire did not provide a mapped CPU plane within its byte limit "
                "(type=%u flags=%u fdPresent=%u dataPresent=%u mapoffset=%u maxsize=%u "
                "chunkOffset=%u chunkSize=%u chunkStride=%d chunkFlags=%d byteLimit=%zu)",
                static_cast<unsigned>(data.type), static_cast<unsigned>(data.flags),
                static_cast<unsigned>(data.fd >= 0), static_cast<unsigned>(data.data != nullptr),
                static_cast<unsigned>(data.mapoffset), static_cast<unsigned>(data.maxsize),
                static_cast<unsigned>(data.chunk->offset), static_cast<unsigned>(data.chunk->size),
                static_cast<int>(data.chunk->stride), static_cast<int>(data.chunk->flags),
                options.pool.maximumBytes / options.pool.slotCount);
            fail(Code::InvalidBuffer, -EINVAL, detail);
            return {};
        }
        const auto offset = static_cast<std::size_t>(data.chunk->offset % data.maxsize);
        const auto size = static_cast<std::size_t>((std::min)(data.chunk->size, data.maxsize));
        std::ptrdiff_t stride = data.chunk->stride;
        if (stride == 0 && size % static_cast<unsigned>(format->height) == 0)
            stride = static_cast<std::ptrdiff_t>(size / static_cast<unsigned>(format->height));
        if (stride < 0) {
            fail(Code::UnsupportedMemory, -ENOTSUP,
                 "Negative PipeWire chunk stride needs a verified first-row/storage convention");
            return {};
        }
        const FramePlaneLayout source{offset, size, 0, stride,
            format->planes[0].rowBytes, static_cast<std::uint32_t>(format->height)};
        if (!validFramePlane(source, data.maxsize)) {
            fail(Code::InvalidBuffer, -EOVERFLOW, "PipeWire rows extend beyond the valid CPU chunk");
            return {};
        }
        FrameMetadata metadata;
        metadata.sessionId = options.sessionId;
        metadata.sourceId = options.sourceId;
        metadata.timing.receivedAt = MediaClock::now();
        metadata.timing.discontinuity = firstInFormat;
        metadata.timing.clockInstance = options.sessionId;
        if (header) {
            metadata.timing.sourceSequence = header->seq;
            if (header->pts != SPA_TIME_INVALID) {
                metadata.timing.sourcePts = std::chrono::nanoseconds(header->pts);
                metadata.timing.sourceClock = FrameClockDomain::PipeWire;
                if (previousPts && header->pts < *previousPts) metadata.timing.discontinuity = true;
                previousPts = header->pts;
            }
            metadata.timing.discontinuity |= (header->flags & SPA_META_HEADER_FLAG_DISCONT) != 0;
        }
        if (crop && spa_meta_region_is_valid(&*crop)) {
            if (crop->region.position.x < 0 || crop->region.position.y < 0) {
                fail(Code::InvalidBuffer, -EINVAL, "The frame crop has a negative pixel origin");
                return {};
            }
            metadata.crop = FrameCrop{static_cast<std::uint32_t>(crop->region.position.x),
                static_cast<std::uint32_t>(crop->region.position.y), crop->region.size.width, crop->region.size.height};
        }
        if (transform) metadata.transform = static_cast<FrameTransform>(transform->transform);
        if (!validFrameMetadata(metadata, *format)) {
            fail(Code::InvalidBuffer, -EINVAL, "The frame crop/transform is outside the negotiated pixels");
            return {};
        }
        auto writable = pool.tryAcquire();
        if (!writable) {
            ++result.stats.poolDrops;
            return {};
        }
        const auto* cpuData = static_cast<const std::uint8_t*>(data.data);
        if (!cpuData) {
            // Some older producer/daemon combinations deliver valid MemFd with
            // no SPA MAPPABLE/READABLE flags. Use the authorized core's existing
            // block and PipeWire's bounded read-only mapping implementation.
            // Keep this pointer local; only the owned FrameLease leaves process.
            const auto mapped = memFdMappings.read(pw_core_get_mempool(core), nativeBuffer, data);
            if (!mapped) {
                const auto code = mapped.error == -ENOBUFS ? Code::Budget :
                    mapped.error == -ENOTSUP ? Code::UnsupportedMemory :
                    mapped.error == -ENOMEM ? Code::Allocation : Code::InvalidBuffer;
                fail(code, mapped.error, mapped.message);
                return {};
            }
            cpuData = mapped.pixels;
        }
        const auto* pixels = cpuData + offset;
        for (std::uint32_t y = 0; y < source.rows; ++y) {
            auto destination = writable->row(0, y);
            std::memcpy(destination.data(), pixels + static_cast<std::size_t>(stride) * y, source.rowBytes);
            makeOpaque(destination);
        }
        if (data.type == SPA_DATA_MemFd) ++result.stats.memFdFrames;
        else ++result.stats.memPtrFrames;
        if (firstInFormat) {
            firstInFormat = false;
            disarmFirstFrame();
        }
        return std::move(*writable).publish(std::move(metadata));
    }

    struct ReturnBuffer {
        Capture& owner;
        pw_buffer* buffer;
        ~ReturnBuffer()
        {
            const int error = pw_stream_queue_buffer(owner.stream, buffer);
            if (error < 0) owner.fail(Code::Stream, error, "Cannot return the captured PipeWire buffer");
            else ++owner.result.stats.buffersReturned;
        }
    };

    void processFrames()
    {
        // Negotiate at most four PW buffers. Bound each event even if a peer
        // supplies more, so stop/deadline events are not starved by a producer.
        for (unsigned count = 0; count < 16 && !finished; ++count) {
            auto* buffer = pw_stream_dequeue_buffer(stream);
            if (!buffer) return;
            ++result.stats.buffersDequeued;
            FrameLease frame;
            {
                ReturnBuffer giveBack{*this, buffer};
                frame = copyFrame(buffer);
            }
            if (!frame || finished) continue;
            ++result.stats.framesDelivered;
            if (!callbacks.frame(std::move(frame))) {
                result.stopped = true;
                finished = true;
                activity(false);
            }
        }
    }

    static void process(void* value) noexcept
    {
        auto& self = *static_cast<Capture*>(value);
        try { self.processFrames(); }
        catch (const std::bad_alloc&) { self.fail(Code::Allocation, -ENOMEM, "Frame consumer allocation failed"); }
        catch (const std::exception& error) { self.fail(Code::Consumer, -EIO, error.what()); }
        catch (...) { self.fail(Code::Consumer, -EIO, "Frame consumer threw an exception"); }
    }
};

const pw_core_events coreEvents = [] {
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.error = Capture::coreError;
    return events;
}();
const pw_stream_events streamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = Capture::stateChanged;
    events.param_changed = Capture::paramChanged;
    events.remove_buffer = Capture::removeBuffer;
    events.process = Capture::process;
    return events;
}();

} // namespace

PipeWireCaptureResult runPipeWireCapture(UniqueFd remote, PipeWireTarget target,
    PipeWireCaptureOptions options, PipeWireCaptureCallbacks callbacks, std::stop_token stop)
{
    try {
        Runtime runtime;
        Capture capture(std::move(options), std::move(callbacks));
        if (!remote || (!target.serial && (target.nodeId == PW_ID_ANY || target.nodeId == 0)) ||
            (target.serial && *target.serial == 0) ||
            capture.options.sessionId == 0 || capture.options.sourceId == 0 || !capture.callbacks.frame ||
            capture.options.firstFrameTimeout.count() <= 0 ||
            capture.options.firstFrameTimeout > std::chrono::minutes(5) ||
            capture.options.pauseTimeout.count() <= 0 || capture.options.pauseTimeout > std::chrono::minutes(5) ||
            capture.options.pool.maximumDimension <= 0 || capture.options.pool.slotCount == 0 ||
            capture.options.pool.maximumBytes == 0 || capture.options.maximumFrameRate > 1000) {
            capture.fail(Code::InvalidArgument, -EINVAL, "Invalid authorized target, callback, identities or capture limits");
            return std::move(capture.result);
        }
        if (stop.stop_requested()) {
            capture.result.stopped = true;
            capture.activity(false);
            return std::move(capture.result);
        }
        const int fdFlags = fcntl(remote.get(), F_GETFD);
        if (fdFlags < 0 || fcntl(remote.get(), F_SETFD, fdFlags | FD_CLOEXEC) < 0) {
            capture.fail(Code::Connection, -errno, "Cannot protect the authorized PipeWire FD across exec");
            return std::move(capture.result);
        }
        capture.mainLoop = pw_main_loop_new(nullptr);
        if (!capture.mainLoop) {
            capture.fail(Code::Initialization, -errno, "Cannot create the independent PipeWire loop");
            return std::move(capture.result);
        }
        capture.loop = pw_main_loop_get_loop(capture.mainLoop);
        capture.cancelEvent = pw_loop_add_event(capture.loop, Capture::cancel, &capture);
        capture.firstFrameTimer = pw_loop_add_timer(capture.loop, Capture::timedOut, &capture);
        capture.pauseTimer = pw_loop_add_timer(capture.loop, Capture::pauseTimedOut, &capture);
        if (!capture.cancelEvent || !capture.firstFrameTimer || !capture.pauseTimer) {
            capture.fail(Code::Initialization, -errno, "Cannot create capture stop/deadline event sources");
            return std::move(capture.result);
        }
        auto* contextProperties = pw_properties_new(PW_KEY_APP_NAME, "huhucapture", "module.rt", "false", nullptr);
        if (!contextProperties) {
            capture.fail(Code::Allocation, -ENOMEM, "Cannot allocate PipeWire context properties");
            return std::move(capture.result);
        }
        capture.context = pw_context_new(capture.loop, contextProperties, 0);
        if (!capture.context) {
            capture.fail(Code::Initialization, -errno, "Cannot load the pinned PipeWire client modules");
            return std::move(capture.result);
        }
        // The reviewed dependency patch makes the documented ownership transfer
        // true even when core/protocol initialization fails before a loop source.
        capture.core = pw_context_connect_fd(capture.context, remote.release(), nullptr, 0);
        if (!capture.core) {
            capture.fail(Code::Connection, -errno, "Cannot connect to the authorized PipeWire remote");
            return std::move(capture.result);
        }
        pw_core_add_listener(capture.core, &capture.coreListener, &coreEvents, &capture);
        auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen", nullptr);
        if (!properties) {
            capture.fail(Code::Allocation, -ENOMEM, "Cannot allocate PipeWire stream properties");
            return std::move(capture.result);
        }
        if (target.serial) {
            char serial[21]{};
            const auto converted = std::to_chars(serial, serial + sizeof(serial) - 1, *target.serial);
            if (converted.ec != std::errc{}) {
                pw_properties_free(properties);
                capture.fail(Code::InvalidArgument, -EINVAL, "Cannot represent the authorized PipeWire serial");
                return std::move(capture.result);
            }
            const int error = pw_properties_set(properties, PW_KEY_TARGET_OBJECT, serial);
            if (error < 0) {
                pw_properties_free(properties);
                capture.fail(Code::Allocation, error, "Cannot set the authorized PipeWire serial");
                return std::move(capture.result);
            }
        }
        capture.stream = pw_stream_new(capture.core, "huhucapture screen capture", properties);
        if (!capture.stream) {
            capture.fail(Code::Initialization, -errno, "Cannot create the authorized PipeWire stream");
            return std::move(capture.result);
        }
        pw_stream_add_listener(capture.stream, &capture.streamListener, &streamEvents, &capture);
        std::uint8_t bytes[1024];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, bytes, sizeof(bytes));
        spa_pod_frame object;
        spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&builder,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx), 0);
        const spa_rectangle minimum{1, 1};
        const spa_rectangle maximum{static_cast<std::uint32_t>(capture.options.pool.maximumDimension),
                                    static_cast<std::uint32_t>(capture.options.pool.maximumDimension)};
        const spa_rectangle preferred{(std::min)(1920u, maximum.width), (std::min)(1080u, maximum.height)};
        spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum), 0);
        if (capture.options.maximumFrameRate != 0) {
            const spa_fraction minimumRate{0, 1};
            const spa_fraction maximumRate{capture.options.maximumFrameRate, 1};
            spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_framerate,
                SPA_POD_CHOICE_RANGE_Fraction(&maximumRate, &minimumRate, &maximumRate), 0);
        }
        const spa_pod* parameters[] = {static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &object))};
        if (!parameters[0]) {
            capture.fail(Code::Initialization, -ENOSPC, "PipeWire format negotiation exceeded its bounded builder");
            return std::move(capture.result);
        }
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT);
        const int error = pw_stream_connect(capture.stream, PW_DIRECTION_INPUT,
            target.serial ? PW_ID_ANY : target.nodeId, flags, parameters, 1);
        if (error < 0) capture.fail(Code::Connection, error, "Cannot connect the authorized screen node");
        if (capture.finished || !capture.armFirstFrame()) return std::move(capture.result);
        {
            // stop_callback destruction waits for a concurrently executing
            // signal callback before any source/loop can be destroyed.
            std::stop_callback cancelWake(stop, [&capture] {
                pw_loop_signal_event(capture.loop, capture.cancelEvent);
            });
            pw_loop_enter(capture.loop);
            while (!capture.finished) {
                const int status = pw_loop_iterate(capture.loop, -1);
                if (status < 0 && status != -EINTR)
                    capture.fail(Code::Stream, status, "PipeWire event dispatch failed");
            }
            pw_loop_leave(capture.loop);
        }
        capture.clearMappings();
        return std::move(capture.result);
    } catch (const std::bad_alloc&) {
        PipeWireCaptureResult result;
        result.failure.emplace();
        result.failure->code = Code::Allocation;
        result.failure->nativeCode = -ENOMEM;
        return result;
    }
}

} // namespace HuhuCapture::Platform::Linux
