// Decode/drain API flow follows FFmpeg's MIT-licensed demux_decode.c:
// Copyright (c) 2012 Stefano Sabatini. Exact source and full notice are recorded
// in third_party/ffmpeg-examples. The encoder is the unmodified upstream mux.c.
#include "platform/FileCommitter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
int huhucapture_ffmpeg_mux_example_main(int argc, char** argv);
}

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checked(int result, const char* operation)
{
    if (result >= 0) return;
    std::array<char, AV_ERROR_MAX_STRING_SIZE> error{};
    av_strerror(result, error.data(), error.size());
    throw std::runtime_error(std::string(operation) + ": " + error.data());
}

std::filesystem::path nativePath(const QString& value)
{
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    const auto bytes = value.toUtf8();
    return std::filesystem::path(bytes.constData());
#endif
}

struct Decoder {
    AVCodecContext* codec{};
    AVFrame* frame{av_frame_alloc()};
    std::uint64_t frames{};
    std::uint64_t samples{};
    double energy{};
    int stream{-1};
    QImage cover;
    ~Decoder() { av_frame_free(&frame); avcodec_free_context(&codec); }
    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    void open(AVFormatContext* container, AVMediaType type)
    {
        require(frame != nullptr, "Allocate real decoder frame");
        const AVCodec* implementation = nullptr;
        stream = av_find_best_stream(container, type, -1, -1, &implementation, 0);
        checked(stream, "Find encoded stream");
        codec = avcodec_alloc_context3(implementation);
        require(codec != nullptr, "Allocate actual decoder context");
        checked(avcodec_parameters_to_context(codec, container->streams[stream]->codecpar), "Copy decoder parameters");
        codec->thread_count = 2;
        codec->pkt_timebase = container->streams[stream]->time_base;
        checked(avcodec_open2(codec, implementation, nullptr), "Open actual decoder");
    }

    void output()
    {
        ++frames;
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            require(frame->width == 352 && frame->height == 288, "Upstream generated video geometry changed");
            if (cover.isNull()) {
                cover = QImage(frame->width, frame->height, QImage::Format_ARGB32);
                require(!cover.isNull(), "Allocate Qt video cover");
                std::unique_ptr<SwsContext, decltype(&sws_freeContext)> conversion(
                    sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                        frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr),
                    &sws_freeContext);
                require(conversion != nullptr, "Create actual YUV to Qt BGRA conversion");
                std::uint8_t* destination[4]{cover.bits(), nullptr, nullptr, nullptr};
                const int strides[4]{static_cast<int>(cover.bytesPerLine()), 0, 0, 0};
                require(sws_scale(conversion.get(), frame->data, frame->linesize, 0, frame->height,
                    destination, strides) == frame->height, "Convert the entire decoded cover");
            }
        } else {
            require(frame->sample_rate == 44100 && frame->ch_layout.nb_channels == 2,
                "Upstream generated audio format changed");
            require(frame->format == AV_SAMPLE_FMT_FLTP && frame->nb_samples > 0,
                "Native AAC decoder did not produce real float planar samples");
            samples += static_cast<std::uint64_t>(frame->nb_samples);
            for (int channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
                const auto* data = reinterpret_cast<const float*>(frame->extended_data[channel]);
                require(data != nullptr, "Missing decoded audio plane");
                for (int i = 0; i < frame->nb_samples; ++i) {
                    require(std::isfinite(data[i]), "AAC decoded a nonfinite sample");
                    energy += static_cast<double>(data[i]) * data[i];
                }
            }
        }
    }

    void decode(const AVPacket* packet)
    {
        checked(avcodec_send_packet(codec, packet), "Submit real encoded packet");
        for (;;) {
            const auto result = avcodec_receive_frame(codec, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            checked(result, "Decode complete media");
            output();
            av_frame_unref(frame);
        }
    }
};

struct Input {
    AVFormatContext* container{};
    AVPacket* packet{av_packet_alloc()};
    ~Input() { av_packet_free(&packet); avformat_close_input(&container); }
};

void validate(const QString& path, const QString& evidence)
{
    Input input;
    require(input.packet != nullptr, "Allocate encoded packet");
    const auto encodedPath = path.toUtf8();
    checked(avformat_open_input(&input.container, encodedPath.constData(), nullptr, nullptr), "Open finalized MP4");
    checked(avformat_find_stream_info(input.container, nullptr), "Read complete MP4 stream metadata");
    require(input.container->nb_streams == 2, "Software MP4 must contain video and audio");
    Decoder video, audio;
    video.open(input.container, AVMEDIA_TYPE_VIDEO);
    audio.open(input.container, AVMEDIA_TYPE_AUDIO);
    require(video.codec->codec_id == AV_CODEC_ID_H264, "Software fallback did not encode H.264");
    require(audio.codec->codec_id == AV_CODEC_ID_AAC, "Software fallback did not encode AAC");
    int result;
    while ((result = av_read_frame(input.container, input.packet)) >= 0) {
        if (input.packet->stream_index == video.stream) video.decode(input.packet);
        else if (input.packet->stream_index == audio.stream) audio.decode(input.packet);
        else throw std::runtime_error("Unexpected extra media stream");
        av_packet_unref(input.packet);
    }
    require(result == AVERROR_EOF, "Input ended with a real read error, not EOF");
    video.decode(nullptr);
    audio.decode(nullptr);
    // mux.c intentionally includes the frame at exactly 10 seconds: 0..250.
    require(video.frames == 251, "Encoder flush lost or duplicated video frames");
    require(audio.samples >= 441000 && audio.samples <= 441000 + 2048,
        "AAC sample duration is inconsistent with the actual ten-second input");
    require(audio.energy > 1.0, "Decoded AAC is empty or falsely silent");
    require(video.cover.save(evidence + QStringLiteral("/software-cover.png"), "PNG"),
        "Write the actual Qt video cover");
    std::cout << "Decoded H.264 frames=" << video.frames << ", AAC samples/channel=" << audio.samples
              << ", sample energy=" << audio.energy << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        require(argc == 2, "Provide a private evidence directory");
        const auto evidenceRoot = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
        require(QDir().mkpath(evidenceRoot), "Create validation evidence directory");
        QTemporaryDir evidenceDirectory(evidenceRoot + QStringLiteral("/run-XXXXXX"));
        require(evidenceDirectory.isValid(), "Create an exclusively owned evidence run directory");
        const auto evidence = evidenceDirectory.path();
        require(avcodec_version() == LIBAVCODEC_VERSION_INT && avformat_version() == LIBAVFORMAT_VERSION_INT
            && avutil_version() == LIBAVUTIL_VERSION_INT && swscale_version() == LIBSWSCALE_VERSION_INT
            && swresample_version() == LIBSWRESAMPLE_VERSION_INT, "Compiled and loaded FFmpeg ABIs differ");
        require(LIBAVCODEC_VERSION_INT == AV_VERSION_INT(63, 1, 102), "Unpinned FFmpeg development headers");
        const std::string_view configuration(avcodec_configuration());
        require(configuration.find("--enable-gpl") != configuration.npos
            && configuration.find("--enable-version3") != configuration.npos
            && configuration.find("--enable-libx264") != configuration.npos
            && configuration.find("--enable-nonfree") == configuration.npos, "Unexpected codec/license configuration");
        require(avcodec_find_encoder_by_name("libx264") && avcodec_find_encoder_by_name("aac"),
            "The required software H.264 and native AAC encoders are unavailable");
        std::cout << "FFmpeg " << av_version_info() << "; " << avcodec_license()
                  << "; Qt " << qVersion() << '\n';
        QTemporaryDir temporary(evidence + QStringLiteral("/音视频-XXXXXX"));
        require(temporary.isValid(), "Create private encoding workspace");
        std::error_code error;
        const auto staging = HuhuCapture::Platform::createExclusiveTemporaryFile(nativePath(temporary.path()), error);
        require(!error && !staging.empty(), "Claim an owned staging file");
        const auto stagingBytes = staging.string();
        const auto original = QString::fromUtf8(stagingBytes.data(), static_cast<qsizetype>(stagingBytes.size()));
        const auto mp4 = original + QStringLiteral(".mp4");
        require(QFile::rename(original, mp4), "Name the owned staging file for MP4 format selection");
        auto program = QByteArray("huhucapture-p00-software-codec");
        auto output = mp4.toUtf8();
        char* upstreamArguments[]{program.data(), output.data(), nullptr};
        require(huhucapture_ffmpeg_mux_example_main(2, upstreamArguments) == 0,
            "The actual upstream libavcodec/libavformat API path failed");
        validate(mp4, evidence);
        const auto archived = evidence + QStringLiteral("/software-codec.mp4");
        const auto committed = HuhuCapture::Platform::commitFileNoReplace(nativePath(mp4), nativePath(archived));
        require(committed.durable(), "Finalized and fully decoded MP4 could not be committed durably");
        evidenceDirectory.setAutoRemove(false);
        std::cout << "PASS same-process Qt + software libx264/native AAC, full decode and archive commit\n"
                  << "This uses known generated test signals; desktop/system/microphone capture is not tested.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL software codec validation: " << error.what() << '\n';
        return 1;
    }
}
