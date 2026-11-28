#include "services/media/GifEncoder.h"
#include "services/media/GifInspection.h"

#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace media = HuhuCapture::Media;
namespace core = HuhuCapture::Core;
namespace platform = HuhuCapture::Platform;
using namespace std::chrono_literals;

namespace {
constexpr int width = 32, height = 24;
constexpr std::size_t stride = width * 4 + 16;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toStdWString();
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}

QColor expected(unsigned frame, int x, int y)
{
    const std::array<QColor, 4> colors{QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue), QColor(Qt::yellow)};
    const auto quadrant = static_cast<unsigned>((y >= height / 2 ? 2 : 0) + (x >= width / 2 ? 1 : 0));
    return colors[(quadrant + frame) % colors.size()];
}
void fillRow(std::span<std::uint8_t> bytes, unsigned frame, int y, bool bgra)
{
    require(bytes.size() >= width * 4, "Provide an actual complete input row");
    for (int x = 0; x < width; ++x) {
        const auto color = expected(frame, x, y);
        const auto p = static_cast<std::size_t>(x) * 4;
        bytes[p] = static_cast<std::uint8_t>(bgra ? color.blue() : color.red());
        bytes[p + 1] = static_cast<std::uint8_t>(color.green());
        bytes[p + 2] = static_cast<std::uint8_t>(bgra ? color.red() : color.blue());
        bytes[p + 3] = 255;
    }
}
std::vector<std::uint8_t> bgra(unsigned index)
{
    std::vector<std::uint8_t> result(stride * height, 0xcd);
    for (int y = 0; y < height; ++y)
        fillRow(std::span(result).subspan(static_cast<std::size_t>(y) * stride, width * 4), index, y, true);
    return result;
}

media::StagingFile staging(media::MediaArchiveService& archive)
{
    auto result = archive.createStaging();
    require(!result.failure && result.file, "Create a real exclusive staging object");
    return std::move(result.file);
}
platform::FileHandle writer(media::StagingFile& file)
{
    std::error_code error;
    auto result = file.openWriter(error);
    require(result && !error, "Open the same owned empty staging file");
    return result;
}
core::MediaTime pts(unsigned index) { return core::MediaTime(index * 625000); }

void validate(const QString& path)
{
    const auto complete = media::inspectGif(path);
    require(!complete.failure && complete.dimensions == QSize(width, height) && complete.frames == 3,
        "The strict aggregate decoder must consume the complete three-frame GIF");
    QImageReader reader(path, "gif");
    require(reader.supportsAnimation() && reader.imageCount() == 3 && reader.loopCount() == -1,
        "Preserve animation frame count and infinite-repeat behavior");
    int totalDelay{};
    for (unsigned index = 0; index < 3; ++index) {
        const auto frame = reader.read();
        require(!frame.isNull() && frame.size() == QSize(width, height), "Decode a real complete GIF frame");
        for (const int y : {height / 4, height * 3 / 4})
            for (const int x : {width / 4, width * 3 / 4})
                require(frame.pixelColor(x, y) == expected(index, x, y),
                    "Real GIF colors, row orientation, padding and frame lifetime must match the source");
        totalDelay += reader.nextImageDelay();
    }
    require(!reader.canRead() && totalDelay >= 180 && totalDelay <= 200,
        "The complete three-frame 16 FPS timeline must retain its rounded GIF delays");
}

void paddedBgra(media::MediaArchiveService& archive)
{
    auto file = staging(archive);
    media::GifEncoder encoder({width, height}, writer(file));
    require(encoder.ready(), "Prepare the actual fixed encoder");
    for (unsigned index = 0; index < 3; ++index) {
        auto pixels = bgra(index);
        require(encoder.addBgra(pixels, stride, pts(index)), "Encode an actual padded top-down BGRA frame");
        // The C ABI copies each accepted input. Immediate overwrite and release
        // must not change frames still being quantized by the real Rust writer.
        std::fill(pixels.begin(), pixels.end(), 0);
    }
    const auto result = encoder.finish();
    require(result.canArchive() && result.framesWritten == 3 && !encoder.ready(),
        "Only successful codec, I/O, flush and close permit archive validation");
    const auto again = encoder.finish();
    require(again.canArchive() && again.framesWritten == result.framesWritten,
        "Repeated finish must not double-free the upstream handle");
    validate(file.path());
    const auto committed = archive.commitStaged(file, media::Kind::Gif);
    require(committed.durable(), "Publish only the completely decoded real GIF");
    validate(committed.path);
    std::cout << "PASS fixed gifski, padded BGRA, exact pixels/delays, writer close and real durable archive\n";
}

core::FrameFormat bottomUpRgba()
{
    auto format = *core::packedFrameFormat(core::FramePixelFormat::Rgba8, width, height);
    format.bytes = stride * height;
    format.planes[0] = {0, format.bytes, stride * (height - 1), -static_cast<std::ptrdiff_t>(stride),
        width * 4, height};
    require(core::validFrameFormat(format), "The shared layout must accept a bounded negative-stride plane");
    return format;
}
core::FrameLease lease(core::FramePool& pool, unsigned index, core::FrameMetadata metadata = {})
{
    auto writable = pool.tryAcquire();
    require(writable.has_value(), "A released one-slot source pool must be reusable");
    for (int y = 0; y < height; ++y)
        fillRow(writable->row(0, static_cast<std::uint32_t>(y)), index, y,
            writable->format().pixelFormat == core::FramePixelFormat::Bgra8);
    metadata.sessionId = 1; metadata.sourceId = 1; metadata.timing.receivedAt = core::MediaClock::now();
    auto result = std::move(*writable).publish(metadata);
    require(static_cast<bool>(result), "Publish a real immutable frame lease");
    return result;
}
void leasedRgba(media::MediaArchiveService& archive)
{
    for (const auto kind : {core::FramePixelFormat::Bgra8, core::FramePixelFormat::Rgba8}) {
        auto format = bottomUpRgba();
        format.pixelFormat = kind;
        core::FramePool pool({1, format.bytes, 1024});
        require(pool.configure(format) == core::FramePoolStatus::Ready, "Allocate the actual bounded source pool");
        auto file = staging(archive);
        media::GifEncoder encoder({width, height}, writer(file));
        for (unsigned index = 0; index < 3; ++index) {
            auto frame = lease(pool, index);
            require(encoder.addFrame(frame, pts(index)), "Consume an actual negative-stride BGRA/RGBA lease");
            frame.reset();
        }
        require(encoder.finish().canArchive(), "Finish all copied lease input before closing the actual output");
        validate(file.path());
    }
    std::cout << "PASS real one-slot lease reuse, BGRA/RGBA conversion and negative-stride orientation\n";
}

void noFramesAndInvalidInputs(media::MediaArchiveService& archive)
{
    {
        auto file = staging(archive);
        media::GifEncoder encoder({width, height}, writer(file));
        require(encoder.ready(), "Prepare an encoder before any frame is available");
        const auto result = encoder.finish();
        require(!result.firstError && result.framesWritten == 0 && !result.canArchive() &&
            result.completion(true) == core::RecordingCompletion::Cancelled && QFileInfo(file.path()).size() == 0,
            "A clean pre-first-frame stop must not start a zero-frame writer or fabricate successful media");
        require(!file.discard(), "Explicitly discard only the actual empty owned file");
    }
    for (const bool duplicate : {false, true}) {
        auto file = staging(archive);
        media::GifEncoder encoder({width, height}, writer(file));
        const auto pixels = bgra(0);
        if (duplicate) require(encoder.addBgra(pixels, stride, pts(0)), "Accept the first actual frame");
        require(!encoder.addBgra(pixels, stride, duplicate ? pts(0) : pts(1)),
            "Reject an invalid first PTS or duplicate PTS before upstream silently skips it");
        const auto result = encoder.finish();
        require(result.firstError && result.firstError->stage == core::RecordingStage::Encode && !result.canArchive(),
            "Codec drain must not erase the rejected input error");
    }
    {
        auto file = staging(archive);
        media::GifEncoder encoder({width, height}, writer(file));
        const auto pixels = bgra(0);
        require(!encoder.addBgra(std::span(pixels).first(5), stride, pts(0)), "Reject a physically short borrowed input");
        require(!encoder.finish().canArchive(), "No out-of-bounds input may produce successful media");
    }
    std::cout << "PASS clean immediate cancellation, strict timestamp admission and actual input bounds\n";
}

void unsupportedMetadata(media::MediaArchiveService& archive)
{
    for (unsigned variant = 0; variant < 3; ++variant) {
        auto format = bottomUpRgba();
        core::FrameMetadata metadata;
        if (variant == 0) metadata.transform = core::FrameTransform::Rotate90;
        if (variant == 1) metadata.crop = core::FrameCrop{0, 0, width / 2, height};
        if (variant == 2) format.color.transfer = core::ColorTransfer::Pq;
        core::FramePool pool({1, format.bytes, 1024});
        require(pool.configure(format) == core::FramePoolStatus::Ready, "Create an actual explicitly described source format");
        auto frame = lease(pool, 0, metadata);
        auto file = staging(archive);
        media::GifEncoder encoder({width, height}, writer(file));
        require(!encoder.addFrame(frame, pts(0)), "Reject unimplemented crop, transform or HDR interpretation");
        require(encoder.finish().firstError.has_value() && QFileInfo(file.path()).size() == 0,
            "An unsupported source must not be flattened into misleading output");
    }
    std::cout << "PASS explicit rejection of unnormalized crop, transform and PQ metadata\n";
}

void ioFailures(media::MediaArchiveService& archive)
{
    auto file = staging(archive);
    std::error_code error;
    auto readOnly = platform::openRegularFileReadOnly(nativePath(file.path()), error);
    require(readOnly && !error, "Open a real read-only OS file descriptor");
    media::GifEncoder denied({width, height}, std::move(readOnly));
    for (unsigned index = 0; index < 3 && denied.ready(); ++index) {
        const auto pixels = bgra(index);
        if (!denied.addBgra(pixels, stride, pts(index))) break;
    }
    const auto failed = denied.finish();
    require(failed.firstError && failed.finalizationError && !failed.canArchive() && QFileInfo(file.path()).size() == 0,
        "An actual OS write rejection must survive the asynchronous encoder finish");

    auto bounded = staging(archive);
    media::GifEncoder limited({width, height, 1024 * 1024, 10}, writer(bounded));
    for (unsigned index = 0; index < 3 && limited.ready(); ++index) {
        const auto pixels = bgra(index);
        if (!limited.addBgra(pixels, stride, pts(index))) break;
    }
    const auto limit = limited.finish();
    require(limit.firstError && limit.finalizationError && !limit.canArchive() && QFileInfo(bounded.path()).size() <= 10,
        "The real writer must fail before exceeding its output budget and never unlink the partial file");
    require(QFileInfo::exists(bounded.path()) && !bounded.retainForRecovery().isEmpty(),
        "The caller owns explicit partial-output retention");
    std::cout << "PASS real read-only fd failure, output-budget write failure, checked finish and retained partial file\n";
}

void replacedPath(media::MediaArchiveService& archive)
{
    auto file = staging(archive);
    const auto original = file.path();
    const auto moved = original + QStringLiteral(".moved.gif");
    media::GifEncoder encoder({width, height}, writer(file));
    require(QFile::rename(original, moved), "Rename the real open owned object");
    QFile replacement(original);
    require(replacement.open(QIODevice::WriteOnly | QIODevice::NewOnly) && replacement.write("unrelated") == 9,
        "Place an actual unrelated object at the old path");
    replacement.close();
    for (unsigned index = 0; index < 3; ++index) {
        const auto pixels = bgra(index);
        require(encoder.addBgra(pixels, stride, pts(index)), "Continue encoding through the original descriptor");
    }
    require(encoder.finish().canArchive(), "Close the same file object after native path replacement");
    validate(moved);
    require(replacement.open(QIODevice::ReadOnly) && replacement.readAll() == QByteArray("unrelated"),
        "No callback may reopen or truncate the replacement pathname");
    replacement.close();
    const auto committed = archive.commitStaged(file, media::Kind::Gif);
    require(!committed.durable() && committed.failure.code == media::Error::ChangedOnDisk &&
        QFileInfo::exists(original) && QFileInfo::exists(moved),
        "The real archive must reject the replaced staging name without deleting either object");
    (void)file.retainForRecovery();
    std::cout << "PASS real renamed descriptor, untouched replacement and archive identity rejection\n";
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir run;
        require(run.isValid(), "Create an isolated actual filesystem test directory");
        media::MediaArchiveService archive(run.path() + QStringLiteral("/archive"),
            [](const QString& path, media::Kind kind, std::stop_token stop) {
                if (kind == media::Kind::Gif) return media::inspectGif(path, {}, stop);
                return media::Inspection{{media::Error::Unsupported, {}}, kind, {}, 0};
            });
        paddedBgra(archive); leasedRgba(archive); noFramesAndInvalidInputs(archive);
        unsupportedMetadata(archive); ioFailures(archive); replacedPath(archive);
        std::cout << "PASS shared GIF encoder with real fixed Rust codec and files; no desktop capture claimed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL shared GIF encoder: " << error.what() << '\n'; return 1;
    }
}
