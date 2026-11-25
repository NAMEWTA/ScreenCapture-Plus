#include "services/media/VideoInspection.h"
#include "platform/FileIdentity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}
#ifdef _WIN32
#include "platform/windows/ComApartment.h"
#include "platform/windows/FileHandleStream.h"
#include <atlbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace media = HuhuCapture::Media;
namespace platform = HuhuCapture::Platform;
namespace {
constexpr int width = 352, height = 288, videoFrames = 51, fps = 25;
constexpr auto ffmpegCommit = "946fcce07b6dcd0331c8cc609192aeff5e1924f8";
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
QString sha256(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Open actual legacy-audio fixture bytes");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Hash the entire actual encoded fixture");
    return QString::fromLatin1(hash.result().toHex());
}
#ifdef _WIN32
void writeJson(const QString& path, const QJsonObject& value)
{
    QFile file(path); const auto bytes = QJsonDocument(value).toJson();
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size() && file.flush(),
        "Publish the actual legacy-audio fixture evidence exclusively");
}
#endif
struct Input {
    QFile file;
    qint64 size{};
    AVIOContext* io{};
    AVFormatContext* context{};
    AVPacket* packet{av_packet_alloc()};
    ~Input()
    {
        av_packet_free(&packet); avformat_close_input(&context);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
    static int read(void* opaque, std::uint8_t* bytes, int count)
    {
        auto& input = *static_cast<Input*>(opaque);
        const auto read = input.file.read(reinterpret_cast<char*>(bytes), count);
        return read < 0 ? AVERROR(EIO) : read == 0 ? AVERROR_EOF : static_cast<int>(read);
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence)
    {
        auto& input = *static_cast<Input*>(opaque);
        if (whence == AVSEEK_SIZE) return input.size;
        whence &= ~AVSEEK_FORCE;
        qint64 base{};
        if (whence == SEEK_CUR) base = input.file.pos();
        else if (whence == SEEK_END) base = input.size;
        else if (whence != SEEK_SET) return AVERROR(EINVAL);
        if (base < 0 || base > input.size || offset < -base || offset > input.size - base)
            return AVERROR_INVALIDDATA;
        return input.file.seek(base + offset) ? base + offset : AVERROR(EIO);
    }
    static int denyExternal(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**)
    { return AVERROR(EPERM); }
    void open(const QString& path)
    {
        require(packet, "Allocate a real compressed packet");
        file.setFileName(path);
        require(file.open(QIODevice::ReadOnly), "Open the actual owned local fixture");
        size = file.size(); require(size > 0 && size <= 256 * 1024 * 1024, "Read a bounded encoded fixture");
        auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
        require(buffer, "Allocate bounded local fixture IO");
        io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
        if (!io) { av_free(buffer); throw std::runtime_error("Allocate local fixture IO state"); }
        context = avformat_alloc_context(); require(context, "Allocate the real MOV demuxer");
        context->pb = io; context->flags |= AVFMT_FLAG_CUSTOM_IO; context->io_open = denyExternal;
        // The Windows production inspection profile intentionally contains no
        // file or network protocol. Use the same local QFile AVIO boundary as
        // the real inspector, including Unicode paths and denied external IO.
        const auto* mov = av_find_input_format("mov");
        require(mov && avformat_open_input(&context, nullptr, mov, nullptr) == 0 &&
            avformat_find_stream_info(context, nullptr) >= 0, "Read actual MP4 stream descriptions");
    }
};
std::pair<qint64, int> lateMp3Packet(const QString& path)
{
    Input input; input.open(path);
    unsigned audio{}, video{}; int audioIndex = -1;
    for (unsigned index = 0; index < input.context->nb_streams; ++index) {
        const auto* parameters = input.context->streams[index]->codecpar;
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++video; require(parameters->codec_id == AV_CODEC_ID_H264, "MF encoded actual H.264 video");
        } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audio; audioIndex = static_cast<int>(index);
            require(parameters->codec_id == AV_CODEC_ID_MP3 && parameters->sample_rate == 44100 &&
                parameters->ch_layout.nb_channels == 2, "MF preserved the shipping stereo 44.1 kHz MP3 audio format");
        }
    }
    require(audio == 1 && video == 1, "The legacy fixture has one real video and one real MP3 track");
    std::vector<std::pair<qint64, int>> packets;
    int status{};
    while ((status = av_read_frame(input.context, input.packet)) >= 0) {
        if (input.packet->stream_index == audioIndex) {
            require(input.packet->pos >= 0 && input.packet->size > 4, "Locate actual compressed MP3 samples in the container");
            packets.emplace_back(input.packet->pos, input.packet->size);
        }
        av_packet_unref(input.packet);
    }
    require(status == AVERROR_EOF && packets.size() > 32, "The real MP3 encoder produced a sequence suitable for late corruption");
    return packets[packets.size() * 3 / 4];
}

#ifdef _WIN32
void checked(HRESULT status, const char* operation)
{
    if (FAILED(status)) throw std::runtime_error(std::string(operation) + " (HRESULT " +
        std::to_string(static_cast<std::uint32_t>(status)) + ")");
}
void typeVideo(IMFMediaType* type, const GUID& subtype)
{
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video major type");
    checked(type->SetGUID(MF_MT_SUBTYPE, subtype), "Set actual video codec/input type");
    checked(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set progressive video");
    checked(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height), "Set actual video geometry");
    checked(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1), "Set actual video cadence");
    checked(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set square pixels");
}
void sample(IMFSinkWriter* writer, DWORD stream, const BYTE* bytes, DWORD length,
    LONGLONG timestamp, LONGLONG duration)
{
    CComPtr<IMFMediaBuffer> buffer; checked(MFCreateMemoryBuffer(length, &buffer), "Allocate bounded actual MF input");
    BYTE* target{}; checked(buffer->Lock(&target, nullptr, nullptr), "Lock actual MF sample storage");
    std::memcpy(target, bytes, length); checked(buffer->Unlock(), "Unlock actual MF sample storage");
    checked(buffer->SetCurrentLength(length), "Publish complete MF sample bytes");
    CComPtr<IMFSample> value; checked(MFCreateSample(&value), "Create actual MF sample");
    checked(value->AddBuffer(buffer), "Attach owned MF sample buffer");
    checked(value->SetSampleTime(timestamp), "Set monotonic 100 ns input timestamp");
    checked(value->SetSampleDuration(duration), "Set actual input sample duration");
    checked(writer->WriteSample(stream, value), "Encode an actual sample through the Windows media codec");
}
void generate(const QString& directory)
{
    require(!QFile::exists(directory) && QDir().mkpath(directory), "Create an exclusively new native MP3 fixture directory");
    platform::ComApartment apartment(COINIT_APARTMENTTHREADED); checked(apartment.result(), "Initialize native test COM apartment");
    checked(MFStartup(MF_VERSION), "Initialize actual Media Foundation");
    struct Shutdown { ~Shutdown() { MFShutdown(); } } shutdown;
    const auto file = directory + QStringLiteral("/windows-h264-mp3.mp4");
    QFile claim(file); require(claim.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Claim only the new owned native recording"); claim.close();
    std::error_code error;
    const auto identity = platform::readFileSnapshot(file.toStdWString(), error);
    require(identity.has_value(), "Read the newly claimed output identity");
    auto descriptor = platform::openOwnedEmptyFileForWrite(file.toStdWString(), identity->identity, error);
    require(bool(descriptor), "Open the exact untruncated native output object");
    CComPtr<IStream> stream; checked(platform::Windows::createFileHandleStream(descriptor.get(), &stream), "Reuse the production owned-descriptor IStream adapter");
    CComPtr<IMFByteStream> bytes; checked(MFCreateMFByteStreamOnStream(stream, &bytes), "Use the production MF byte-stream bridge");
    CComPtr<IMFAttributes> options; checked(MFCreateAttributes(&options, 3), "Create actual native encoder options");
    checked(options->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4), "Select the real MP4 sink");
    checked(options->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE), "Select reproducible native software codecs");
    checked(options->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "Encode controlled fixture input without real-time throttling");
    CComPtr<IMFSinkWriter> writer;
    checked(MFCreateSinkWriterFromURL(nullptr, bytes, options, &writer), "Create actual MP4 sink writer");
    CComPtr<IMFMediaType> video, rawVideo, audio, rawAudio;
    checked(MFCreateMediaType(&video), "Create encoded video media type"); typeVideo(video, MFVideoFormat_H264);
    checked(video->SetUINT32(MF_MT_AVG_BITRATE, 1'500'000), "Set encoded video bitrate");
    DWORD videoStream{}, audioStream{};
    checked(writer->AddStream(video, &videoStream), "Declare actual H.264 output stream");
    checked(MFCreateMediaType(&audio), "Create encoded audio media type");
    checked(audio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "Set encoded audio major type");
    checked(audio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3), "Preserve the actual shipping MP3 codec");
    checked(audio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2), "Set shipping audio channel count");
    checked(audio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100), "Set shipping audio sample rate");
    checked(audio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000 / 8), "Set shipping audio bitrate");
    checked(writer->AddStream(audio, &audioStream), "Declare actual MP3-in-MP4 output stream");
    checked(MFCreateMediaType(&rawVideo), "Create native BGRA input type"); typeVideo(rawVideo, MFVideoFormat_RGB32);
    checked(writer->SetInputMediaType(videoStream, rawVideo, nullptr), "Resolve the actual Windows H.264 encoder");
    checked(MFCreateMediaType(&rawAudio), "Create native PCM input type");
    checked(rawAudio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "Set PCM major type");
    checked(rawAudio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "Set real signed PCM input");
    checked(rawAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2), "Set PCM channel count");
    checked(rawAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100), "Set PCM sample rate");
    checked(rawAudio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "Set PCM sample width");
    checked(rawAudio->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4), "Set interleaved PCM alignment");
    checked(rawAudio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4), "Set PCM data rate");
    checked(writer->SetInputMediaType(audioStream, rawAudio, nullptr), "Resolve the actual Windows MP3 encoder");
    checked(writer->BeginWriting(), "Begin actual native encoding");
    std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
    constexpr int audioFrames = 44100 / fps;
    std::array<std::int16_t, audioFrames * 2> pcm{};
    constexpr LONGLONG duration = 10'000'000 / fps;
    for (int index = 0; index < videoFrames; ++index) {
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            auto* pixel = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            pixel[0] = static_cast<BYTE>(x + index * 3); pixel[1] = static_cast<BYTE>(y + index * 5);
            pixel[2] = static_cast<BYTE>((x ^ y) + index * 7); pixel[3] = 255;
        }
        for (int frame = 0; frame < audioFrames; ++frame) {
            const auto time = static_cast<double>(index * audioFrames + frame) / 44100;
            pcm[frame * 2] = static_cast<std::int16_t>(12000 * std::sin(time * 440 * 6.283185307179586));
            pcm[frame * 2 + 1] = static_cast<std::int16_t>(8000 * std::sin(time * 660 * 6.283185307179586));
        }
        sample(writer, videoStream, pixels.data(), static_cast<DWORD>(pixels.size()), index * duration, duration);
        sample(writer, audioStream, reinterpret_cast<const BYTE*>(pcm.data()), static_cast<DWORD>(sizeof(pcm)), index * duration, duration);
    }
    checked(writer->Finalize(), "Drain actual Windows H.264/MP3 codecs and write the MP4 trailer"); writer.Release();
    checked(bytes->Flush(), "Flush completed native recording"); checked(bytes->Close(), "Close completed native recording");
    bytes.Release(); stream.Release(); require(descriptor.close(error), "Close the final native file handle");
    const auto position = lateMp3Packet(file);
    const auto corrupted = directory + QStringLiteral("/windows-corrupt-late-mp3.mp4");
    require(QFile::copy(file, corrupted), "Preserve the valid native recording before corrupting another owned copy");
    QFile output(corrupted); const QByteArray invalid(position.second, '\0');
    require(output.open(QIODevice::ReadWrite) && output.seek(position.first) && output.write(invalid) == invalid.size() && output.flush(),
        "Corrupt a physically located late compressed MP3 packet, leaving the video and early audio intact"); output.close();
    QJsonArray cases;
    for (const auto& name : {QStringLiteral("windows-h264-mp3.mp4"), QStringLiteral("windows-corrupt-late-mp3.mp4")})
        cases.append(QJsonObject{{QStringLiteral("file"), name}, {QStringLiteral("accept"), name == QStringLiteral("windows-h264-mp3.mp4")},
            {QStringLiteral("sha256"), sha256(directory + QChar(u'/') + name)}});
    writeJson(directory + QStringLiteral("/legacy-mp3-manifest.json"), {
        {QStringLiteral("schema"), 1}, {QStringLiteral("ffmpegCommit"), QString::fromLatin1(ffmpegCommit)},
        {QStringLiteral("generator"), QStringLiteral("Actual Windows Media Foundation H.264 + MP3 encoders; production owned-file stream adapter")},
        {QStringLiteral("kernelVersion"), QSysInfo::kernelVersion()}, {QStringLiteral("videoFrames"), videoFrames},
        {QStringLiteral("audioCodec"), QStringLiteral("mp3")}, {QStringLiteral("cases"), cases}});
}
#endif
void inspect(const QString& directory)
{
    require(avcodec_version() == LIBAVCODEC_VERSION_INT && LIBAVCODEC_VERSION_INT == AV_VERSION_INT(63, 1, 102),
        "Use the actual pinned FFmpeg 9.0.2 decoder ABI");
    require(avcodec_find_decoder(AV_CODEC_ID_MP3) != nullptr, "The actual production dependency includes its legacy MP3 decoder");
    auto* parser = av_parser_init(AV_CODEC_ID_MP3); require(parser != nullptr, "The actual dependency includes the MPEG audio parser"); av_parser_close(parser);
    QFile file(directory + QStringLiteral("/legacy-mp3-manifest.json"));
    require(file.open(QIODevice::ReadOnly), "Load the actual Windows-generated legacy-audio manifest");
    const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    require(manifest.value(QStringLiteral("schema")).toInt() == 1 &&
        manifest.value(QStringLiteral("ffmpegCommit")).toString() == QString::fromLatin1(ffmpegCommit) &&
        manifest.value(QStringLiteral("audioCodec")).toString() == QStringLiteral("mp3") &&
        manifest.value(QStringLiteral("videoFrames")).toInt() == videoFrames,
        "Fixture provenance identifies the native baseline and exact decoder source");
    const auto cases = manifest.value(QStringLiteral("cases")).toArray(); require(cases.size() == 2, "Both actual valid and late-corrupt MP3 cases are required");
    for (const auto& value : cases) {
        const auto entry = value.toObject(); const auto name = entry.value(QStringLiteral("file")).toString();
        require(!name.isEmpty() && !name.contains(QChar(u'/')) && !name.contains(QChar(u'\\')) && name != QStringLiteral(".."), "Fixture name cannot escape its owned directory");
        const auto path = directory + QChar(u'/') + name;
        require(sha256(path) == entry.value(QStringLiteral("sha256")).toString(), "Both platforms decode the exact same Windows-generated bytes");
        const auto result = media::inspectVideo(path);
        const bool accepted = entry.value(QStringLiteral("accept")).toBool();
        if (accepted && result.failure) throw std::runtime_error("Native legacy MP3 recording was rejected: " + result.failure.detail.toStdString());
        require(!result.failure == accepted, "The complete decoder must reject late corrupt audio even when video preview succeeds");
        if (accepted) {
            lateMp3Packet(path);
            require(result.frames == videoFrames && result.dimensions == QSize(width, height), "Every encoded native video frame survives full-track validation");
        }
        std::cout << "PASS " << name.toStdString() << ": accepted=" << !result.failure << ", frames=" << result.frames << '\n';
    }
    std::cout << "Scope: actual native encoded local media and strict all-track decoding; no desktop/audio hardware acquisition claimed\n";
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
#ifdef _WIN32
        if (argc == 3 && std::string(argv[1]) == "--generate") {
            const auto directory = QDir(QString::fromLocal8Bit(argv[2])).absolutePath();
            generate(directory); inspect(directory); return 0;
        }
#endif
        require(argc == 2, "Provide the exact Windows-generated legacy MP3 fixture directory");
        inspect(QDir(QString::fromLocal8Bit(argv[1])).absolutePath()); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
