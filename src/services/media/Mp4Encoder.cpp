/*
 * Encoder send/receive, stream setup and drain adapted from FFmpeg mux.c.
 * Copyright (c) 2003 Fabrice Bellard
 * Custom AVIO ownership follows avio_read_callback.c.
 * Copyright (c) 2014 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "services/media/Mp4Encoder.h"
#include "core/PixelBuffer.h"
#include "platform/FileOutput.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <limits>
#include <utility>

namespace HuhuCapture::Media {
namespace {
using Core::RecordingStage;
constexpr AVRational mediaTimeBase{1, 10'000'000};
constexpr auto maximumTimeline = Core::mediaTicks(std::chrono::minutes(120));

bool opaqueSdr(const Core::FrameFormat& format) noexcept
{
    const auto& color = format.color;
    return format.alpha == Core::FrameAlpha::Opaque &&
        (format.pixelFormat == Core::FramePixelFormat::Bgra8 || format.pixelFormat == Core::FramePixelFormat::Rgba8) &&
        (color.primaries == Core::ColorPrimaries::Unspecified || color.primaries == Core::ColorPrimaries::Bt709) &&
        (color.transfer == Core::ColorTransfer::Unspecified || color.transfer == Core::ColorTransfer::Srgb ||
            color.transfer == Core::ColorTransfer::Bt709) &&
        (color.matrix == Core::ColorMatrix::Unspecified || color.matrix == Core::ColorMatrix::Rgb) &&
        (color.range == Core::ColorRange::Unspecified || color.range == Core::ColorRange::Full);
}
AVColorTransferCharacteristic transfer(const Core::FrameFormat& format) noexcept
{
    // Unspecified desktop RGB follows the existing unmanaged sRGB contract.
    // swscale changes matrix/range; it does not silently perform HDR tone mapping
    // or change the source transfer function.
    return format.color.transfer == Core::ColorTransfer::Bt709 ? AVCOL_TRC_BT709 : AVCOL_TRC_IEC61966_2_1;
}
}

struct Mp4Encoder::State {
    Mp4EncoderOptions options;
    Platform::FileHandle file;
    Core::RecordingResult result;
    Mp4EncoderStatistics statistics;
    AVFormatContext* container{};
    AVIOContext* io{};
    AVCodecContext* video{};
    AVCodecContext* audio{};
    AVStream* videoStream{};
    AVStream* audioStream{};
    AVFrame* videoFrame{};
    AVFrame* audioFrame{};
    AVPacket* packet{};
    SwsContext* scaler{};
    SwrContext* resampler{};
    AVAudioFifo* fifo{};
    std::uint64_t position{}, extent{}, audioEncodedSamples{};
    Core::MediaTime lastVideoTime{};
    int firstIoError{};
    bool opened{}, finished{};

    State(Mp4EncoderOptions values, Platform::FileHandle writer)
        : options(values), file(std::move(writer))
    {
        static_assert(LIBAVCODEC_VERSION_INT == AV_VERSION_INT(63, 1, 102));
        static_assert(LIBAVFORMAT_VERSION_INT == AV_VERSION_INT(63, 1, 102));
        const auto pixels = Core::pixelLayout(options.width, options.height, 4);
        std::error_code error;
        const auto snapshot = file ? Platform::readFileDescriptorSnapshot(file.get(), error) : std::nullopt;
        if (!pixels || options.width > 16384 || options.height > 16384 || options.width % 2 || options.height % 2 ||
            pixels->bytes > options.maximumFrameBytes || !options.framesPerSecond || options.framesPerSecond > 240 ||
            !options.maximumOutputBytes || options.maximumOutputBytes > 32ull * 1024 * 1024 * 1024 ||
            !options.maximumQueuedAudioBytes || options.maximumQueuedAudioBytes > 64ull * 1024 * 1024 ||
            !snapshot || snapshot->bytes || (options.audio &&
                ((options.audio->sampleRate != 44100 && options.audio->sampleRate != 48000) ||
                 (options.audio->channels != 1 && options.audio->channels != 2)))) {
            fail(RecordingStage::Prepare, error ? -error.value() : AVERROR(EINVAL)); return;
        }
        if (avcodec_version() != LIBAVCODEC_VERSION_INT || avformat_version() != LIBAVFORMAT_VERSION_INT ||
            avutil_version() != LIBAVUTIL_VERSION_INT || swscale_version() != LIBSWSCALE_VERSION_INT ||
            swresample_version() != LIBSWRESAMPLE_VERSION_INT) {
            fail(RecordingStage::Prepare, AVERROR(ENOSYS)); return;
        }
        if (const auto seek = Platform::seekFileDescriptor(file.get(), 0)) fail(RecordingStage::Prepare, -seek.value());
    }
    ~State()
    {
        finish();
        sws_freeContext(scaler);
        swr_free(&resampler);
        av_audio_fifo_free(fifo);
        av_frame_free(&videoFrame);
        av_frame_free(&audioFrame);
        av_packet_free(&packet);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        if (container) container->pb = nullptr;
        avformat_free_context(container);
        // FFmpeg may replace its AVIO buffer. Free its current buffer, not the
        // original allocation saved at construction; context_free does not.
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }

    bool fail(RecordingStage stage, int code) noexcept
    { result.fail(stage, code < 0 ? code : AVERROR(EIO)); return false; }
    bool checked(int code, RecordingStage stage) noexcept
    { return code >= 0 || fail(stage, code); }
    bool ready() const noexcept { return !finished && !result.firstError && static_cast<bool>(file); }
    int ioFailure(int code) noexcept
    {
        if (!firstIoError) firstIoError = code < 0 ? code : AVERROR(EIO);
        return firstIoError;
    }
    static int writeBytes(void* opaque, const std::uint8_t* bytes, int count) noexcept
    {
        auto& self = *static_cast<State*>(opaque);
        if (self.firstIoError) return self.firstIoError;
        if (count < 0 || (!bytes && count)) return self.ioFailure(AVERROR(EINVAL));
        const auto size = static_cast<std::uint64_t>(count);
        if (self.position > self.options.maximumOutputBytes || size > self.options.maximumOutputBytes - self.position)
            return self.ioFailure(AVERROR(EFBIG));
        const auto error = Platform::writeFileDescriptor(self.file.get(), {bytes, static_cast<std::size_t>(count)});
        if (error) return self.ioFailure(-error.value());
        self.position += size;
        self.extent = (std::max)(self.extent, self.position);
        return count;
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int origin) noexcept
    {
        auto& self = *static_cast<State*>(opaque);
        if (self.firstIoError) return self.firstIoError;
        if (origin == AVSEEK_SIZE) return static_cast<std::int64_t>(self.extent);
        origin &= ~AVSEEK_FORCE;
        std::int64_t base{};
        if (origin == SEEK_CUR) base = static_cast<std::int64_t>(self.position);
        else if (origin == SEEK_END) base = static_cast<std::int64_t>(self.extent);
        else if (origin != SEEK_SET) return self.ioFailure(AVERROR(EINVAL));
        if (offset < -base || offset > static_cast<std::int64_t>(self.options.maximumOutputBytes) - base)
            return self.ioFailure(AVERROR(EFBIG));
        const auto target = static_cast<std::uint64_t>(base + offset);
        if (const auto error = Platform::seekFileDescriptor(self.file.get(), target)) return self.ioFailure(-error.value());
        self.position = target;
        return static_cast<std::int64_t>(target);
    }

    bool openAudio() noexcept
    {
        if (!options.audio) return true;
        const auto* codec = avcodec_find_encoder_by_name("aac");
        if (!codec || !(codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) return fail(RecordingStage::Prepare, AVERROR_ENCODER_NOT_FOUND);
        audio = avcodec_alloc_context3(codec);
        audioStream = avformat_new_stream(container, nullptr);
        if (!audio || !audioStream) return fail(RecordingStage::Prepare, AVERROR(ENOMEM));
        audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio->sample_rate = options.audio->sampleRate;
        audio->time_base = AVRational{1, audio->sample_rate};
        audio->bit_rate = 192000;
        av_channel_layout_default(&audio->ch_layout, options.audio->channels);
        audioStream->time_base = audio->time_base;
        if (container->oformat->flags & AVFMT_GLOBALHEADER) audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (!checked(avcodec_open2(audio, codec, nullptr), RecordingStage::Prepare) ||
            !checked(avcodec_parameters_from_context(audioStream->codecpar, audio), RecordingStage::Prepare)) return false;
        if (audio->frame_size <= 0 || static_cast<std::uint64_t>(audio->frame_size) * options.audio->channels * sizeof(float) >
            options.maximumQueuedAudioBytes) return fail(RecordingStage::Prepare, AVERROR(ENOBUFS));
        audioFrame = av_frame_alloc();
        if (!audioFrame) return fail(RecordingStage::Prepare, AVERROR(ENOMEM));
        audioFrame->format = audio->sample_fmt;
        audioFrame->sample_rate = audio->sample_rate;
        audioFrame->nb_samples = audio->frame_size;
        if (!checked(av_channel_layout_copy(&audioFrame->ch_layout, &audio->ch_layout), RecordingStage::Prepare) ||
            !checked(av_frame_get_buffer(audioFrame, 0), RecordingStage::Prepare)) return false;
        fifo = av_audio_fifo_alloc(audio->sample_fmt, audio->ch_layout.nb_channels, audio->frame_size);
        if (!fifo) return fail(RecordingStage::Prepare, AVERROR(ENOMEM));
        if (!checked(swr_alloc_set_opts2(&resampler, &audio->ch_layout, audio->sample_fmt, audio->sample_rate,
                &audio->ch_layout, AV_SAMPLE_FMT_FLT, audio->sample_rate, 0, nullptr), RecordingStage::Prepare) ||
            !checked(swr_init(resampler), RecordingStage::Prepare)) return false;
        return true;
    }

    bool open(const Core::FrameFormat& format) noexcept
    {
        if (opened) return true;
        const auto* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) return fail(RecordingStage::Prepare, AVERROR_ENCODER_NOT_FOUND);
        if (!checked(avformat_alloc_output_context2(&container, nullptr, "mp4", nullptr), RecordingStage::Prepare)) return false;
        video = avcodec_alloc_context3(codec);
        videoStream = avformat_new_stream(container, nullptr);
        videoFrame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!video || !videoStream || !videoFrame || !packet) return fail(RecordingStage::Prepare, AVERROR(ENOMEM));
        video->width = options.width; video->height = options.height;
        video->pix_fmt = AV_PIX_FMT_YUV420P;
        video->time_base = mediaTimeBase;
        video->framerate = AVRational{static_cast<int>(options.framesPerSecond), 1};
        video->sample_aspect_ratio = AVRational{1, 1};
        video->gop_size = static_cast<int>(options.framesPerSecond * 2);
        video->max_b_frames = 2;
        video->thread_count = 2;
        video->color_primaries = AVCOL_PRI_BT709;
        video->color_trc = transfer(format);
        video->colorspace = AVCOL_SPC_BT709;
        video->color_range = AVCOL_RANGE_MPEG;
        video->flags |= AV_CODEC_FLAG_FRAME_DURATION;
        if (container->oformat->flags & AVFMT_GLOBALHEADER) video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        // Actual fixed libx264 options, not a made-up quality abstraction. The
        // production performance/size comparison remains a separate gate.
        if (!checked(av_opt_set(video->priv_data, "preset", "veryfast", 0), RecordingStage::Prepare) ||
            !checked(av_opt_set_double(video->priv_data, "crf", 18, 0), RecordingStage::Prepare) ||
            !checked(av_opt_set_int(video->priv_data, "rc-lookahead", 10, 0), RecordingStage::Prepare) ||
            !checked(avcodec_open2(video, codec, nullptr), RecordingStage::Prepare)) return false;
        videoStream->time_base = mediaTimeBase;
        videoStream->avg_frame_rate = video->framerate;
        if (!checked(avcodec_parameters_from_context(videoStream->codecpar, video), RecordingStage::Prepare)) return false;
        videoFrame->format = video->pix_fmt;
        videoFrame->width = video->width; videoFrame->height = video->height;
        videoFrame->color_primaries = video->color_primaries;
        videoFrame->color_trc = video->color_trc;
        videoFrame->colorspace = video->colorspace;
        videoFrame->color_range = video->color_range;
        if (!checked(av_frame_get_buffer(videoFrame, 32), RecordingStage::Prepare) || !openAudio()) return false;
        constexpr int bufferSize = 32768;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(bufferSize));
        if (!buffer) return fail(RecordingStage::Prepare, AVERROR(ENOMEM));
        io = avio_alloc_context(buffer, bufferSize, 1, this, nullptr, &writeBytes, &seek);
        if (!io) { av_free(buffer); return fail(RecordingStage::Prepare, AVERROR(ENOMEM)); }
        container->pb = io;
        container->flags |= AVFMT_FLAG_CUSTOM_IO;
        // movenc's automatic gcd for 100 ns video and 44.1 kHz audio exceeds
        // INT_MAX and falls back to a 1 kHz movie clock. Rounding the AAC priming
        // edit then trims real tail samples. Use the actual audio sample clock
        // for movie edits; track timestamps keep their original precision.
        if (!checked(av_opt_set_int(container->priv_data, "movie_timescale",
                audio ? audio->sample_rate : mediaTimeBase.den, 0), RecordingStage::Prepare)) return false;
        // A stalled companion stream cannot grow the muxer's interleaving queue
        // indefinitely. This threshold flushes packets; it never inserts silence.
        container->max_interleave_delta = AV_TIME_BASE;
        if (!checked(avformat_write_header(container, nullptr), RecordingStage::Prepare)) return false;
        opened = true;
        return true;
    }

    bool receive(AVCodecContext* codec, AVStream* stream, bool draining, std::uint64_t& count,
        RecordingStage stage) noexcept
    {
        for (;;) {
            const auto status = avcodec_receive_packet(codec, packet);
            if (status == AVERROR_EOF) return true;
            if (status == AVERROR(EAGAIN)) return !draining || fail(stage, AVERROR_BUG);
            if (!checked(status, stage)) return false;
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const auto written = av_interleaved_write_frame(container, packet);
            av_packet_unref(packet);
            if (!checked(written, stage)) return false;
            ++count;
        }
    }
    bool submit(AVCodecContext* codec, AVStream* stream, AVFrame* frame, std::uint64_t& packets,
        RecordingStage stage) noexcept
    {
        auto status = avcodec_send_frame(codec, frame);
        if (status == AVERROR(EAGAIN)) {
            const auto before = packets;
            if (!receive(codec, stream, false, packets, stage)) return false;
            if (before == packets) return fail(stage, AVERROR_BUG);
            status = avcodec_send_frame(codec, frame);
        }
        if (status == AVERROR_EOF && !frame) return true;
        if (!checked(status, stage)) return false;
        if (frame && codec == video) ++result.framesWritten;
        return receive(codec, stream, frame == nullptr, packets, stage);
    }

    bool checkTime(Core::MediaTime time, Core::MediaTime duration) noexcept
    {
        if (!ready()) return false;
        if (time.count() < 0 || duration.count() <= 0 || time.count() > maximumTimeline ||
            duration.count() > maximumTimeline - time.count() ||
            (!result.framesWritten && time.count() != 0) ||
            (result.framesWritten && (time <= lastVideoTime || time < statistics.videoEnd)))
            return fail(RecordingStage::Encode, AVERROR(EINVAL));
        return true;
    }
    bool append(const Core::FrameFormat& format, const std::uint8_t* firstRow, int stride,
        Core::MediaTime time, Core::MediaTime duration) noexcept
    {
        if (!open(format)) return false;
        if (transfer(format) != video->color_trc) return fail(RecordingStage::Encode, AVERROR(ENOSYS));
        if (!checked(av_frame_make_writable(videoFrame), RecordingStage::Encode)) return false;
        scaler = sws_getCachedContext(scaler, options.width, options.height,
            format.pixelFormat == Core::FramePixelFormat::Bgra8 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA,
            options.width, options.height, video->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) return fail(RecordingStage::Encode, AVERROR(ENOMEM));
        const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        if (!checked(sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16),
                RecordingStage::Encode)) return false;
        const std::uint8_t* source[]{firstRow, nullptr, nullptr, nullptr};
        const int strides[]{stride, 0, 0, 0};
        if (sws_scale(scaler, source, strides, 0, options.height, videoFrame->data, videoFrame->linesize) != options.height)
            return fail(RecordingStage::Encode, AVERROR(EIO));
        videoFrame->pts = time.count();
        videoFrame->duration = duration.count();
        if (!submit(video, videoStream, videoFrame, statistics.videoPackets, RecordingStage::Encode)) return false;
        lastVideoTime = time;
        statistics.videoEnd = time + duration;
        return true;
    }

    bool encodeAudio(bool drain, RecordingStage stage) noexcept
    {
        while (av_audio_fifo_size(fifo) >= audio->frame_size || (drain && av_audio_fifo_size(fifo))) {
            const int count = (std::min)(av_audio_fifo_size(fifo), audio->frame_size);
            audioFrame->nb_samples = audio->frame_size;
            if (!checked(av_frame_make_writable(audioFrame), stage)) return false;
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(audioFrame->data), count) != count)
                return fail(stage, AVERROR(EIO));
            audioFrame->nb_samples = count;
            audioFrame->pts = static_cast<std::int64_t>(audioEncodedSamples);
            audioFrame->duration = count;
            if (!submit(audio, audioStream, audioFrame, statistics.audioPackets, stage)) return false;
            audioEncodedSamples += static_cast<std::uint64_t>(count);
        }
        return true;
    }
    bool addAudio(std::span<const float> interleaved, std::uint64_t firstSample) noexcept
    {
        if (!ready()) return false;
        if (!opened || !audio || interleaved.empty() ||
            interleaved.size() % static_cast<std::size_t>(audio->ch_layout.nb_channels) ||
            firstSample != statistics.audioSamples || interleaved.size_bytes() > options.maximumQueuedAudioBytes)
            return fail(RecordingStage::Encode, AVERROR(EINVAL));
        const auto frames = interleaved.size() / static_cast<std::size_t>(audio->ch_layout.nb_channels);
        const auto maximumSamples = static_cast<std::uint64_t>(audio->sample_rate) * 120 * 60;
        if (frames > INT_MAX || firstSample > maximumSamples || frames > maximumSamples - firstSample)
            return fail(RecordingStage::Encode, AVERROR(EFBIG));
        for (const auto sample : interleaved)
            if (!std::isfinite(sample)) return fail(RecordingStage::Encode, AVERROR(EINVAL));
        // Same-rate sample-format conversion has no resampling delay. Process in
        // codec-sized slices so a caller's block cannot force an unbounded FIFO.
        std::size_t consumed{};
        while (consumed < frames) {
            const auto count = static_cast<int>((std::min)(frames - consumed, static_cast<std::size_t>(audio->frame_size)));
            audioFrame->nb_samples = audio->frame_size;
            if (!checked(av_frame_make_writable(audioFrame), RecordingStage::Encode)) return false;
            const std::uint8_t* input[]{reinterpret_cast<const std::uint8_t*>(interleaved.data() +
                consumed * static_cast<std::size_t>(audio->ch_layout.nb_channels))};
            const int converted = swr_convert(resampler, audioFrame->data, audio->frame_size, input, count);
            if (!checked(converted, RecordingStage::Encode)) return false;
            if (converted != count || swr_get_delay(resampler, audio->sample_rate) != 0)
                return fail(RecordingStage::Encode, AVERROR_BUG);
            const auto queued = static_cast<std::uint64_t>(av_audio_fifo_size(fifo)) + static_cast<unsigned>(converted);
            if (queued > options.maximumQueuedAudioBytes / (sizeof(float) * audio->ch_layout.nb_channels))
                return fail(RecordingStage::Encode, AVERROR(ENOBUFS));
            if (av_audio_fifo_write(fifo, reinterpret_cast<void**>(audioFrame->data), converted) != converted)
                return fail(RecordingStage::Encode, AVERROR(ENOMEM));
            statistics.audioSamples += static_cast<unsigned>(count);
            consumed += static_cast<unsigned>(count);
            if (!encodeAudio(false, RecordingStage::Encode)) return false;
        }
        return true;
    }
    Core::RecordingResult finish() noexcept
    {
        if (finished) return result;
        finished = true;
        bool complete = true;
        if (opened) {
            if (audio) {
                if (!statistics.audioSamples) fail(RecordingStage::Encode, AVERROR(ENODATA));
                complete = encodeAudio(true, RecordingStage::Finalize);
            }
            // Always attempt both real encoder drains and the container trailer
            // after an input failure; successful cleanup never clears firstError.
            complete = submit(video, videoStream, nullptr, statistics.videoPackets, RecordingStage::Finalize) && complete;
            if (audio) complete = submit(audio, audioStream, nullptr, statistics.audioPackets, RecordingStage::Finalize) && complete;
            complete = checked(av_write_trailer(container), RecordingStage::Finalize) && complete;
        }
        if (io) {
            avio_flush(io);
            if (io->error < 0) complete = fail(RecordingStage::Finalize, io->error);
        }
        if (firstIoError) complete = fail(RecordingStage::Finalize, firstIoError);
        if (file) {
            if (const auto error = Platform::flushFileDescriptor(file.get())) complete = fail(RecordingStage::Finalize, -error.value());
            std::error_code error;
            if (!file.close(error)) complete = fail(RecordingStage::Finalize, -error.value());
        }
        result.finish(complete, result.finalizationError ? result.finalizationError->code : 0);
        return result;
    }
};

Mp4Encoder::Mp4Encoder(Mp4EncoderOptions options, Platform::FileHandle writer)
    : state_(std::make_unique<State>(options, std::move(writer))) {}
Mp4Encoder::~Mp4Encoder() = default;
bool Mp4Encoder::ready() const noexcept { return state_->ready(); }
const Core::RecordingResult& Mp4Encoder::result() const noexcept { return state_->result; }
const Mp4EncoderStatistics& Mp4Encoder::statistics() const noexcept { return state_->statistics; }

bool Mp4Encoder::addFrame(const Core::FrameLease& frame, Core::MediaTime time, Core::MediaTime duration) noexcept
{
    auto& state = *state_;
    if (!state.checkTime(time, duration)) return false;
    if (!frame) return state.fail(RecordingStage::Encode, AVERROR(EINVAL));
    const auto& format = frame.format();
    const auto& metadata = frame.metadata();
    if (!Core::validFrameFormat(format) || !Core::validFrameMetadata(metadata, format) ||
        format.width != state.options.width || format.height != state.options.height ||
        format.bytes > state.options.maximumFrameBytes || !opaqueSdr(format) ||
        metadata.transform != Core::FrameTransform::None ||
        (metadata.crop && *metadata.crop != Core::FrameCrop{0, 0, static_cast<std::uint32_t>(format.width),
            static_cast<std::uint32_t>(format.height)}) ||
        format.planes[0].stride < -INT_MAX || format.planes[0].stride > INT_MAX)
        return state.fail(RecordingStage::Encode, AVERROR(ENOSYS));
    const auto row = frame.row(0, 0);
    if (row.size() < static_cast<std::size_t>(format.width) * 4) return state.fail(RecordingStage::Encode, AVERROR(EINVAL));
    return state.append(format, row.data(), static_cast<int>(format.planes[0].stride), time, duration);
}
bool Mp4Encoder::addBgra(std::span<const std::uint8_t> pixels, std::size_t stride,
    Core::MediaTime time, Core::MediaTime duration) noexcept
{
    auto& state = *state_;
    if (!state.checkTime(time, duration)) return false;
    auto format = Core::packedFrameFormat(Core::FramePixelFormat::Bgra8, state.options.width, state.options.height);
    if (!format || pixels.size() > state.options.maximumFrameBytes || stride > INT_MAX)
        return state.fail(RecordingStage::Encode, AVERROR(EINVAL));
    format->bytes = pixels.size();
    format->planes[0] = {0, pixels.size(), 0, static_cast<std::ptrdiff_t>(stride),
        static_cast<std::size_t>(state.options.width) * 4, static_cast<std::uint32_t>(state.options.height)};
    if (!Core::validFrameFormat(*format)) return state.fail(RecordingStage::Encode, AVERROR(EINVAL));
    return state.append(*format, pixels.data(), static_cast<int>(stride), time, duration);
}
bool Mp4Encoder::addAudio(std::span<const float> samples, std::uint64_t firstSample) noexcept
{ return state_->addAudio(samples, firstSample); }
Core::RecordingResult Mp4Encoder::finish() noexcept { return state_->finish(); }

} // namespace HuhuCapture::Media
