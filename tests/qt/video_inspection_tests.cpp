#include "services/media/VideoInspection.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace HuhuCapture::Media;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

QString hashFile(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Read the actual cross-platform fixture");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Read the complete fixture for SHA-256");
    return QString::fromLatin1(hash.result().toHex());
}

void validateCover(const QImage& image, unsigned edge)
{
    require(!image.isNull() && image.width() > 0 && image.height() > 0 &&
        image.width() <= static_cast<int>(edge) && image.height() <= static_cast<int>(edge), "Cover must obey its edge budget");
    require(std::abs(static_cast<double>(image.width()) / image.height() - 352.0 / 288.0) < 0.03,
        "Cover must preserve the actual video's aspect ratio");
    std::set<QRgb> pixels;
    for (int y = 0; y < image.height(); y += 7)
        for (int x = 0; x < image.width(); x += 7) pixels.insert(image.pixel(x, y));
    require(pixels.size() > 8, "Cover must contain the real decoded test pattern");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        const bool hevc = argc == 4 && std::string(argv[3]) == "--hevc";
        require(argc == 3 || hevc, "Provide shared real-fixture and evidence directories, optionally followed by --hevc");
        const auto root = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
        const auto evidenceRoot = QDir(QString::fromLocal8Bit(argv[2])).absolutePath();
        require(QDir().mkpath(evidenceRoot), "Create the video inspection evidence directory");
        QTemporaryDir run(evidenceRoot + QStringLiteral("/run-XXXXXX"));
        require(run.isValid(), "Create an exclusively owned inspection evidence directory");
        run.setAutoRemove(false);
        QFile manifestFile(root + (hevc ? QStringLiteral("/hevc-manifest.json") : QStringLiteral("/manifest.json")));
        require(manifestFile.open(QIODevice::ReadOnly), "Read the actual generated fixture manifest");
        const auto manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        require(manifest.value(QStringLiteral("schema")).toInt() == 1 &&
            manifest.value(QStringLiteral("ffmpegCommit")).toString() == QStringLiteral("946fcce07b6dcd0331c8cc609192aeff5e1924f8"),
            "Fixtures must come from the pinned real encoder");
        const auto cases = manifest.value(QStringLiteral("cases")).toArray();
        require(cases.size() == (hevc ? 10 : 15), "Do not silently omit malformed, bit-depth or additional-track fixtures");
        if (hevc) {
            const auto x265 = manifest.value(QStringLiteral("hevcSource")).toObject().value(QStringLiteral("x265")).toObject();
            require(x265.value(QStringLiteral("version")).toString() == QStringLiteral("4.1") &&
                x265.value(QStringLiteral("archiveSha512")).toString() == QStringLiteral(
                    "5af5b958ff507f96a52813a6b72847c9fbc555bf95af287ae7f011055338266fb647dfea3b7789730c7f5c746f7e6cb7d9395ba10cbe43eb5034d3ed99730c3f"),
                "HEVC test data must retain actual pinned encoder source and binary provenance");
        }
        QJsonArray outcomes;
        unsigned failures{};
        for (const auto& entry : cases) {
            const auto item = entry.toObject();
            const auto name = item.value(QStringLiteral("file")).toString();
            require(!name.isEmpty() && !name.contains(QChar(u'/')) && !name.contains(QChar(u'\\')) && name != QStringLiteral(".."),
                "Fixture names must stay in their supplied directory");
            const auto path = root + QChar(u'/') + name;
            require(hashFile(path) == item.value(QStringLiteral("sha256")).toString(), "Downloaded fixture bytes differ from the generator manifest");
            QJsonObject outcome;
            outcome[QStringLiteral("file")] = name;
            const auto inspected = inspectVideo(path);
            const bool accepted = !inspected.failure;
            outcome[QStringLiteral("accepted")] = accepted;
            outcome[QStringLiteral("expectedAccept")] = item.value(QStringLiteral("accept")).toBool();
            outcome[QStringLiteral("failureCode")] = static_cast<int>(inspected.failure.code);
            outcome[QStringLiteral("detail")] = inspected.failure.detail;
            outcome[QStringLiteral("decodedVideoFrames")] = static_cast<qint64>(inspected.frames);
            bool passed = accepted == item.value(QStringLiteral("accept")).toBool();
            if (accepted) {
                passed = passed && inspected.kind == Kind::Mp4 && inspected.dimensions == QSize(352, 288) &&
                    inspected.frames == static_cast<std::uint64_t>(item.value(QStringLiteral("videoFrames")).toInt());
            }
            const auto preview = videoThumbnail(path, 128);
            const int expectedPreview = item.value(QStringLiteral("preview")).toInt(-1);
            outcome[QStringLiteral("previewAccepted")] = !preview.failure;
            outcome[QStringLiteral("previewDetail")] = preview.failure.detail;
            if (expectedPreview >= 0) passed = passed && ((!preview.failure) == (expectedPreview == 1));
            if (!preview.failure) {
                try { validateCover(preview.image, 128); }
                catch (const std::exception& error) {
                    passed = false;
                    outcome[QStringLiteral("previewAssertion")] = QString::fromUtf8(error.what());
                }
                require(preview.image.save(run.path() + QChar(u'/') + name + QStringLiteral(".png"), "PNG"),
                    "Save the real decoded cover evidence");
            }
            outcome[QStringLiteral("pass")] = passed;
            if (!passed) ++failures;
            outcomes.append(outcome);
            std::cout << (passed ? "PASS " : "FAIL ") << name.toStdString() << ": accepted=" << accepted
                      << ", video_frames=" << inspected.frames << ", " << inspected.failure.detail.toStdString() << '\n';
        }

        QJsonObject report;
#ifdef _WIN32
        report[QStringLiteral("backend")] = QStringLiteral("Pinned strict FFmpeg 9.0.2 full inspection; Windows Media Foundation cover");
#else
        report[QStringLiteral("backend")] = QStringLiteral("Pinned FFmpeg 9.0.2 MOV + H.264/HEVC/AAC software decoders");
#endif
        report[QStringLiteral("suite")] = hevc ? QStringLiteral("HEVC Main/Main10") : QStringLiteral("H.264/AAC and malformed MOV metadata");
        report[QStringLiteral("cases")] = outcomes;
        report[QStringLiteral("fixtureFailures")] = static_cast<int>(failures);
        report[QStringLiteral("auxiliaryChecks")] = QStringLiteral("not completed");
        report[QStringLiteral("scope")] = QStringLiteral("Real encoded local files, all tracks, covers, malformed packets/container tails, budgets, cancellation and handle release. No capture hardware or user archive is touched.");
        bool saved{};
        const auto saveReport = [&] {
            QFile evidence(run.path() + QStringLiteral("/video-inspection.json"));
            const auto json = QJsonDocument(report).toJson();
            const auto flags = QIODevice::WriteOnly | (saved ? QIODevice::Truncate : QIODevice::NewOnly);
            require(evidence.open(flags) && evidence.write(json) == json.size() && evidence.flush(),
                "Write actual inspection results, including unexpected accepts");
            saved = true;
        };
        // Preserve every negative-case outcome even if a subsequent budget,
        // Unicode or cancellation assertion fails on the actual platform.
        saveReport();

        // Use a private Unicode-named copy for handle lifetime and budget tests;
        // keep the shared cross-job fixtures immutable.
        const auto local = run.path() + QStringLiteral("/视频检查-中文.mp4");
        require(QFile::copy(root + QStringLiteral("/valid.mp4"), local), "Create an owned Unicode-path fixture copy");
        const auto unicode = inspectVideo(local);
        require(!unicode.failure && unicode.frames == 251, "Complete inspection must accept a native Unicode path");
        const auto unscaled = videoThumbnail(local, 1024);
        require(!unscaled.failure && unscaled.image.size() == QSize(352, 288), "History thumbnails must not upscale existing video");
        Limits limits;
        limits.maximumFileBytes = 8;
        require(inspectVideo(local, limits).failure.code == Error::Budget, "File bytes must obey the configured budget");
        limits = {};
        limits.maximumImageBytes = 256 * 1024;
        require(inspectVideo(local, limits).failure.code == Error::Budget, "Declared decoded pixels must obey their budget");
        require(videoThumbnail(local, 0).failure.code == Error::Budget, "Zero-sized cover request must fail explicitly");
        std::stop_source before;
        before.request_stop();
        require(inspectVideo(local, {}, before.get_token()).failure.code == Error::Cancelled &&
            videoThumbnail(local, 128, {}, before.get_token()).failure.code == Error::Cancelled,
            "Pre-cancelled worker operations must not report success");

        std::stop_source during;
        std::atomic<bool> entered{};
        std::jthread canceller([&] {
            while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            during.request_stop();
        });
        entered.store(true, std::memory_order_release);
        const auto stopped = inspectVideo(local, {}, during.get_token());
        canceller.join();
        require(stopped.failure.code == Error::Cancelled, "A stop requested during real inspection must be propagated");
        const auto renamed = local + QStringLiteral(".closed");
        require(QFile::rename(local, renamed) && QFile::remove(renamed), "Every successful/failed/cancelled worker must release its owned file handles");

        report[QStringLiteral("auxiliaryChecks")] = QStringLiteral("passed");
        saveReport();
        require(failures == 0, "Some real malformed fixtures were accepted, or valid files regressed; inspection is not an archive gate yet");
        std::cout << "PASS real video inspection, covers, all-track rejection, budgets, worker cancellation and handle release\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL video inspection: " << error.what() << '\n';
        return 1;
    }
}
