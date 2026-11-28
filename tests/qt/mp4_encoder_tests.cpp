// Full decode/drain flow follows the existing software_codec_probe and the
// fixed MIT FFmpeg demux_decode.c reference in third_party/ffmpeg-examples.
// Copyright (c) 2012 Stefano Sabatini; the full MIT notice is retained there.
#include "services/media/Mp4Encoder.h"
#include "services/media/VideoInspection.h"

#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace media = HuhuCapture::Media;
namespace core = HuhuCapture::Core;
namespace platform = HuhuCapture::Platform;
using namespace std::chrono_literals;
namespace {
constexpr int width = 64, height = 48;
constexpr std::size_t stride = width * 4 + 16;
constexpr AVRational mediaTimeBase{1, 10000000};
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void checked(int code, const char* operation)
{
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toStdWString();
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
media::StagingFile staging(media::MediaArchiveService& archive)
{
    auto created = archive.createStaging();
    require(!created.failure && created.file, "Claim an actual exclusive staging object");
    return std::move(created.file);
}
platform::FileHandle writer(media::StagingFile& file)
{
    std::error_code error;
    auto value = file.openWriter(error);
    require(value && !error, "Open the actual identity-checked output descriptor");
    return value;
}
core::MediaTime pts(unsigned index) { return core::MediaTime(static_cast<std::int64_t>(index) * 10000000 / 30); }
core::MediaTime duration(unsigned index) { return pts(index + 1) - pts(index); }
QColor expected(unsigned index, int x, int y)
{
    const std::array<QColor, 4> colors{QColor(220, 30, 40), QColor(30, 210, 45), QColor(25, 50, 220), QColor(170, 170, 170)};
    const auto quadrant = static_cast<unsigned>((y >= height / 2 ? 2 : 0) + (x >= width / 2 ? 1 : 0));
    return colors[(quadrant + index) % colors.size()];
}
void fill(std::span<std::uint8_t> row, unsigned index, int y, bool bgra)
{
    require(row.size() >= width * 4, "Actual input row must contain every pixel");
    for (int x = 0; x < width; ++x) {
        const auto color = expected(index, x, y);
        const auto p = static_cast<std::size_t>(x) * 4;
        row[p] = static_cast<std::uint8_t>(bgra ? color.blue() : color.red());
        row[p + 1] = static_cast<std::uint8_t>(color.green());
        row[p + 2] = static_cast<std::uint8_t>(bgra ? color.red() : color.blue());
        row[p + 3] = 255;
    }
}
std::vector<std::uint8_t> bgra(unsigned index)
{
    std::vector<std::uint8_t> pixels(stride * height, 0xcd);
    for (int y = 0; y < height; ++y) fill(std::span(pixels).subspan(static_cast<std::size_t>(y) * stride, width * 4), index, y, true);
    return pixels;
}
struct Track {
    AVCodecContext* codec{};
    AVFrame* frame{av_frame_alloc()};
    AVStream* stream{};
    SwsContext* scaler{};
    QImage image{width, height, QImage::Format_ARGB32};
    std::uint64_t frames{}, samples{};
    double energy[2]{};
    int expectedRate{44100}, expectedChannels{2};
    core::MediaTime lastVideoEnd{};
    ~Track() { sws_freeContext(scaler); av_frame_free(&frame); avcodec_free_context(&codec); }
    void open(AVStream* value)
    {
        stream = value;
        const auto* implementation = avcodec_find_decoder(stream->codecpar->codec_id);
        require(implementation && frame && !image.isNull(), "Open the actual complete media decoder");
        codec = avcodec_alloc_context3(implementation);
        require(codec != nullptr, "Allocate actual decoder state");
        checked(avcodec_parameters_to_context(codec, stream->codecpar), "Read encoded stream parameters");
        codec->thread_count = 1;
        codec->pkt_timebase = stream->time_base;
        codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
        codec->error_concealment = 0;
        checked(avcodec_open2(codec, implementation, nullptr), "Initialize actual complete media decoder");
    }
    void output()
    {
        require(!frame->decode_error_flags && !(frame->flags & AV_FRAME_FLAG_CORRUPT), "No corrupt frame may be accepted");
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            require(frame->width == width && frame->height == height && frame->duration > 0,
                "Every actual frame including the tail must have correct geometry and a positive duration");
            require(frame->best_effort_timestamp == av_rescale_q(pts(static_cast<unsigned>(frames)).count(), mediaTimeBase, stream->time_base),
                "B-frame reorder and flush must preserve every requested presentation timestamp");
            lastVideoEnd = core::MediaTime(av_rescale_q(frame->best_effort_timestamp + frame->duration, stream->time_base, mediaTimeBase));
            scaler = sws_getCachedContext(scaler, width, height, static_cast<AVPixelFormat>(frame->format), width, height,
                AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            require(scaler != nullptr && frame->colorspace == AVCOL_SPC_BT709 && frame->color_range == AVCOL_RANGE_MPEG,
                "The real encoded stream must retain its explicit BT.709 limited-range matrix");
            const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
            checked(sws_setColorspaceDetails(scaler, coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16), "Set actual decoder output range");
            std::uint8_t* output[]{image.bits(), nullptr, nullptr, nullptr};
            const int strides[]{static_cast<int>(image.bytesPerLine()), 0, 0, 0};
            require(sws_scale(scaler, frame->data, frame->linesize, 0, height, output, strides) == height,
                "Convert the complete actual frame");
            for (const int y : {height / 4, height * 3 / 4}) for (const int x : {width / 4, width * 3 / 4}) {
                const auto actual = image.pixelColor(x, y);
                const auto color = expected(static_cast<unsigned>(frames), x, y);
                require(std::abs(actual.red() - color.red()) <= 25 && std::abs(actual.green() - color.green()) <= 25 &&
                    std::abs(actual.blue() - color.blue()) <= 25,
                    "Each decoded image, including the last, must retain its own colors and row orientation");
            }
        } else {
            require(frame->sample_rate == expectedRate && frame->ch_layout.nb_channels == expectedChannels && frame->format == AV_SAMPLE_FMT_FLTP &&
                frame->nb_samples > 0, "The actual native AAC track must preserve the mixed PCM format");
            samples += static_cast<unsigned>(frame->nb_samples);
            for (int channel = 0; channel < expectedChannels; ++channel) {
                const auto* data = reinterpret_cast<const float*>(frame->extended_data[channel]);
                require(data != nullptr, "AAC must provide a real decoded plane");
                for (int sample = 0; sample < frame->nb_samples; ++sample) {
                    require(std::isfinite(data[sample]), "Actual decoded AAC must contain finite PCM");
                    energy[channel] += static_cast<double>(data[sample]) * data[sample];
                }
            }
        }
        ++frames;
    }
    void decode(const AVPacket* packet)
    {
        checked(avcodec_send_packet(codec, packet), "Submit actual MP4 packet");
        for (;;) {
            const auto code = avcodec_receive_frame(codec, frame);
            if (code == AVERROR_EOF) return;
            if (code == AVERROR(EAGAIN)) { require(packet != nullptr, "Decoder flush cannot request more input"); return; }
            checked(code, "Decode complete actual MP4");
            output(); av_frame_unref(frame);
        }
    }
};
void validate(const QString& path, unsigned expectedFrames, std::uint64_t expectedSamples = 0,
    int sampleRate = 44100, int channels = 2)
{
    const auto inspection = media::inspectVideo(path);
    if (inspection.failure) throw std::runtime_error("Strict MP4 inspection: " + inspection.failure.detail.toStdString());
    require(inspection.frames == expectedFrames && inspection.dimensions == QSize(width, height),
        "Strict all-track decode must preserve every submitted video frame");
    struct Input {
        AVFormatContext* format{};
        AVPacket* packet{av_packet_alloc()};
        ~Input() { av_packet_free(&packet); avformat_close_input(&format); }
    } input;
    const auto filename = path.toUtf8();
    checked(avformat_open_input(&input.format, filename.constData(), nullptr, nullptr), "Open the actual encoded file");
    require(input.packet && input.format->nb_streams == (expectedSamples ? 2u : 1u), "Only the requested real tracks may be present");
    Track video, audio;
    audio.expectedRate = sampleRate; audio.expectedChannels = channels;
    for (unsigned index = 0; index < input.format->nb_streams; ++index) {
        auto* stream = input.format->streams[index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) video.open(stream);
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio.open(stream);
        else throw std::runtime_error("Unexpected media track");
    }
    require(video.codec && video.codec->codec_id == AV_CODEC_ID_H264 &&
        (expectedSamples ? audio.codec && audio.codec->codec_id == AV_CODEC_ID_AAC : !audio.codec), "Real software codec selection must match the contract");
    int code{};
    while ((code = av_read_frame(input.format, input.packet)) >= 0) {
        if (input.packet->stream_index == video.stream->index) video.decode(input.packet);
        else audio.decode(input.packet);
        av_packet_unref(input.packet);
    }
    require(code == AVERROR_EOF, "A read error cannot count as a successful file end");
    video.decode(nullptr);
    if (audio.codec) audio.decode(nullptr);
    require(video.frames == expectedFrames && std::abs(video.lastVideoEnd.count() - pts(expectedFrames).count()) <= 2,
        "The final encoded sample must cover the entire requested video timeline");
    if (expectedSamples) {
        std::cout << "Actual AAC expected=" << expectedSamples << ", decoded=" << audio.samples << ", frames=" << audio.frames
            << ", channel energy=" << audio.energy[0] << ',' << audio.energy[1] << '\n';
        require(audio.samples == expectedSamples && audio.energy[0] > 1 && (channels == 1 || audio.energy[1] > 1),
            "Actual gapless AAC decode must preserve exactly every input sample in all requested channels");
    }
}
void encode(media::MediaArchiveService& archive, unsigned frames, bool withAudio, int sampleRate = 44100, int channels = 2)
{
    auto file = staging(archive);
    media::Mp4EncoderOptions options{width, height};
    if (withAudio) options.audio = media::Mp4AudioFormat{sampleRate, channels};
    media::Mp4Encoder encoder(options, writer(file));
    require(encoder.ready(), "Prepare the actual fixed software MP4 encoder");
    std::uint64_t samples{};
    for (unsigned index = 0; index < frames; ++index) {
        auto pixels = bgra(index);
        require(encoder.addBgra(pixels, stride, pts(index), duration(index)), "Submit an actual padded source frame");
        std::fill(pixels.begin(), pixels.end(), 0);
        if (withAudio) {
            const auto end = static_cast<std::uint64_t>(index + 1) * static_cast<unsigned>(sampleRate) / 30;
            std::vector<float> pcm(static_cast<std::size_t>(end - samples) * static_cast<unsigned>(channels));
            for (std::size_t i = 0; i < static_cast<std::size_t>(end - samples); ++i)
                for (int channel = 0; channel < channels; ++channel)
                    pcm[i * static_cast<unsigned>(channels) + static_cast<unsigned>(channel)] =
                        (channel ? 0.2f : 0.3f) * static_cast<float>(std::sin((samples + i) * 2.0 * std::numbers::pi *
                            (channel ? 660.0 : 440.0) / sampleRate));
            require(encoder.addAudio(pcm, samples), "Encode real interleaved PCM through native AAC");
            std::fill(pcm.begin(), pcm.end(), 0);
            samples = end;
        }
    }
    const auto result = encoder.finish();
    if (result.firstError) throw std::runtime_error("Real MP4 encoder failed with " + std::to_string(result.firstError->code));
    require(result.canArchive() && result.framesWritten == frames && encoder.statistics().videoPackets == frames &&
        encoder.statistics().audioSamples == samples, "Drain every actual delayed codec packet before successful finish");
    require(encoder.finish().canArchive() && !encoder.ready(), "Repeated finish must be safe and final");
    try { validate(file.path(), frames, samples, sampleRate, channels); }
    catch (...) {
        std::cerr << "Retained failed actual encoder output: " << file.retainForRecovery().toStdString() << '\n';
        throw;
    }
    const auto committed = archive.commitStaged(file, media::Kind::Mp4);
    require(committed.durable(), "Only fully decoded actual media may be durably archived");
    validate(committed.path, frames, samples, sampleRate, channels);
    std::cout << "PASS actual libx264/native AAC, frames=" << frames << ", audio samples=" << samples << ", strict tail/timeline/colors and archive\n";
}
void negativeStride(media::MediaArchiveService& archive)
{
    for (const auto pixelFormat : {core::FramePixelFormat::Bgra8, core::FramePixelFormat::Rgba8}) {
        auto format = *core::packedFrameFormat(pixelFormat, width, height);
        format.bytes = stride * height;
        format.planes[0] = {0, format.bytes, stride * (height - 1), -static_cast<std::ptrdiff_t>(stride), width * 4, height};
        core::FramePool pool({1, format.bytes, 1024});
        require(pool.configure(format) == core::FramePoolStatus::Ready, "Allocate actual one-slot negative-stride source");
        auto file = staging(archive);
        media::Mp4Encoder encoder({width, height}, writer(file));
        for (unsigned index = 0; index < 9; ++index) {
            auto writable = pool.tryAcquire();
            require(writable.has_value(), "Encoder must not retain the source lease after add returns");
            for (int y = 0; y < height; ++y) fill(writable->row(0, static_cast<unsigned>(y)), index, y, pixelFormat == core::FramePixelFormat::Bgra8);
            core::FrameMetadata metadata;
            metadata.sessionId = 1; metadata.sourceId = 2; metadata.timing.receivedAt = core::MediaClock::now();
            auto frame = std::move(*writable).publish(metadata);
            require(frame && encoder.addFrame(frame, pts(index), duration(index)), "Encode actual negative-stride lease before immediate reuse");
            frame.reset();
        }
        require(encoder.finish().canArchive(), "Finish every actual leased frame");
        validate(file.path(), 9);
    }
    std::cout << "PASS actual one-slot BGRA/RGBA lease reuse and negative-stride decoded orientation\n";
}
void rejection(media::MediaArchiveService& archive)
{
    {
        auto file = staging(archive);
        media::Mp4Encoder encoder({width, height}, writer(file));
        const auto result = encoder.finish();
        require(!result.firstError && !result.framesWritten && result.completion(true) == core::RecordingCompletion::Cancelled &&
            QFileInfo(file.path()).size() == 0, "No actual input must remain an empty clean cancellation");
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
        auto file = staging(archive);
        media::Mp4EncoderOptions options{width, height};
        if (variant >= 2) options.audio = media::Mp4AudioFormat{};
        media::Mp4Encoder encoder(options, writer(file));
        auto pixels = bgra(0);
        require(encoder.addBgra(pixels, stride, pts(0), duration(0)), "Submit actual media before a rejected continuation");
        if (variant == 0) require(!encoder.addBgra(pixels, stride, pts(0), duration(0)), "Reject duplicate PTS");
        if (variant == 1) require(!encoder.addBgra(std::span(pixels).first(8), stride, pts(1), duration(1)), "Reject physically short input");
        if (variant == 2) {
            std::vector<float> pcm(64, 0.1f);
            require(!encoder.addAudio(pcm, 1), "Reject reordered/missing audio rather than insert silence");
        }
        if (variant == 3) {
            const std::array<float, 2> pcm{std::numeric_limits<float>::quiet_NaN(), 0};
            require(!encoder.addAudio(pcm, 0), "Reject nonfinite real PCM before the encoder");
        }
        const auto original = encoder.result().firstError;
        const auto result = encoder.finish();
        require(original && result.firstError == original && !result.canArchive(), "Real drain must preserve the first input error");
    }
    {
        auto file = staging(archive);
        media::Mp4EncoderOptions options{width, height}; options.audio = media::Mp4AudioFormat{};
        media::Mp4Encoder encoder(options, writer(file));
        const auto pixels = bgra(0);
        require(encoder.addBgra(pixels, stride, pts(0), duration(0)), "Encode actual video with an explicitly selected audio stream");
        require(encoder.finish().firstError.has_value() && !encoder.result().canArchive(), "Missing selected audio cannot become successful silent output");
    }
    std::cout << "PASS empty cancellation, duplicate/short input, PCM sequence/NaN errors and selected-audio absence\n";
}
void ioAndIdentity(media::MediaArchiveService& archive)
{
    const auto pixels = bgra(0);
    {
        auto file = staging(archive);
        std::error_code error;
        auto readOnly = platform::openRegularFileReadOnly(nativePath(file.path()), error);
        require(readOnly && !error, "Acquire an actual read-only OS descriptor");
        media::Mp4Encoder encoder({width, height}, std::move(readOnly));
        if (encoder.ready()) encoder.addBgra(pixels, stride, pts(0), duration(0));
        const auto result = encoder.finish();
        require(result.firstError && result.finalizationError && !result.canArchive() && QFileInfo(file.path()).size() == 0,
            "Real OS write failure must survive trailer/flush/close");
    }
    {
        auto file = staging(archive);
        media::Mp4EncoderOptions options{width, height}; options.maximumOutputBytes = 10;
        media::Mp4Encoder encoder(options, writer(file));
        encoder.addBgra(pixels, stride, pts(0), duration(0));
        const auto result = encoder.finish();
        require(result.firstError && !result.canArchive() && QFileInfo(file.path()).size() <= 10,
            "Actual seek/write callbacks must enforce the output extent budget");
    }
    {
        auto file = staging(archive);
        const auto original = file.path();
        const auto moved = original + QStringLiteral(".moved.mp4");
        media::Mp4Encoder encoder({width, height}, writer(file));
        require(QFile::rename(original, moved), "Rename the actual open file object");
        QFile replacement(original);
        require(replacement.open(QIODevice::WriteOnly | QIODevice::NewOnly) && replacement.write("unrelated") == 9,
            "Put an unrelated real file at the old path");
        replacement.close();
        require(encoder.addBgra(pixels, stride, pts(0), duration(0)) && encoder.finish().canArchive(),
            "Custom AVIO must encode through the original descriptor, including trailer seeks");
        validate(moved, 1);
        require(!archive.commitStaged(file, media::Kind::Mp4).durable(), "Archive identity must reject the replacement path");
        require(replacement.open(QIODevice::ReadOnly) && replacement.readAll() == "unrelated", "Never truncate the unrelated replacement");
    }
    std::cout << "PASS actual read-only fd error, extent limit, descriptor-only seek and renamed-file identity protection\n";
}
void thumbnailAlignment(media::MediaArchiveService& archive)
{
    // c71's actual X11 252x190 recording exposed a native SIMD store beyond a
    // tightly packed QImage. Exercise that geometry and smaller non-32-byte
    // row cases through the real encoder, decoder and production cover API.
    for (const auto dimensions : {QSize(252, 190), QSize(20, 18), QSize(36, 34), QSize(4, 2)}) {
        auto file = staging(archive);
        const auto rowBytes = static_cast<std::size_t>(dimensions.width()) * 4;
        std::vector<std::uint8_t> pixels(rowBytes * static_cast<unsigned>(dimensions.height()));
        {
            media::Mp4Encoder encoder({dimensions.width(), dimensions.height()}, writer(file));
            for (unsigned frame = 0; frame < 5; ++frame) {
                for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
                    pixels[offset] = 160; pixels[offset + 1] = static_cast<std::uint8_t>(100 + frame * 3);
                    pixels[offset + 2] = 45; pixels[offset + 3] = 255;
                }
                require(encoder.addBgra(pixels, rowBytes, pts(frame), duration(frame)),
                    "Encode actual non-SIMD-width video pixels");
            }
            require(encoder.finish().canArchive(), "Drain the actual small and non-aligned video encoder");
        }
        const auto inspection = media::inspectVideo(file.path());
        require(!inspection.failure && inspection.frames == 5 && inspection.dimensions == dimensions,
            "The alignment regression keeps all actual video frames and dimensions");
        for (const unsigned edge : {512u, 127u, 63u}) {
            const auto expectedSize = dimensions.width() <= static_cast<int>(edge) && dimensions.height() <= static_cast<int>(edge) ? dimensions :
                dimensions.scaled(static_cast<int>(edge), static_cast<int>(edge), Qt::KeepAspectRatio);
            for (int repeat = 0; repeat < 3; ++repeat) {
                const auto cover = media::videoThumbnail(file.path(), edge);
                require(!cover.failure && cover.image.size() == expectedSize,
                    "The production cover must preserve logical pixels while using padded native storage");
                for (int y = 0; y < cover.image.height(); ++y) for (int x = 0; x < cover.image.width(); ++x) {
                    const auto pixel = cover.image.pixelColor(x, y);
                    require(std::abs(pixel.red() - 45) <= 8 && std::abs(pixel.green() - 100) <= 8 &&
                        std::abs(pixel.blue() - 160) <= 8 && pixel.alpha() == 255,
                        "Every returned owned cover pixel must survive native scaler/frame destruction");
                }
            }
        }
    }
    std::cout << "PASS actual SIMD-tail thumbnail geometries, downscale, complete owned pixel rows and native allocation release\n";
}
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    av_log_set_level(AV_LOG_ERROR);
    try {
        if (argc == 2)
            require(QDir().mkpath(QString::fromLocal8Bit(argv[1])), "Create the explicitly requested media-evidence directory");
        QTemporaryDir directory(argc == 2 ? QDir(QString::fromLocal8Bit(argv[1])).absoluteFilePath(QStringLiteral("run-XXXXXX"))
                                         : QDir::tempPath() + QStringLiteral("/huhucapture-mp4-XXXXXX"));
        require(directory.isValid(), "Create actual isolated encoding test storage");
        if (argc == 2) { directory.setAutoRemove(false); std::cout << "Actual media evidence: " << directory.path().toStdString() << '\n'; }
        media::MediaArchiveService archive(directory.path(), [](const QString& path, media::Kind kind, std::stop_token stop) {
            if (kind != media::Kind::Mp4) return media::Inspection{{media::Error::Unsupported, {}}, kind, {}, 0};
            return media::inspectVideo(path, {}, stop);
        });
        encode(archive, 1, false);
        encode(archive, 31, false);
        encode(archive, 31, true);
        encode(archive, 31, true, 48000);
        encode(archive, 1, true, 44100, 1);
        negativeStride(archive);
        rejection(archive);
        ioAndIdentity(archive);
        thumbnailAlignment(archive);
        std::cout << "PASS real fixed software MP4 encoder; generated codec inputs do not prove desktop/audio-device capture\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL software MP4 encoder: " << error.what() << '\n';
        return 1;
    }
}
