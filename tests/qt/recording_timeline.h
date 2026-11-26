#pragma once

// Decoder send/receive/drain flow reuses the project's fixed FFmpeg
// demux_decode.c adaptation. Copyright (c) 2012 Stefano Sabatini, MIT;
// exact source and full notice are retained in third_party/ffmpeg-examples.
#include "core/RecordingResult.h"

#include <QFile>
#include <QJsonObject>
#include <QSize>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace HuhuCapture::Tests {
namespace RecordingTimelineDetail {
inline void require(bool value, const char* detail)
{ if (!value) throw std::runtime_error(detail); }
inline void checked(int value, const char* operation)
{
    if (value >= 0) return;
    std::array<char, AV_ERROR_MAX_STRING_SIZE> detail{};
    av_strerror(value, detail.data(), detail.size());
    throw std::runtime_error(std::string(operation) + ": " + detail.data());
}
struct Input {
    QFile file;
    qint64 size{};
    AVIOContext* io{};
    AVFormatContext* format{};
    AVCodecContext* decoder{};
    AVFrame* frame{av_frame_alloc()};
    AVPacket* packet{av_packet_alloc()};
    ~Input()
    {
        av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&decoder);
        avformat_close_input(&format);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
    static int read(void* opaque, std::uint8_t* bytes, int count)
    {
        auto& input = *static_cast<Input*>(opaque);
        const auto result = input.file.read(reinterpret_cast<char*>(bytes), count);
        return result < 0 ? AVERROR(EIO) : result == 0 ? AVERROR_EOF : static_cast<int>(result);
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int origin)
    {
        auto& input = *static_cast<Input*>(opaque);
        if (origin == AVSEEK_SIZE) return input.size;
        origin &= ~AVSEEK_FORCE;
        qint64 base{};
        if (origin == SEEK_CUR) base = input.file.pos();
        else if (origin == SEEK_END) base = input.size;
        else if (origin != SEEK_SET) return AVERROR(EINVAL);
        if (base < 0 || base > input.size || offset < -base || offset > input.size - base) return AVERROR_INVALIDDATA;
        return input.file.seek(base + offset) ? base + offset : AVERROR(EIO);
    }
    static int denyExternal(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**)
    { return AVERROR(EPERM); }
    void open(const QString& path)
    {
        require(frame && packet, "Allocate the real recording timeline decoder");
        file.setFileName(path);
        require(file.open(QIODevice::ReadOnly), "Open the exact actual native recording");
        size = file.size(); require(size > 0 && size <= 256 * 1024 * 1024, "Bound the native probe recording input");
        auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
        require(buffer, "Allocate native recording timeline IO");
        io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
        if (!io) { av_free(buffer); throw std::runtime_error("Allocate native recording timeline AVIO"); }
        format = avformat_alloc_context(); require(format, "Allocate native recording MOV demuxer");
        format->pb = io; format->flags |= AVFMT_FLAG_CUSTOM_IO; format->io_open = denyExternal;
        const auto* mov = av_find_input_format("mov"); require(mov, "Use the fixed MOV demuxer");
        checked(avformat_open_input(&format, nullptr, mov, nullptr), "Read actual native recording container");
        checked(avformat_find_stream_info(format, nullptr), "Read actual native recording streams");
        require(format->nb_streams == 1 && format->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO,
            "This audio-disabled live probe must have exactly its actual video stream");
        const auto* codec = avcodec_find_decoder(format->streams[0]->codecpar->codec_id);
        require(codec, "Use the real retained native codec's software decoder");
        decoder = avcodec_alloc_context3(codec); require(decoder, "Allocate complete timeline decoder");
        checked(avcodec_parameters_to_context(decoder, format->streams[0]->codecpar), "Copy actual codec parameters");
        decoder->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
        decoder->thread_count = 1;
        checked(avcodec_open2(decoder, codec, nullptr), "Open actual complete timeline decoder");
    }
};
}

// This is a complete second decode of short, actual Windows probe output. It
// supplements the production all-track inspector with the real MF stage and
// output-time contract, rather than assuming one input is one FRC output.
inline QJsonObject verifyNativeRecordingTimeline(const QString& path, const Core::RecordingResult& result,
    std::uint32_t fps, QSize expectedSize)
{
    using namespace RecordingTimelineDetail;
    require(result.canArchive() && result.sinkStatistics.has_value() && fps > 0 && fps <= 240,
        "A native recording requires finalized media and actual sink statistics");
    const auto& sink = *result.sinkStatistics;
    require(sink.queryStatus >= 0 && sink.received == result.framesWritten && sink.received > 0 &&
        sink.encoded == sink.processed && sink.encoded > 0 && sink.queuedBytes == 0 &&
        sink.lastReceived == sink.lastSubmitted && sink.lastEncoded >= 0 && sink.lastEncoded == sink.lastProcessed,
        "Finalized MF input, encoded, processed and pending stages must account for all real work");
    require(sink.lastSubmitted >= 0 && sink.lastSubmittedDuration > 0 &&
        sink.lastSubmittedDuration <= (std::numeric_limits<std::int64_t>::max)() - sink.lastSubmitted,
        "The real final submitted sample has a valid bounded interval");
    require(sink.captured == result.framesWritten && sink.captureEnd > 0 &&
        sink.captureEnd == sink.lastOriginalEnd && sink.submittedDuration == sink.tailTargetEnd &&
        sink.lastSubmitted + sink.lastSubmittedDuration == sink.tailTargetEnd,
        "Every captured sample must be submitted in contiguous intervals up to the one fixed tail target, preserving the actual stop");
    Input input; input.open(path);
    const auto timeBase = input.format->streams[0]->time_base;
    require(timeBase.num > 0 && timeBase.den > 0, "Read a positive actual output time base");
    constexpr AVRational hundredNs{1, 10'000'000};
    const AVRational cadence{1, static_cast<int>(fps)};
    const auto inputEnd = sink.lastSubmitted + sink.lastSubmittedDuration;
    require(sink.lastOriginalEnd > sink.lastSubmitted && sink.lastOriginalEnd <= sink.tailTargetEnd &&
        sink.tailTargetEnd == inputEnd,
        "The final native input must preserve its original interval and use its one frozen CFR target");
    // Compute the independent boundary with FFmpeg's rational rescaler rather
    // than the production integer helper. The preceding grid boundary must be
    // strictly before the original end: equality must not add another frame.
    const auto targetFrame = av_rescale_q(sink.tailTargetEnd, hundredNs, cadence);
    require(targetFrame > 0 && av_rescale_q(targetFrame, cadence, hundredNs) == sink.tailTargetEnd,
        "The frozen final target must be an actual rounded nominal-FPS boundary");
    const auto previousBoundary = av_rescale_q(targetFrame - 1, cadence, hundredNs);
    require(previousBoundary >= 0 && previousBoundary < sink.lastOriginalEnd &&
        sink.tailTargetEnd > previousBoundary &&
        sink.tailTargetEnd - sink.lastOriginalEnd < sink.tailTargetEnd - previousBoundary,
        "The final target must be the first covering CFR boundary and add less than one frame");
    std::uint64_t decoded{}, packets{};
    std::int64_t lastPts{}, lastEnd{}, lastPacketPts{AV_NOPTS_VALUE};
    const auto receive = [&](bool draining) {
        for (;;) {
            const auto status = avcodec_receive_frame(input.decoder, input.frame);
            if (status == AVERROR_EOF) return;
            if (status == AVERROR(EAGAIN)) { require(!draining, "The real decoder must drain to EOF"); return; }
            checked(status, "Decode every actual native output frame");
            require(decoded < 100000 && !(input.frame->flags & AV_FRAME_FLAG_CORRUPT) && !input.frame->decode_error_flags &&
                input.frame->width == expectedSize.width() && input.frame->height == expectedSize.height(),
                "Every decoded native frame must be complete and retain the actual geometry");
            const auto expectedPts = av_rescale_q(static_cast<std::int64_t>(decoded), cadence, timeBase);
            const auto expectedEnd = av_rescale_q(static_cast<std::int64_t>(decoded + 1), cadence, timeBase);
            require(input.frame->pts == expectedPts && input.frame->best_effort_timestamp == expectedPts &&
                input.frame->duration == expectedEnd - expectedPts && input.frame->duration > 0,
                "Every output frame must occupy its contiguous real nominal-FPS timeline slot");
            lastPts = expectedPts; lastEnd = expectedEnd; ++decoded;
            av_frame_unref(input.frame);
        }
    };
    int status{};
    while ((status = av_read_frame(input.format, input.packet)) >= 0) {
        require(input.packet->stream_index == 0 && !(input.packet->flags & AV_PKT_FLAG_CORRUPT) &&
            input.packet->pts != AV_NOPTS_VALUE,
            "Every actual output packet belongs to the complete native video stream");
        lastPacketPts = input.packet->pts;
        auto submitted = avcodec_send_packet(input.decoder, input.packet);
        if (submitted == AVERROR(EAGAIN)) { receive(false); submitted = avcodec_send_packet(input.decoder, input.packet); }
        checked(submitted, "Submit every actual compressed output packet");
        ++packets; av_packet_unref(input.packet); receive(false);
    }
    require(status == AVERROR_EOF && !input.file.error() && (!input.io || input.io->error >= 0 || input.io->error == AVERROR_EOF),
        "Actual local recording demux must reach clean EOF");
    checked(avcodec_send_packet(input.decoder, nullptr), "Begin complete native output decoder drain");
    receive(true);
    require(decoded == sink.encoded && decoded == sink.processed && packets == sink.encoded,
        "Every MF encoded/processed output must survive the actual file and complete decoder drain");
    const auto decodedLast = av_rescale_q(lastPts, timeBase, hundredNs);
    const auto decodedEnd = av_rescale_q(lastEnd, timeBase, hundredNs);
    // MF's last-encoded statistic describes call order. With B frames the last
    // encoded packet need not have the greatest presentation timestamp. The
    // independent decoded timeline still proves complete tail coverage below.
    const auto lastEncodedPacket = av_rescale_q(lastPacketPts, timeBase, hundredNs);
    // The producer has already frozen the first CFR boundary covering the
    // observed stop. Require that exact number of independently decoded slots;
    // neither a missing frame nor an extra frame is an acceptable tail result.
    // Only the existing one-100ns rational conversion allowance remains.
    require(lastEncodedPacket >= 0 && std::llabs(lastEncodedPacket - sink.lastEncoded) <= 1 &&
        decoded == static_cast<std::uint64_t>(targetFrame) && decodedEnd >= sink.captureEnd &&
        std::llabs(decodedEnd - sink.tailTargetEnd) <= 1,
        "The encoded packet must match MF and the complete decoded timeline must end at exactly the frozen covering CFR slot");
    return {{QStringLiteral("framesDecoded"), static_cast<qint64>(decoded)},
        {QStringLiteral("packetsDecoded"), static_cast<qint64>(packets)},
        {QStringLiteral("lastDecoded100ns"), static_cast<qint64>(decodedLast)},
        {QStringLiteral("lastEncodedPacket100ns"), static_cast<qint64>(lastEncodedPacket)},
        {QStringLiteral("decodedEnd100ns"), static_cast<qint64>(decodedEnd)},
        {QStringLiteral("submittedEnd100ns"), static_cast<qint64>(inputEnd)},
        {QStringLiteral("lastOriginalEnd100ns"), static_cast<qint64>(sink.lastOriginalEnd)},
        {QStringLiteral("tailTargetEnd100ns"), static_cast<qint64>(sink.tailTargetEnd)},
        {QStringLiteral("framesCaptured"), static_cast<qint64>(sink.captured)},
        {QStringLiteral("expectedOutputSlots"), static_cast<qint64>(targetFrame)},
        {QStringLiteral("tailRounding100ns"), static_cast<qint64>(decodedEnd - inputEnd)},
        {QStringLiteral("tailExtension100ns"), static_cast<qint64>(inputEnd - sink.captureEnd)},
        {QStringLiteral("submittedDuration100ns"), static_cast<qint64>(sink.submittedDuration)},
        {QStringLiteral("captureEnd100ns"), static_cast<qint64>(sink.captureEnd)},
        {QStringLiteral("nominalFps"), static_cast<qint64>(fps)},
        {QStringLiteral("contiguousTimeline"), true}};
}
} // namespace HuhuCapture::Tests
