/* Encoder send/receive, stream setup and muxing flow adapted from FFmpeg
 * 9.0.2 doc/examples/mux.c (MIT), Copyright (c) 2003 Fabrice Bellard.
 * Exact source and license: third_party/ffmpeg-examples/.
 */
#include "recording/SoftwareMp4Recording.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace HuhuCapture::Recording {
namespace {
using Core::RecordingStage;
using Clock = Core::MediaClock;
constexpr AVRational videoTimeBase{1, 1'000'000};
constexpr AVRational audioTimeBase{1, AudioSource::sampleRate};
constexpr std::int64_t maximumOutputBytes = 32ll * 1024 * 1024 * 1024;

struct Output {
    int descriptor;
    int error{};
    std::int64_t position{};
    int fail(int value) noexcept { if (!error) error = value ? value : EIO; return AVERROR(error); }
    static int write(void* opaque, const std::uint8_t* data, int bytes) noexcept
    {
        auto& self = *static_cast<Output*>(opaque);
        if (self.error) return AVERROR(self.error);
        if (!data || bytes < 0 || self.position < 0 || bytes > maximumOutputBytes - self.position)
            return self.fail(EFBIG);
        const int requested = bytes;
        while (bytes) {
#ifdef _WIN32
            const auto written = _write(self.descriptor, data, static_cast<unsigned>(bytes));
#else
            const auto written = ::write(self.descriptor, data, static_cast<std::size_t>(bytes));
#endif
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return self.fail(written == 0 ? EIO : errno);
            data += written; bytes -= static_cast<int>(written); self.position += written;
        }
        return requested;
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) noexcept
    {
        auto& self = *static_cast<Output*>(opaque);
        if (self.error) return AVERROR(self.error);
        if (whence == AVSEEK_SIZE) {
            std::error_code error;
            const auto snapshot = Platform::readFileDescriptorSnapshot(self.descriptor, error);
            if (!snapshot || snapshot->bytes > static_cast<std::uint64_t>(maximumOutputBytes))
                return self.fail(error ? error.value() : EFBIG);
            return static_cast<std::int64_t>(snapshot->bytes);
        }
        whence &= ~AVSEEK_FORCE;
        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return self.fail(EINVAL);
#ifdef _WIN32
        const auto next = _lseeki64(self.descriptor, offset, whence);
#else
        const auto next = lseek(self.descriptor, static_cast<off_t>(offset), whence);
#endif
        if (next < 0) return self.fail(errno);
        if (next > maximumOutputBytes) return self.fail(EFBIG);
        self.position = next;
        return next;
    }
};

AVColorPrimaries primaries(Core::ColorPrimaries value) noexcept
{
    using P = Core::ColorPrimaries;
    switch (value) {
    case P::Bt709: return AVCOL_PRI_BT709;
    case P::Bt470M: return AVCOL_PRI_BT470M;
    case P::Bt470Bg: return AVCOL_PRI_BT470BG;
    case P::Smpte170M: return AVCOL_PRI_SMPTE170M;
    case P::Smpte240M: return AVCOL_PRI_SMPTE240M;
    case P::Film: return AVCOL_PRI_FILM;
    case P::Bt2020: return AVCOL_PRI_BT2020;
    case P::Smpte428: return AVCOL_PRI_SMPTE428;
    case P::DciP3: return AVCOL_PRI_SMPTE431;
    case P::DisplayP3: return AVCOL_PRI_SMPTE432;
    case P::Ebu3213: return AVCOL_PRI_EBU3213;
    default: return AVCOL_PRI_UNSPECIFIED;
    }
}
AVColorTransferCharacteristic transfer(Core::ColorTransfer value) noexcept
{
    using T = Core::ColorTransfer;
    switch (value) {
    case T::Linear: return AVCOL_TRC_LINEAR;
    case T::Gamma22: return AVCOL_TRC_GAMMA22;
    case T::Gamma28: return AVCOL_TRC_GAMMA28;
    case T::Bt709: return AVCOL_TRC_BT709;
    case T::Bt601: return AVCOL_TRC_SMPTE170M;
    case T::Smpte240M: return AVCOL_TRC_SMPTE240M;
    case T::Srgb: return AVCOL_TRC_IEC61966_2_1;
    case T::Log100: return AVCOL_TRC_LOG;
    case T::Log316: return AVCOL_TRC_LOG_SQRT;
    case T::Bt2020_10: return AVCOL_TRC_BT2020_10;
    case T::Bt2020_12: return AVCOL_TRC_BT2020_12;
    default: return AVCOL_TRC_UNSPECIFIED;
    }
}

struct Encoder {
    AVFormatContext* container{};
    AVIOContext* io{};
    AVCodecContext* video{};
    AVCodecContext* audio{};
    AVStream* videoStream{};
    AVStream* audioStream{};
    AVFrame* videoFrame{};
    AVFrame* audioFrame{};
    AVPacket* packet{};
    SwsContext* converter{};
    bool header{}, videoOpened{}, audioOpened{};
    Core::FrameFormat sourceFormat;
    std::vector<float> pcm;

    ~Encoder()
    {
        sws_freeContext(converter);
        av_frame_free(&videoFrame); av_frame_free(&audioFrame); av_packet_free(&packet);
        avcodec_free_context(&video); avcodec_free_context(&audio);
        avformat_free_context(container);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
    int open(const BackendRequest& request, const Core::FrameFormat& input, Output& output)
    {
        sourceFormat = input;
        const auto* videoCodec = avcodec_find_encoder_by_name("libx264");
        const auto* audioCodec = request.systemAudio || request.microphone ? avcodec_find_encoder_by_name("aac") : nullptr;
        if (!videoCodec || ((request.systemAudio || request.microphone) && !audioCodec)) return AVERROR_ENCODER_NOT_FOUND;
        int status = avformat_alloc_output_context2(&container, nullptr, "mp4", nullptr);
        if (status < 0 || !container) return status < 0 ? status : AVERROR(ENOMEM);
        packet = av_packet_alloc(); videoFrame = av_frame_alloc();
        videoStream = avformat_new_stream(container, nullptr); video = avcodec_alloc_context3(videoCodec);
        if (!packet || !videoFrame || !videoStream || !video) return AVERROR(ENOMEM);
        video->width = request.region.width; video->height = request.region.height;
        video->pix_fmt = AV_PIX_FMT_YUV420P; video->time_base = videoTimeBase;
        video->framerate = AVRational{static_cast<int>(request.framesPerSecond), 1};
        video->gop_size = static_cast<int>(request.framesPerSecond * 2); video->max_b_frames = 0;
        video->color_range = AVCOL_RANGE_MPEG; video->colorspace = AVCOL_SPC_BT709;
        video->color_primaries = primaries(input.color.primaries); video->color_trc = transfer(input.color.transfer);
        // Duration is supplied by the actual monotonic frame timeline. Without
        // this flag FFmpeg clears AVFrame.duration before libx264 sees it.
        video->flags |= AV_CODEC_FLAG_FRAME_DURATION;
        if (container->oformat->flags & AVFMT_GLOBALHEADER) video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((status = av_opt_set(video->priv_data, "preset", "veryfast", 0)) < 0 ||
            (status = av_opt_set(video->priv_data, "tune", "zerolatency", 0)) < 0 ||
            (status = av_opt_set(video->priv_data, "crf", "23", 0)) < 0) return status;
        if ((status = avcodec_open2(video, videoCodec, nullptr)) < 0) return status;
        videoOpened = true; videoStream->time_base = video->time_base;
        if ((status = avcodec_parameters_from_context(videoStream->codecpar, video)) < 0) return status;
        videoFrame->format = video->pix_fmt; videoFrame->width = video->width; videoFrame->height = video->height;
        videoFrame->color_range = video->color_range; videoFrame->colorspace = video->colorspace;
        videoFrame->color_primaries = video->color_primaries; videoFrame->color_trc = video->color_trc;
        if ((status = av_frame_get_buffer(videoFrame, 32)) < 0) return status;
        const auto pixels = input.pixelFormat == Core::FramePixelFormat::Bgra8 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
        converter = sws_getContext(video->width, video->height, pixels, video->width, video->height,
            video->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!converter) return AVERROR(ENOMEM);
        const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        if ((status = sws_setColorspaceDetails(converter, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16)) < 0)
            return status;
        if (audioCodec) {
            audioStream = avformat_new_stream(container, nullptr); audio = avcodec_alloc_context3(audioCodec);
            audioFrame = av_frame_alloc();
            if (!audioStream || !audio || !audioFrame) return AVERROR(ENOMEM);
            audio->sample_fmt = AV_SAMPLE_FMT_FLTP; audio->sample_rate = AudioSource::sampleRate;
            audio->time_base = audioTimeBase; audio->bit_rate = 192'000;
            av_channel_layout_default(&audio->ch_layout, AudioSource::channels);
            if (container->oformat->flags & AVFMT_GLOBALHEADER) audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if ((status = avcodec_open2(audio, audioCodec, nullptr)) < 0) return status;
            audioOpened = true;
            if (audio->frame_size <= 0 || audio->frame_size > 4096 || audio->sample_fmt != AV_SAMPLE_FMT_FLTP ||
                audio->sample_rate != static_cast<int>(AudioSource::sampleRate) || audio->ch_layout.nb_channels != 2)
                return AVERROR(EINVAL);
            audioStream->time_base = audio->time_base;
            if ((status = avcodec_parameters_from_context(audioStream->codecpar, audio)) < 0) return status;
            audioFrame->format = audio->sample_fmt; audioFrame->sample_rate = audio->sample_rate;
            audioFrame->nb_samples = audio->frame_size;
            if ((status = av_channel_layout_copy(&audioFrame->ch_layout, &audio->ch_layout)) < 0 ||
                (status = av_frame_get_buffer(audioFrame, 0)) < 0) return status;
            pcm.resize(static_cast<std::size_t>(audio->frame_size) * AudioSource::channels);
        }
        constexpr int ioBufferBytes = 64 * 1024;
        auto* buffer = static_cast<unsigned char*>(av_malloc(ioBufferBytes));
        if (!buffer) return AVERROR(ENOMEM);
        io = avio_alloc_context(buffer, ioBufferBytes, 1, &output, nullptr, &Output::write, &Output::seek);
        if (!io) { av_free(buffer); return AVERROR(ENOMEM); }
        container->pb = io; container->flags |= AVFMT_FLAG_CUSTOM_IO;
        // Bounded interleaving; a missing audio source is also detected by the
        // caller and cannot accumulate an unbounded muxer packet queue.
        container->max_interleave_delta = 2'000'000;
        if ((status = avformat_write_header(container, nullptr)) < 0) return status;
        header = true;
        return io->error < 0 ? io->error : 0;
    }
    bool accepts(const Core::FrameLease& frame, PixelRegion region) const noexcept
    {
        if (!frame) return false;
        const auto& format = frame.format();
        return format.width == region.width && format.height == region.height &&
            (format.pixelFormat == Core::FramePixelFormat::Bgra8 || format.pixelFormat == Core::FramePixelFormat::Rgba8) &&
            format.alpha == Core::FrameAlpha::Opaque && !frame.metadata().crop &&
            frame.metadata().transform == Core::FrameTransform::None && !frame.metadata().timing.discontinuity &&
            format.color.transfer != Core::ColorTransfer::Pq && format.color.transfer != Core::ColorTransfer::Hlg &&
            format.color.range != Core::ColorRange::Limited &&
            (format.color.matrix == Core::ColorMatrix::Rgb || format.color.matrix == Core::ColorMatrix::Unspecified) &&
            format.planes[0].stride >= (std::numeric_limits<int>::min)() &&
            format.planes[0].stride <= (std::numeric_limits<int>::max)();
    }
    int convert(const Core::FrameLease& frame, PixelRegion region, std::int64_t pts)
    {
        if (!accepts(frame, region) || frame.format() != sourceFormat) return AVERROR(EINVAL);
        int status = av_frame_make_writable(videoFrame);
        if (status < 0) return status;
        const auto row = frame.row(0, 0);
        const std::uint8_t* planes[]{row.data(), nullptr, nullptr, nullptr};
        const int strides[]{static_cast<int>(frame.format().planes[0].stride), 0, 0, 0};
        status = sws_scale(converter, planes, strides, 0, video->height, videoFrame->data, videoFrame->linesize);
        if (status != video->height) return status < 0 ? status : AVERROR(EINVAL);
        videoFrame->pts = pts;
        return 0;
    }
    int encode(AVCodecContext* codec, AVStream* stream, AVFrame* frame, bool& accepted)
    {
        accepted = false;
        int status = avcodec_send_frame(codec, frame);
        if (status < 0) return status;
        accepted = true;
        for (;;) {
            status = avcodec_receive_packet(codec, packet);
            if (status == AVERROR_EOF) return frame ? AVERROR(EINVAL) : 0;
            if (status == AVERROR(EAGAIN)) return frame ? 0 : status;
            if (status < 0) return status;
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            status = av_interleaved_write_frame(container, packet);
            av_packet_unref(packet);
            if (status < 0) return status;
            if (io->error < 0) return io->error;
        }
    }
};

std::string codecError(const char* context, std::int64_t code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(static_cast<int>(code), message.data(), message.size());
    return std::string(context) + ": " + message.data();
}
}

BackendResult runSoftwareMp4Recording(const BackendRequest& request, Platform::FileHandle writer,
    std::unique_ptr<FrameSource> video, std::unique_ptr<AudioSource> audio,
    const BackendEvents& events, std::stop_token stop)
{
    BackendResult result;
    Output output{writer.get()};
    Encoder encoder;
    bool pendingVideo{}, audioFailed{};
    std::int64_t audioSamples{};
    auto lastAudioProgress = Clock::now();
    const auto fail = [&](RecordingStage stage, std::int64_t code, std::string detail) {
        if (!result.media.firstError) result.detail = std::move(detail);
        result.media.fail(stage, code ? code : EIO);
    };
    const auto encodeVideo = [&](std::int64_t end, RecordingStage stage) {
        if (!pendingVideo) return;
        encoder.videoFrame->duration = (std::max)(std::int64_t{1}, end - encoder.videoFrame->pts);
        bool accepted{};
        const auto status = encoder.encode(encoder.video, encoder.videoStream, encoder.videoFrame, accepted);
        if (accepted) ++result.media.framesWritten;
        pendingVideo = false;
        if (status < 0) fail(stage, status, codecError("The H.264 encoder could not write a frame", status));
    };
    const auto encodeAudio = [&](std::int64_t limit, bool final) {
        if (!encoder.audio || audioFailed || !result.prepared) return;
        while (audioSamples < limit) {
            const auto remaining = limit - audioSamples;
            if (!final && remaining < encoder.audio->frame_size) return;
            const auto frames = static_cast<int>((std::min)(remaining, std::int64_t{encoder.audio->frame_size}));
            const auto at = result.prepared->started + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(static_cast<double>(audioSamples) / AudioSource::sampleRate));
            const auto now = Clock::now();
            const auto received = audio->read(at, {encoder.pcm.data(), static_cast<std::size_t>(frames) * AudioSource::channels},
                final ? now + std::chrono::seconds(2) : now, final ? std::stop_token{} : stop);
            if (received.cancelled) return;
            if (!received) {
                // Device latency need not block video capture. Available PCM
                // catches up on later frames; a two-second stall is terminal.
                if (!final && received.error == ETIMEDOUT && now - lastAudioProgress < std::chrono::seconds(2)) return;
                audioFailed = true;
                fail(RecordingStage::Capture, received.error, received.detail); return;
            }
            auto status = av_frame_make_writable(encoder.audioFrame);
            if (status < 0) { audioFailed = true; fail(RecordingStage::Encode, status, codecError("Cannot reuse the AAC frame", status)); return; }
            encoder.audioFrame->nb_samples = frames; encoder.audioFrame->pts = audioSamples;
            for (int channel = 0; channel < 2; ++channel) {
                auto* target = reinterpret_cast<float*>(encoder.audioFrame->data[channel]);
                for (int index = 0; index < frames; ++index) target[index] = encoder.pcm[static_cast<std::size_t>(index) * 2 + channel];
            }
            bool accepted{};
            status = encoder.encode(encoder.audio, encoder.audioStream, encoder.audioFrame, accepted);
            if (accepted) audioSamples += frames;
            if (status < 0) { audioFailed = true; fail(RecordingStage::Encode, status, codecError("The AAC encoder could not write a frame", status)); return; }
            lastAudioProgress = Clock::now();
        }
    };
    try {
        const auto capture = [&] {
            const auto aligned = alignedMp4Region(request.region, request.maximumFrameBytes);
            if (request.format != Format::Mp4 || !writer || !video || !aligned || *aligned != request.region ||
                !request.framesPerSecond || request.framesPerSecond > 60 || request.maximumDuration.count() <= 0 ||
                request.maximumDuration > std::chrono::minutes(120) || request.firstFrameTimeout.count() <= 0 ||
                request.firstFrameTimeout > std::chrono::minutes(1) ||
                static_cast<bool>(audio) != (request.systemAudio || request.microphone)) {
                fail(RecordingStage::Prepare, EINVAL, "Invalid software MP4 recording parameters"); return;
            }
            if (stop.stop_requested()) return;
            std::error_code identityError;
            const auto identity = Platform::readFileDescriptorSnapshot(writer.get(), identityError);
            if (!identity || identity->bytes) { fail(RecordingStage::Prepare, EINVAL, "MP4 output is not an owned empty ordinary file"); return; }
            Core::FrameLease latest;
            const auto firstDeadline = Clock::now() + request.firstFrameTimeout;
            std::mutex mutex; std::condition_variable_any wait;
            while (!latest && !stop.stop_requested()) {
                auto received = video->read(firstDeadline, stop);
                if (stop.stop_requested()) return;
                if (received.status == FrameReadStatus::Stopped) { fail(RecordingStage::Capture, EPIPE, "The recording source stopped unexpectedly"); return; }
                if (received.status == FrameReadStatus::Failed) { fail(RecordingStage::Capture, received.error, received.detail); return; }
                if (received.status == FrameReadStatus::Frame) latest = std::move(received.frame);
                if (!latest) {
                    if (Clock::now() >= firstDeadline) { fail(RecordingStage::Capture, ETIMEDOUT, "Timed out waiting for the first recording frame"); return; }
                    std::unique_lock lock(mutex);
                    wait.wait_until(lock, stop, (std::min)(firstDeadline, Clock::now() + std::chrono::milliseconds(5)), [] { return false; });
                }
            }
            if (stop.stop_requested()) return;
            if (!encoder.accepts(latest, request.region)) { fail(RecordingStage::Prepare, EINVAL, "The screen frame geometry, color or transform is unsupported"); return; }
            int status = encoder.open(request, latest.format(), output);
            if (status < 0) { fail(RecordingStage::Prepare, status, codecError("Cannot prepare the software H.264/AAC encoder", status)); return; }
            if (audio) {
                const auto ready = audio->prepare(Clock::now() + request.firstFrameTimeout, stop);
                if (ready.cancelled) return;
                if (!ready) { fail(RecordingStage::Prepare, ready.error, ready.detail); return; }
            }
            if (stop.stop_requested()) return;
            result.prepared = PreparedRecording{request.region, Codec::H264, video->kind(), Clock::now(), request.systemAudio, request.microphone};
            if (events.prepared) events.prepared(*result.prepared);
            lastAudioProgress = result.prepared->started;
            if ((status = encoder.convert(latest, request.region, 0)) < 0) { fail(RecordingStage::Capture, status, "Cannot convert the first recording frame"); return; }
            pendingVideo = true;
            const auto interval = std::chrono::nanoseconds(1'000'000'000 / request.framesPerSecond);
            const auto stopAt = result.prepared->started + request.maximumDuration;
            auto nextFrame = result.prepared->started + interval;
            while (!stop.stop_requested() && !result.media.firstError) {
                { std::unique_lock lock(mutex); wait.wait_until(lock, stop, (std::min)(nextFrame, stopAt), [] { return false; }); }
                if (stop.stop_requested()) break;
                if (Clock::now() >= stopAt) { result.durationLimitReached = true; break; }
                auto received = video->read(nextFrame, stop);
                if (stop.stop_requested()) break;
                if (received.status == FrameReadStatus::Stopped) { fail(RecordingStage::Capture, EPIPE, "The recording source stopped unexpectedly"); break; }
                if (received.status == FrameReadStatus::Failed) { fail(RecordingStage::Capture, received.error, received.detail); break; }
                if (received.status == FrameReadStatus::Frame) {
                    if (!received.frame) { fail(RecordingStage::Capture, EINVAL, "The screen source returned no pixels"); break; }
                    latest = std::move(received.frame);
                }
                const auto capturedAt = (std::min)(Clock::now(), stopAt);
                const auto pts = std::chrono::duration_cast<std::chrono::microseconds>(capturedAt - result.prepared->started).count();
                encodeVideo(pts, RecordingStage::Encode);
                if (result.media.firstError) break;
                if ((status = encoder.convert(latest, request.region, pts)) < 0) {
                    fail(RecordingStage::Capture, status, "The recording source changed geometry, layout or color"); break;
                }
                pendingVideo = true;
                encodeAudio(av_rescale_q(pts, videoTimeBase, audioTimeBase), false);
                nextFrame += interval;
                const auto after = Clock::now();
                if (nextFrame < after) nextFrame += interval * ((after - nextFrame) / interval + 1);
            }
        };
        capture();
        if (result.prepared) {
            result.elapsed = std::chrono::duration_cast<Core::MediaTime>((std::min)(Clock::now() - result.prepared->started,
                std::chrono::duration_cast<Clock::duration>(request.maximumDuration)));
            const auto end = (std::max)(std::int64_t{1}, std::chrono::duration_cast<std::chrono::microseconds>(result.elapsed).count());
            encodeVideo(end, RecordingStage::Encode);
            encodeAudio(av_rescale_q(end, videoTimeBase, audioTimeBase), true);
        }
    } catch (const std::bad_alloc&) { fail(RecordingStage::Encode, ENOMEM, "The MP4 recording exceeded available memory"); }
    catch (...) { fail(RecordingStage::Encode, EIO, "The MP4 recording worker failed"); }

    result.stopRequested = stop.stop_requested();
    // Native producers are stopped on this worker before codec finalization.
    // User stop never aborts codec draining or publication of already captured data.
    if (video) video->stopAndWait();
    video.reset(); audio.reset();
    if (encoder.header) {
        if (events.finalizing) {
            try { events.finalizing(); }
            catch (...) { fail(RecordingStage::Finalize, EIO, "The finalization callback failed"); }
        }
        bool accepted{};
        if (encoder.videoOpened) {
            const auto status = encoder.encode(encoder.video, encoder.videoStream, nullptr, accepted);
            if (status < 0 && (result.media.framesWritten || !result.stopRequested))
                fail(RecordingStage::Finalize, status, codecError("The H.264 encoder did not drain to EOF", status));
        }
        if (encoder.audioOpened) {
            const auto status = encoder.encode(encoder.audio, encoder.audioStream, nullptr, accepted);
            if (status < 0 && (audioSamples || !result.stopRequested))
                fail(RecordingStage::Finalize, status, codecError("The AAC encoder did not drain to EOF", status));
        }
        const auto status = av_write_trailer(encoder.container);
        if (status < 0 && (result.media.framesWritten || !result.stopRequested))
            fail(RecordingStage::Finalize, status, codecError("The MP4 trailer could not be written", status));
        avio_flush(encoder.io);
        if (encoder.io->error < 0) fail(RecordingStage::Finalize, encoder.io->error, "The MP4 output stream failed while flushing");
        result.media.finish(!result.media.finalizationError);
    }
    if (output.error) fail(RecordingStage::Finalize, output.error, "The MP4 output file could not be written");
    std::error_code closeError;
    if (!writer.close(closeError)) fail(RecordingStage::Finalize, closeError.value(), "The MP4 output file could not be closed");
    if (result.media.finalizationError) result.media.finalized = false;
    return result;
}

} // namespace HuhuCapture::Recording
