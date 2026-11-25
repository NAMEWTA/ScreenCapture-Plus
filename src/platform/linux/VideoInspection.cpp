// Demux / send / receive / drain flow adapted from FFmpeg 9.0.2's
// doc/examples/demux_decode.c (MIT, Stefano Sabatini). The local-file I/O,
// all-track checks and budgets below are application policy; see
// third_party/ffmpeg-examples/ and docs/video-inspection.md for exact sources.
#include "services/media/VideoInspection.h"
#include "services/media/Mp4Envelope.h"
#include "core/PixelBuffer.h"

#include <QFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace HuhuCapture::Media {
namespace {
Failure cancelled() { return {Error::Cancelled, {}}; }
Failure invalid(const char* detail) { return {Error::InvalidMedia, QString::fromLatin1(detail)}; }
Failure budget(const char* detail) { return {Error::Budget, QString::fromLatin1(detail)}; }

Failure codecFailure(int code, const char* operation, Error category = Error::InvalidMedia)
{
    if (code == AVERROR(ENOMEM)) category = Error::Budget;
    if (code == AVERROR_DECODER_NOT_FOUND || code == AVERROR_DEMUXER_NOT_FOUND) category = Error::Unsupported;
    if (code == AVERROR_EXIT) return cancelled();
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return {category, QString::fromLatin1(operation) + QStringLiteral(": ") + QString::fromUtf8(message)};
}

struct PacketDeleter { void operator()(AVPacket* value) const { av_packet_free(&value); } };
struct FrameDeleter { void operator()(AVFrame* value) const { av_frame_free(&value); } };
struct CodecDeleter { void operator()(AVCodecContext* value) const { avcodec_free_context(&value); } };
struct ScaleDeleter { void operator()(SwsContext* value) const { sws_freeContext(value); } };
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Codec = std::unique_ptr<AVCodecContext, CodecDeleter>;

// Supply the already-open local file. Neither an MP4 data reference nor a URL
// in its metadata may open another file or start a network request.
struct Input {
    QFile file;
    std::stop_token stop;
    std::int64_t size{};
    bool readFailed{};
    AVIOContext* io{};
    AVFormatContext* format{};

    ~Input()
    {
        avformat_close_input(&format);
        if (io) {
            // AVIO may replace the caller's original buffer while probing.
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }

    static int read(void* opaque, std::uint8_t* data, int count)
    {
        auto& self = *static_cast<Input*>(opaque);
        if (self.stop.stop_requested()) return AVERROR_EXIT;
        const auto bytes = self.file.read(reinterpret_cast<char*>(data), count);
        if (bytes < 0) { self.readFailed = true; return AVERROR(EIO); }
        return bytes == 0 ? AVERROR_EOF : static_cast<int>(bytes);
    }

    static std::int64_t seek(void* opaque, std::int64_t offset, int whence)
    {
        auto& self = *static_cast<Input*>(opaque);
        if (self.stop.stop_requested()) return AVERROR_EXIT;
        if (whence == AVSEEK_SIZE) return self.size;
        whence &= ~AVSEEK_FORCE;
        std::int64_t base{};
        if (whence == SEEK_CUR) base = self.file.pos();
        else if (whence == SEEK_END) base = self.size;
        else if (whence != SEEK_SET) return AVERROR(EINVAL);
        if (base < 0 || offset < -base || offset > self.size - base) return AVERROR_INVALIDDATA;
        const auto position = base + offset;
        if (!self.file.seek(position)) { self.readFailed = true; return AVERROR(EIO); }
        return position;
    }

    static int interrupted(void* opaque)
    {
        return static_cast<Input*>(opaque)->stop.stop_requested() ? 1 : 0;
    }

    static int denyExternal(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**)
    {
        return AVERROR(EPERM);
    }

    Failure open(const QString& path, const Limits& limits, std::stop_token token)
    {
        stop = token;
        if (stop.stop_requested()) return cancelled();
        if (path.isEmpty() || path.contains(QChar(u'\0'))) return {Error::Read, QStringLiteral("Invalid MP4 path")};
        if (!limits.maximumFileBytes || !limits.maximumImageBytes) return budget("Invalid media budget");
        file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly)) return {Error::Read, file.errorString()};
        size = file.size();
        if (size <= 0) return invalid("MP4 is empty");
        if (static_cast<std::uint64_t>(size) > limits.maximumFileBytes) return budget("Video exceeds the file byte budget");
        const auto headerReader = [&](std::uint64_t offset, std::span<std::uint8_t> bytes) {
            return file.seek(static_cast<qint64>(offset)) &&
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size());
        };
        if (auto error = Detail::checkMp4Envelope(static_cast<std::uint64_t>(size), headerReader, limits, stop)) return error;
        if (!file.seek(0)) return {Error::Read, file.errorString()};
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(32768));
        if (!buffer) return budget("Cannot allocate local-file I/O buffer");
        io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
        if (!io) { av_free(buffer); return budget("Cannot allocate local-file I/O state"); }
        format = avformat_alloc_context();
        if (!format) return budget("Cannot allocate MP4 demuxer");
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->interrupt_callback = {interrupted, this};
        format->io_open = denyExternal;
        format->max_streams = 16;
        format->probesize = 1024 * 1024;
        format->max_probe_packets = 64;
        format->max_analyze_duration = AV_TIME_BASE;
        format->error_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
        AVDictionary* options{};
        av_dict_set(&options, "enable_drefs", "0", 0);
        av_dict_set(&options, "use_absolute_path", "0", 0);
        const auto* mov = av_find_input_format("mov");
        const int result = mov ? avformat_open_input(&format, nullptr, mov, &options) : AVERROR_DEMUXER_NOT_FOUND;
        av_dict_free(&options);
        if (stop.stop_requested()) return cancelled();
        if (result < 0) return codecFailure(result, "Read MP4 header", readFailed ? Error::Read : Error::InvalidMedia);
        if (!format->nb_streams || format->nb_streams > 16) return budget("Invalid or excessive MP4 stream count");
        // MOV supplies codec parameters from the sample descriptions. Avoid
        // avformat_find_stream_info's implicit probe decoders, which would run
        // before the application has installed its decoded-pixel limits.
        return {};
    }
};

struct Track {
    AVStream* stream{};
    Codec codec;
    std::uint64_t packets{};
    std::uint64_t frames{};
    int checkedEntries{};
    bool video{};
};

Failure checkIndex(Input& input, Track& track, const Limits& limits)
{
    const auto count = avformat_index_get_entries_count(track.stream);
    if (count < track.checkedEntries) return invalid("MP4 sample index changed after inspection began");
    if (static_cast<std::uint64_t>(count) > limits.maximumImageBytes / sizeof(AVIndexEntry))
        return budget("MP4 sample index exceeds the metadata budget");
    // Check every indexed byte range, including packets not yet read. A demuxer
    // reporting EOF is not evidence that a truncated final sample was present.
    for (int i = track.checkedEntries; i < count; ++i) {
        if ((i & 1023) == 0 && input.stop.stop_requested()) return cancelled();
        const auto* entry = avformat_index_get_entry(track.stream, i);
        if (!entry || entry->pos < 0 || entry->size <= 0 || entry->pos > input.size ||
            entry->size > input.size - entry->pos)
            return invalid("Declared MP4 sample extends outside the physical file");
    }
    track.checkedEntries = count;
    return {};
}

Failure videoLayout(int width, int height, const Limits& limits)
{
    const auto layout = Core::pixelLayout(width, height, 4);
    if (!layout || layout->bytes > limits.maximumImageBytes) return budget("Video exceeds the decoded-pixel budget");
    return {};
}

Failure openTracks(Input& input, const Limits& limits, std::vector<Track>& tracks, bool coverOnly)
{
    bool foundVideo = false;
    std::uint64_t simultaneousPixels{};
    for (unsigned index = 0; index < input.format->nb_streams; ++index) {
        if (input.stop.stop_requested()) return cancelled();
        auto* stream = input.format->streams[index];
        const auto* parameters = stream->codecpar;
        if (parameters->codec_type != AVMEDIA_TYPE_VIDEO && parameters->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const bool video = parameters->codec_type == AVMEDIA_TYPE_VIDEO;
        if (video) {
            if (auto error = videoLayout(parameters->width, parameters->height, limits)) return error;
            const auto bytes = Core::pixelLayout(parameters->width, parameters->height, 4)->bytes;
            if (bytes > limits.maximumImageBytes - simultaneousPixels) return budget("Video tracks exceed the combined pixel budget");
            simultaneousPixels += bytes;
        }
        if (coverOnly && (!video || foundVideo)) continue;
        if (video) foundVideo = true;
        const auto* decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder) return {Error::Unsupported, QStringLiteral("No installed decoder for a declared MP4 audio/video track")};
        Track track;
        track.stream = stream;
        track.video = video;
        track.codec.reset(avcodec_alloc_context3(decoder));
        if (!track.codec) return budget("Cannot allocate the track decoder");
        int code = avcodec_parameters_to_context(track.codec.get(), parameters);
        if (code < 0) return codecFailure(code, "Copy declared decoder parameters");
        track.codec->thread_count = 1;
        track.codec->pkt_timebase = stream->time_base;
        track.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
        track.codec->error_concealment = 0;
        track.codec->max_pixels = static_cast<std::int64_t>(std::min<std::uint64_t>(
            limits.maximumImageBytes / 4, std::numeric_limits<std::int64_t>::max()));
        track.codec->max_samples = static_cast<std::int64_t>(std::min<std::uint64_t>(
            limits.maximumImageBytes / sizeof(double), std::numeric_limits<std::int64_t>::max()));
        if (!video && (parameters->ch_layout.nb_channels <= 0 || parameters->ch_layout.nb_channels > 32 || parameters->sample_rate <= 0))
            return invalid("MP4 declares an invalid audio channel or sample-rate layout");
        if ((code = avcodec_open2(track.codec.get(), decoder, nullptr)) < 0)
            return codecFailure(code, "Open the declared audio/video decoder");
        if (!coverOnly) if (auto error = checkIndex(input, track, limits)) return error;
        tracks.push_back(std::move(track));
    }
    return foundVideo ? Failure{} : invalid("MP4 contains no declared video track");
}

Failure checkAudio(const AVFrame& frame, const Limits& limits, std::stop_token stop)
{
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const int bytes = av_get_bytes_per_sample(format);
    const int channels = frame.ch_layout.nb_channels;
    if (bytes <= 0 || channels <= 0 || channels > 32 || frame.nb_samples <= 0 || frame.sample_rate <= 0)
        return invalid("Decoder produced an invalid audio sample layout");
    const auto total = static_cast<std::uint64_t>(frame.nb_samples) * channels * bytes;
    if (total > limits.maximumImageBytes) return budget("Decoded audio exceeds the sample byte budget");
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int planes = planar ? channels : 1;
    const auto samples = static_cast<std::uint64_t>(frame.nb_samples) * (planar ? 1 : channels);
    const auto packed = av_get_packed_sample_fmt(format);
    if (!frame.extended_data) return invalid("Decoded audio has no plane pointers");
    for (int plane = 0; plane < planes; ++plane) {
        const auto* pointer = frame.extended_data[plane];
        const auto* allocation = av_frame_get_plane_buffer(&frame, plane);
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto base = allocation ? reinterpret_cast<std::uintptr_t>(allocation->data) : 0;
        const auto length = samples * bytes;
        if (!allocation || !pointer || address < base || address - base > allocation->size ||
            length > allocation->size - (address - base))
            return invalid("Decoded audio plane exceeds its owning buffer");
        for (std::uint64_t sample = 0; sample < samples; ++sample) {
            if ((sample & 4095) == 0 && stop.stop_requested()) return cancelled();
            if (packed == AV_SAMPLE_FMT_FLT) {
                float value;
                std::memcpy(&value, pointer + sample * bytes, sizeof(value));
                if (!std::isfinite(value)) return invalid("Decoded audio contains a nonfinite sample");
            } else if (packed == AV_SAMPLE_FMT_DBL) {
                double value;
                std::memcpy(&value, pointer + sample * bytes, sizeof(value));
                if (!std::isfinite(value)) return invalid("Decoded audio contains a nonfinite sample");
            }
        }
    }
    return {};
}

Failure checkFrame(Track& track, const AVFrame& frame, Inspection& result, const Limits& limits, std::stop_token stop)
{
    if ((frame.flags & AV_FRAME_FLAG_CORRUPT) || frame.decode_error_flags)
        return invalid("Decoder reported a corrupt or concealed media frame");
    if (track.video) {
        if (auto error = videoLayout(frame.width, frame.height, limits)) return error;
        const auto pixelFormat = static_cast<AVPixelFormat>(frame.format);
        const auto* description = av_pix_fmt_desc_get(pixelFormat);
        const auto size = av_image_get_buffer_size(pixelFormat, frame.width, frame.height, 1);
        if (!description || (description->flags & AV_PIX_FMT_FLAG_HWACCEL) || size <= 0)
            return invalid("Decoder did not produce a valid CPU video layout");
        if (static_cast<std::uint64_t>(size) > limits.maximumImageBytes)
            return budget("Decoded video planes exceed the byte budget");
        if (result.dimensions.isEmpty()) result.dimensions = QSize(frame.width, frame.height);
        if (result.frames == std::numeric_limits<std::uint64_t>::max()) return budget("Decoded video frame count overflows");
        ++result.frames;
    } else if (auto error = checkAudio(frame, limits, stop)) return error;
    if (track.frames == std::numeric_limits<std::uint64_t>::max()) return budget("Decoded track frame count overflows");
    ++track.frames;
    return {};
}

VideoThumbnailResult makeCover(const AVFrame& frame, unsigned edge, const Limits& limits, std::stop_token stop)
{
    if (stop.stop_requested()) return {cancelled(), {}};
    if (auto error = videoLayout(frame.width, frame.height, limits)) return {std::move(error), {}};
    if (frame.color_trc == AVCOL_TRC_SMPTE2084 || frame.color_trc == AVCOL_TRC_ARIB_STD_B67)
        return {{Error::Unsupported, QStringLiteral("HDR thumbnail tone mapping has not been validated")}, {}};
    const QSize original(frame.width, frame.height);
    const auto dimensions = original.width() <= static_cast<int>(edge) && original.height() <= static_cast<int>(edge) ? original :
        original.scaled(static_cast<int>(edge), static_cast<int>(edge), Qt::KeepAspectRatio);
    QImage image(dimensions, QImage::Format_RGB32);
    if (image.isNull()) return {budget("Cannot allocate the video cover"), {}};
    std::unique_ptr<SwsContext, ScaleDeleter> scaler(sws_getContext(frame.width, frame.height,
        static_cast<AVPixelFormat>(frame.format), image.width(), image.height(), AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) return {budget("Cannot allocate the video cover scaler"), {}};
    int colorspace = SWS_CS_DEFAULT;
    switch (frame.colorspace) {
    case AVCOL_SPC_BT709: colorspace = SWS_CS_ITU709; break;
    case AVCOL_SPC_FCC: colorspace = SWS_CS_FCC; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: colorspace = SWS_CS_ITU601; break;
    case AVCOL_SPC_SMPTE240M: colorspace = SWS_CS_SMPTE240M; break;
    case AVCOL_SPC_BT2020_NCL: colorspace = SWS_CS_BT2020; break;
    default: break;
    }
    const auto* coefficients = sws_getCoefficients(colorspace);
    const int colorResult = sws_setColorspaceDetails(scaler.get(), coefficients,
        frame.color_range == AVCOL_RANGE_JPEG ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);
    if (colorResult < 0) return {codecFailure(colorResult, "Configure video cover color conversion"), {}};
    std::uint8_t* planes[]{image.bits(), nullptr, nullptr, nullptr};
    int strides[]{static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    const int rows = sws_scale(scaler.get(), frame.data, frame.linesize, 0, frame.height, planes, strides);
    if (rows != image.height()) return {invalid("Video cover scaler returned an incomplete image"), {}};
    if (stop.stop_requested()) return {cancelled(), {}};
    return {{}, std::move(image)};
}

struct DecodeResult {
    Inspection inspection;
    QImage cover;
};

DecodeResult decode(const QString& path, const Limits& limits, std::stop_token stop, unsigned coverEdge)
{
    DecodeResult result;
    result.inspection.kind = Kind::Mp4;
    Input input;
    if ((result.inspection.failure = input.open(path, limits, stop))) return result;
    std::vector<Track> tracks;
    if ((result.inspection.failure = openTracks(input, limits, tracks, coverEdge != 0))) return result;
    Packet packet(av_packet_alloc());
    Frame frame(av_frame_alloc());
    if (!packet || !frame) { result.inspection.failure = budget("Cannot allocate media decoding state"); return result; }

    auto receive = [&](Track& track, bool draining) -> Failure {
        for (;;) {
            if (stop.stop_requested()) return cancelled();
            const int code = avcodec_receive_frame(track.codec.get(), frame.get());
            if (code == AVERROR(EAGAIN))
                return draining ? invalid("Decoder requested more input after end-of-file drain") : Failure{};
            if (code == AVERROR_EOF)
                return draining ? Failure{} : invalid("Decoder ended before all declared packets were supplied");
            if (code < 0) return codecFailure(code, "Decode the complete track");
            const auto error = checkFrame(track, *frame, result.inspection, limits, stop);
            if (error) { av_frame_unref(frame.get()); return error; }
            if (coverEdge && result.cover.isNull()) {
                auto thumbnail = makeCover(*frame, coverEdge, limits, stop);
                result.cover = std::move(thumbnail.image);
                av_frame_unref(frame.get());
                return thumbnail.failure;
            }
            av_frame_unref(frame.get());
        }
    };
    auto send = [&](Track& track, const AVPacket* value) -> Failure {
        int code = avcodec_send_packet(track.codec.get(), value);
        if (code == AVERROR(EAGAIN)) {
            const auto before = track.frames;
            if (auto error = receive(track, false)) return error;
            if (!result.cover.isNull()) return {};
            if (track.frames == before) return invalid("Decoder made no progress while retrying an input packet");
            code = avcodec_send_packet(track.codec.get(), value);
        }
        if (code < 0) return codecFailure(code, value ? "Submit complete media packet" : "Drain final delayed frames");
        return receive(track, value == nullptr);
    };

    int code{};
    while ((code = av_read_frame(input.format, packet.get())) >= 0) {
        if (stop.stop_requested()) { result.inspection.failure = cancelled(); return result; }
        if (packet->stream_index < 0 || static_cast<unsigned>(packet->stream_index) >= input.format->nb_streams) {
            result.inspection.failure = invalid("Demuxer returned an undeclared stream"); return result;
        }
        const auto found = std::ranges::find_if(tracks, [&](const Track& track) { return track.stream->index == packet->stream_index; });
        if (found != tracks.end()) {
            auto& track = *found;
            if (packet->flags & AV_PKT_FLAG_CORRUPT || packet->size <= 0 || packet->pos < 0 || packet->pos > input.size ||
                packet->size > input.size - packet->pos) {
                result.inspection.failure = invalid("Demuxer returned an incomplete or corrupt MP4 packet"); return result;
            }
            if (static_cast<std::uint64_t>(packet->size) > limits.maximumImageBytes) {
                result.inspection.failure = budget("Compressed packet exceeds the byte budget"); return result;
            }
            if (!coverEdge && (result.inspection.failure = checkIndex(input, track, limits))) return result;
            ++track.packets;
            if ((result.inspection.failure = send(track, packet.get()))) return result;
            if (!result.cover.isNull()) return result;
        }
        av_packet_unref(packet.get());
    }
    if (stop.stop_requested()) { result.inspection.failure = cancelled(); return result; }
    if (code != AVERROR_EOF) {
        result.inspection.failure = codecFailure(code, "Read all MP4 packets", input.readFailed ? Error::Read : Error::InvalidMedia);
        return result;
    }
    for (auto& track : tracks) {
        if (!coverEdge) {
            // Fragmented MOV may have inserted entries while reading. Inspect
            // the final complete index again instead of trusting only its tail.
            track.checkedEntries = 0;
            if ((result.inspection.failure = checkIndex(input, track, limits))) return result;
            if (!track.checkedEntries || track.packets != static_cast<std::uint64_t>(track.checkedEntries)) {
                result.inspection.failure = invalid("MP4 ended before every declared sample was read"); return result;
            }
        }
        if ((result.inspection.failure = send(track, nullptr))) return result;
        if (!result.cover.isNull()) return result;
        if (!track.frames) {
            result.inspection.failure = invalid("A declared audio/video track contains no complete decoded sample"); return result;
        }
    }
    if (!result.inspection.frames || (coverEdge && result.cover.isNull()))
        result.inspection.failure = invalid("MP4 contains no complete video frame");
    return result;
}
} // namespace

Inspection inspectVideo(const QString& path, const Limits& limits, std::stop_token stop)
{
    try { return decode(path, limits, stop, 0).inspection; }
    catch (const std::bad_alloc&) {
        Inspection result;
        result.kind = Kind::Mp4;
        result.failure = budget("Cannot allocate video inspection state");
        return result;
    }
}

VideoThumbnailResult videoThumbnail(const QString& path, unsigned edge, const Limits& limits, std::stop_token stop)
{
    if (!edge || edge > INT_MAX) return {budget("Invalid thumbnail edge budget"), {}};
    try {
        auto result = decode(path, limits, stop, edge);
        return {std::move(result.inspection.failure), std::move(result.cover)};
    } catch (const std::bad_alloc&) {
        return {budget("Cannot allocate video thumbnail state"), {}};
    }
}
} // namespace HuhuCapture::Media
