// Remux stream/packet flow adapted from FFmpeg 9.0.2 doc/examples/remux.c,
// Copyright (c) 2013 Stefano Sabatini (MIT). See the retained complete notice
// and pinned source record in third_party/ffmpeg-examples.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
int huhucapture_ffmpeg_mux_example_main(int argc, char** argv);
}

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checked(int code, const char* operation)
{
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

struct Input {
    AVFormatContext* format{};
    AVPacket* packet{av_packet_alloc()};
    ~Input() { av_packet_free(&packet); avformat_close_input(&format); }
    void open(const QString& path)
    {
        require(packet != nullptr, "Allocate actual fixture packet");
        const auto encoded = path.toUtf8();
        checked(avformat_open_input(&format, encoded.constData(), nullptr, nullptr), "Open encoded fixture");
        checked(avformat_find_stream_info(format, nullptr), "Read encoded fixture metadata");
    }
};

struct Output {
    AVFormatContext* format{};
    ~Output()
    {
        if (format && format->pb) avio_closep(&format->pb);
        avformat_free_context(format);
    }
};

void claim(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Exclusively claim a new generated fixture");
    file.close();
}

void remux(const QString& inputPath, const QString& outputPath, bool video, bool audio,
    bool duplicated, bool fastStart)
{
    Input input;
    input.open(inputPath);
    claim(outputPath);
    Output output;
    const auto path = outputPath.toUtf8();
    checked(avformat_alloc_output_context2(&output.format, nullptr, "mp4", path.constData()), "Allocate fixture MP4 muxer");
    require(output.format != nullptr, "Create actual fixture output context");
    std::vector<std::vector<int>> mapping(input.format->nb_streams);
    for (unsigned copy = 0; copy < (duplicated ? 2U : 1U); ++copy) {
        for (unsigned index = 0; index < input.format->nb_streams; ++index) {
            const auto* source = input.format->streams[index];
            if (!((video && source->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ||
                (audio && source->codecpar->codec_type == AVMEDIA_TYPE_AUDIO))) continue;
            auto* destination = avformat_new_stream(output.format, nullptr);
            require(destination != nullptr, "Allocate real remuxed track");
            checked(avcodec_parameters_copy(destination->codecpar, source->codecpar), "Copy encoded track parameters");
            destination->codecpar->codec_tag = 0;
            destination->time_base = source->time_base;
            mapping[index].push_back(destination->index);
        }
    }
    checked(avio_open(&output.format->pb, path.constData(), AVIO_FLAG_WRITE), "Open owned fixture output");
    AVDictionary* options{};
    if (fastStart) checked(av_dict_set(&options, "movflags", "+faststart", 0), "Configure real fast-start muxing");
    const int header = avformat_write_header(output.format, &options);
    av_dict_free(&options);
    checked(header, "Write real MP4 sample descriptions");
    int code{};
    while ((code = av_read_frame(input.format, input.packet)) >= 0) {
        require(input.packet->stream_index >= 0 && static_cast<unsigned>(input.packet->stream_index) < mapping.size(),
            "Fixture source returned an undeclared track");
        auto* source = input.format->streams[input.packet->stream_index];
        for (const auto destinationIndex : mapping[input.packet->stream_index]) {
            AVPacket* copy = av_packet_clone(input.packet);
            require(copy != nullptr, "Reference the real compressed sample");
            copy->stream_index = destinationIndex;
            av_packet_rescale_ts(copy, source->time_base, output.format->streams[destinationIndex]->time_base);
            copy->pos = -1;
            const auto written = av_interleaved_write_frame(output.format, copy);
            av_packet_free(&copy);
            checked(written, "Write complete remuxed sample");
        }
        av_packet_unref(input.packet);
    }
    require(code == AVERROR_EOF, "Encoded source did not end cleanly");
    checked(av_write_trailer(output.format), "Finalize actual MP4 fixture");
    checked(avio_closep(&output.format->pb), "Close finalized MP4 fixture");
}

struct SampleRange {
    qint64 offset{};
    int length{};
};

SampleRange lateSample(const QString& path, unsigned targetTrack)
{
    Input input;
    input.open(path);
    require(targetTrack < input.format->nb_streams, "Select an existing real fixture track");
    std::vector<SampleRange> samples;
    int code{};
    while ((code = av_read_frame(input.format, input.packet)) >= 0) {
        if (input.packet->stream_index == static_cast<int>(targetTrack)) {
            require(input.packet->pos >= 0 && input.packet->size > 0, "Find physical encoded sample bytes");
            samples.push_back({input.packet->pos, input.packet->size});
        }
        av_packet_unref(input.packet);
    }
    require(code == AVERROR_EOF && samples.size() > 32, "A late corruption fixture needs a real sequence of encoded samples");
    return samples[samples.size() * 3 / 4];
}

void corruptLater(const QString& source, const QString& path, unsigned stream)
{
    const auto range = lateSample(source, stream);
    require(QFile::copy(source, path), "Copy an actual encoded file for a single-packet fault");
    QFile output(path);
    require(output.open(QIODevice::ReadWrite) && output.seek(range.offset), "Open a verified later packet offset");
    const QByteArray invalidPacket(range.length, static_cast<char>(0xff));
    require(output.write(invalidPacket) == invalidPacket.size() && output.flush(), "Overwrite only the selected later compressed packet");
}

void truncatedCopy(const QString& source, const QString& path, qint64 remove)
{
    require(QFile::copy(source, path), "Copy actual encoded bytes for a physical truncation");
    QFile output(path);
    require(output.open(QIODevice::ReadWrite) && output.size() > remove && output.resize(output.size() - remove),
        "Physically remove bytes from the owned file");
}

void brokenSampleTable(const QString& source, const QString& path, bool overflowChild)
{
    QFile input(source);
    require(input.open(QIODevice::ReadOnly) && input.size() < 64 * 1024 * 1024, "Read the small actual encoder fixture");
    auto bytes = input.readAll();
    // This fixture is produced by the fixed upstream muxer above: its last
    // top-level box is moov. Check that invariant before modifying a child;
    // do not guess an offset in arbitrary user input or compressed media.
    const auto movieType = bytes.lastIndexOf("moov");
    require(movieType >= 4, "Locate the generated terminal movie box");
    const auto movie = movieType - 4;
    const auto movieSize = qFromBigEndian<quint32>(bytes.constData() + movie);
    require(movie + movieSize == bytes.size() && movieSize > 8, "Fixed muxer terminal movie extent changed");
    const char* wanted = overflowChild ? "stsd" : "stsz";
    qsizetype chosen = -1;
    for (auto type = bytes.indexOf(wanted, movie + 8); type >= 0; type = bytes.indexOf(wanted, type + 4)) {
        const auto start = type - 4;
        if (start < movie + 8 || start + 20 > bytes.size()) continue;
        const auto size = qFromBigEndian<quint32>(bytes.constData() + start);
        if (size < 20 || size > static_cast<quint64>(bytes.size() - start)) continue;
        if (!overflowChild && (qFromBigEndian<quint32>(bytes.constData() + start + 12) != 0 ||
            qFromBigEndian<quint32>(bytes.constData() + start + 16) != 251)) continue;
        chosen = start;
        break;
    }
    require(chosen >= 0, "Find the real generated video sample-description/size table");
    if (overflowChild) {
        qToBigEndian<quint32>(0x7fffffffU, bytes.data() + chosen);
    } else {
        // The table still physically contains exactly 251 encoded video sample
        // sizes. A tolerant parser can read the next sibling's size as entry
        // 252, rewind after an overread warning, and decode all original frames.
        qToBigEndian<quint32>(252, bytes.data() + chosen + 16);
    }
    QFile output(path);
    require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly) && output.write(bytes) == bytes.size() && output.flush(),
        "Write a real file with only its internal table/child bound modified");
}

QString digest(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Open generated fixture for SHA-256");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Read every generated fixture byte for SHA-256");
    return QString::fromLatin1(hash.result().toHex());
}

void appendCase(QJsonArray& cases, const QString& root, const char* file,
    bool accepted, int frames, int preview)
{
    QJsonObject item;
    item[QStringLiteral("file")] = QString::fromLatin1(file);
    item[QStringLiteral("sha256")] = digest(root + QChar(u'/') + QString::fromLatin1(file));
    item[QStringLiteral("accept")] = accepted;
    item[QStringLiteral("videoFrames")] = frames;
    // -1: malformed container may reject preview too; 0: no video;
    // 1: the undamaged first frame must remain independently usable.
    item[QStringLiteral("preview")] = preview;
    cases.append(item);
}

void writeManifest(const QString& path, const QJsonObject& manifest)
{
    QFile output(path);
    const auto json = QJsonDocument(manifest).toJson();
    require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly) && output.write(json) == json.size() && output.flush(),
        "Persist the shared cross-platform fixture manifest without overwriting a previous result");
}

void generateHevcCases(const QString& root, const QString& source)
{
    require(QFile::exists(root + QStringLiteral("/manifest.json")), "Preserve the original real fixture suite");
    QFile information(source + QStringLiteral("/hevc-source.json"));
    require(information.open(QIODevice::ReadOnly), "Read real x265 and remux build/encoding evidence");
    const auto provenance = QJsonDocument::fromJson(information.readAll()).object();
    const auto encoder = provenance.value(QStringLiteral("x265")).toObject();
    require(encoder.value(QStringLiteral("version")).toString() == QStringLiteral("4.1") &&
        encoder.value(QStringLiteral("archiveSha512")).toString() == QStringLiteral(
            "5af5b958ff507f96a52813a6b72847c9fbc555bf95af287ae7f011055338266fb647dfea3b7789730c7f5c746f7e6cb7d9395ba10cbe43eb5034d3ed99730c3f"),
        "HEVC fixtures must come from the reviewed fixed x265 source");
    require(provenance.value(QStringLiteral("ffmpeg")).toObject().value(QStringLiteral("commit")).toString() ==
        QStringLiteral("946fcce07b6dcd0331c8cc609192aeff5e1924f8"), "HEVC remux must use the fixed FFmpeg source");
    for (const int depth : {8, 10}) {
        const auto filename = QStringLiteral("valid-hevc-%1bit.mp4").arg(depth);
        const auto path = source + QChar(u'/') + filename;
        const auto recorded = provenance.value(QStringLiteral("fixtures")).toObject().value(filename).toObject();
        require(digest(path) == recorded.value(QStringLiteral("sha256")).toString(), "The completed HEVC encoder bytes changed");
        Input input;
        input.open(path);
        require(input.format->nb_streams == 2, "HEVC fixture must contain a real video and audio track");
        const auto* video = input.format->streams[0]->codecpar;
        const auto* pixels = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(video->format));
        require(video->codec_type == AVMEDIA_TYPE_VIDEO && video->codec_id == AV_CODEC_ID_HEVC &&
            video->width == 352 && video->height == 288 &&
            pixels && pixels->nb_components == 3 && pixels->log2_chroma_w == 1 && pixels->log2_chroma_h == 1 &&
            !(pixels->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL)) &&
            pixels->comp[0].depth == depth && pixels->comp[1].depth == depth && pixels->comp[2].depth == depth &&
            video->profile == (depth == 8 ? AV_PROFILE_HEVC_MAIN : AV_PROFILE_HEVC_MAIN_10),
            "Inspect the real HEVC Main/Main10 metadata instead of relabelling an H.264 file");
        require(input.format->streams[1]->codecpar->codec_id == AV_CODEC_ID_AAC, "Preserve the actual AAC audio track");
        require(QFile::copy(path, root + QChar(u'/') + filename), "Claim a new immutable real HEVC fixture");
    }
    const auto main = root + QStringLiteral("/valid-hevc-8bit.mp4");
    const auto main10 = root + QStringLiteral("/valid-hevc-10bit.mp4");
    const auto fast = root + QStringLiteral("/valid-hevc-faststart.mp4");
    const auto multi = root + QStringLiteral("/two-hevc-av-tracks.mp4");
    remux(main, fast, true, true, false, true);
    remux(main, multi, true, true, true, false);
    corruptLater(main, root + QStringLiteral("/corrupt-late-hevc8.mp4"), 0);
    corruptLater(main10, root + QStringLiteral("/corrupt-late-hevc10.mp4"), 0);
    corruptLater(multi, root + QStringLiteral("/corrupt-second-hevc.mp4"), 2);
    corruptLater(multi, root + QStringLiteral("/corrupt-hevc-second-audio.mp4"), 3);
    truncatedCopy(fast, root + QStringLiteral("/truncated-hevc-final-sample.mp4"), 1);
    truncatedCopy(main, root + QStringLiteral("/truncated-hevc-footer.mp4"), 1);
    QJsonArray cases;
    appendCase(cases, root, "valid-hevc-8bit.mp4", true, 251, 1);
    appendCase(cases, root, "valid-hevc-10bit.mp4", true, 251, 1);
    appendCase(cases, root, "valid-hevc-faststart.mp4", true, 251, 1);
    appendCase(cases, root, "two-hevc-av-tracks.mp4", true, 502, 1);
    appendCase(cases, root, "corrupt-late-hevc8.mp4", false, 0, 1);
    appendCase(cases, root, "corrupt-late-hevc10.mp4", false, 0, 1);
    appendCase(cases, root, "corrupt-second-hevc.mp4", false, 0, 1);
    appendCase(cases, root, "corrupt-hevc-second-audio.mp4", false, 0, 1);
    appendCase(cases, root, "truncated-hevc-final-sample.mp4", false, 0, -1);
    appendCase(cases, root, "truncated-hevc-footer.mp4", false, 0, -1);
    QJsonObject manifest;
    manifest[QStringLiteral("schema")] = 1;
    manifest[QStringLiteral("ffmpegCommit")] = QStringLiteral("946fcce07b6dcd0331c8cc609192aeff5e1924f8");
    manifest[QStringLiteral("generator")] = QStringLiteral("Pinned x265 4.1 CLI Main/Main10 plus FFmpeg 9.0.2 stream-copy mux and remux");
    manifest[QStringLiteral("hevcSource")] = provenance;
    manifest[QStringLiteral("cases")] = cases;
    writeManifest(root + QStringLiteral("/hevc-manifest.json"), manifest);
    std::cout << "PASS generated " << cases.size() << " additional actual HEVC inspection fixtures\n";
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(avformat_version() == LIBAVFORMAT_VERSION_INT && LIBAVFORMAT_VERSION_INT == AV_VERSION_INT(63, 1, 102),
            "Fixture generator must use the pinned FFmpeg 9.0.2 ABI");
        if (argc == 4 && std::string(argv[1]) == "--hevc") {
            generateHevcCases(QDir(QString::fromLocal8Bit(argv[2])).absolutePath(),
                QDir(QString::fromLocal8Bit(argv[3])).absolutePath());
            return 0;
        }
        require(argc == 2, "Provide a new private fixture directory");
        const auto root = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
        require(!QFile::exists(root) && QDir().mkpath(root), "Create an exclusively new fixture directory");
        const auto original = root + QStringLiteral("/valid.mp4");
        claim(original);
        auto name = QByteArray("huhucapture-video-fixtures");
        auto path = original.toUtf8();
        char* arguments[]{name.data(), path.data(), nullptr};
        require(huhucapture_ffmpeg_mux_example_main(2, arguments) == 0, "Generate actual H.264 and AAC with the pinned upstream encoder example");
        const auto fast = root + QStringLiteral("/valid-faststart.mp4");
        const auto multi = root + QStringLiteral("/two-av-tracks.mp4");
        remux(original, fast, true, true, false, true);
        remux(original, multi, true, true, true, false);
        remux(original, root + QStringLiteral("/video-only.mp4"), true, false, false, false);
        remux(original, root + QStringLiteral("/audio-only.mp4"), false, true, false, false);
        corruptLater(original, root + QStringLiteral("/corrupt-late-video.mp4"), 0);
        corruptLater(original, root + QStringLiteral("/corrupt-late-audio.mp4"), 1);
        corruptLater(multi, root + QStringLiteral("/corrupt-second-video.mp4"), 2);
        corruptLater(multi, root + QStringLiteral("/corrupt-second-audio.mp4"), 3);
        truncatedCopy(fast, root + QStringLiteral("/truncated-final-sample.mp4"), 1);
        truncatedCopy(original, root + QStringLiteral("/truncated-footer.mp4"), 1);
        brokenSampleTable(original, root + QStringLiteral("/stsz-extra-entry.mp4"), false);
        brokenSampleTable(original, root + QStringLiteral("/nested-child-overflow.mp4"), true);
        const auto extra = root + QStringLiteral("/truncated-extra-box.mp4");
        require(QFile::copy(original, extra), "Copy valid media before a partial trailing box");
        QFile incomplete(extra);
        require(incomplete.open(QIODevice::Append), "Open owned trailing-box fault fixture");
        // A declared 16-byte free box with only 12 physical bytes. No media
        // packet or earlier moov byte is touched; tolerant EOF is insufficient.
        const auto partial = QByteArray::fromHex("000000106672656500000000");
        require(incomplete.write(partial) == partial.size() && incomplete.flush(), "Write a physically incomplete final MP4 box");
        incomplete.close();
        claim(root + QStringLiteral("/empty.mp4"));

        QJsonArray cases;
        const auto add = [&](const char* file, bool accepted, int frames, int preview) {
            appendCase(cases, root, file, accepted, frames, preview);
        };
        add("valid.mp4", true, 251, 1);
        add("valid-faststart.mp4", true, 251, 1);
        add("video-only.mp4", true, 251, 1);
        add("two-av-tracks.mp4", true, 502, 1);
        add("audio-only.mp4", false, 0, 0);
        add("corrupt-late-video.mp4", false, 0, 1);
        add("corrupt-late-audio.mp4", false, 0, 1);
        add("corrupt-second-video.mp4", false, 0, 1);
        add("corrupt-second-audio.mp4", false, 0, 1);
        add("truncated-final-sample.mp4", false, 0, -1);
        add("truncated-footer.mp4", false, 0, -1);
        add("truncated-extra-box.mp4", false, 0, -1);
        add("stsz-extra-entry.mp4", false, 0, -1);
        add("nested-child-overflow.mp4", false, 0, -1);
        add("empty.mp4", false, 0, 0);
        QJsonObject manifest;
        manifest[QStringLiteral("schema")] = 1;
        manifest[QStringLiteral("ffmpegCommit")] = QStringLiteral("946fcce07b6dcd0331c8cc609192aeff5e1924f8");
        manifest[QStringLiteral("generator")] = QStringLiteral("Pinned upstream mux.c plus real libavformat remux; faults modify actual encoded file bytes");
        manifest[QStringLiteral("cases")] = cases;
        writeManifest(root + QStringLiteral("/manifest.json"), manifest);
        std::cout << "PASS generated " << cases.size() << " actual MP4 inspection fixtures in " << root.toStdString() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL video fixture generation: " << error.what() << '\n';
        return 1;
    }
}
