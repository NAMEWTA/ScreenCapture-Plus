#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "platform/windows/NativeMp4Recording.h"
#include "recording/RecordingSession.h"
#include "services/media/VideoInspection.h"
#include "recording_timeline.h"

#include <QApplication>
#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace recording = HuhuCapture::Recording;
namespace media = HuhuCapture::Media;
namespace core = HuhuCapture::Core;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void settle()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void waitFor(Predicate predicate, const char* message)
{
    const auto deadline = core::MediaClock::now() + 30s;
    while (!predicate()) {
        require(core::MediaClock::now() < deadline, message);
        settle(); std::this_thread::sleep_for(1ms);
    }
}
template<class Work> auto worker(Work work)
{
    auto result = QtConcurrent::run(std::move(work));
    waitFor([&] { return result.isFinished(); }, "Real recording inspection worker timed out");
    return result.takeResult();
}

// This window is painted by the real Windows desktop. The only pixels passed
// to the encoder come from the retained DXGI/GDI backend, never QWidget::grab.
class PatternWindow final : public QWidget {
public:
    PatternWindow() : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
        setWindowTitle(QStringLiteral("huhucapture native recording validation"));
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFixedSize(256, 192);
        timer_.setInterval(30);
        connect(&timer_, &QTimer::timeout, this, [this] { ++ticks_; update(); });
        timer_.start();
    }
    unsigned painted() const noexcept { return painted_; }
    static std::array<QColor, 4> colors()
    { return {QColor(220, 30, 40), QColor(30, 210, 45), QColor(25, 50, 220), QColor(170, 170, 170)}; }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const auto palette = colors();
        painter.fillRect(QRect(0, 0, width() / 2, height() / 2), palette[0]);
        painter.fillRect(QRect(width() / 2, 0, width() - width() / 2, height() / 2), palette[1]);
        painter.fillRect(QRect(0, height() / 2, width() / 2, height() - height() / 2), palette[2]);
        painter.fillRect(QRect(width() / 2, height() / 2, width() - width() / 2, height() - height() / 2), palette[3]);
        painter.fillRect(QRect(width() / 2 - 18 + static_cast<int>(ticks_ % 32), height() / 2 - 8, 8, 16), Qt::white);
        ++painted_;
    }
private:
    QTimer timer_;
    unsigned ticks_{}, painted_{};
};

recording::PixelRegion physicalClient(QWidget& widget)
{
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    require(previous != nullptr, "Set a real physical-pixel scope for the test window");
    struct Restore { DPI_AWARENESS_CONTEXT context; ~Restore() { SetThreadDpiAwarenessContext(context); } } restore{previous};
    const auto window = reinterpret_cast<HWND>(widget.winId());
    RECT client{}; POINT start{};
    require(GetClientRect(window, &client) && ClientToScreen(window, &start), "Read the actual visible native client rectangle");
    return {static_cast<std::int32_t>(start.x), static_cast<std::int32_t>(start.y),
        static_cast<std::int32_t>(client.right - client.left), static_cast<std::int32_t>(client.bottom - client.top)};
}
std::shared_ptr<media::MediaArchiveService> archiveAt(const QString& path)
{
    return std::make_shared<media::MediaArchiveService>(path,
        [](const QString& file, media::Kind kind, std::stop_token stop) -> media::Inspection {
            if (kind == media::Kind::Mp4) return media::inspectVideo(file, {}, stop);
            return {{media::Error::Unsupported, QStringLiteral("This actual probe covers MP4 only")}, kind, {}, 0};
        });
}
QJsonObject resultJson(const recording::SessionResult& result)
{
    QJsonObject item;
    item[QStringLiteral("token")] = static_cast<qint64>(result.token);
    item[QStringLiteral("state")] = static_cast<int>(result.state);
    item[QStringLiteral("intent")] = static_cast<int>(result.intent);
    item[QStringLiteral("framesSubmitted")] = static_cast<qint64>(result.backend.media.framesWritten);
    if (result.backend.media.sinkStatistics) {
        const auto& statistics = *result.backend.media.sinkStatistics;
        QJsonObject sink;
        sink[QStringLiteral("queryStatus")] = static_cast<qint64>(statistics.queryStatus);
        sink[QStringLiteral("samplesReceived")] = static_cast<qint64>(statistics.received);
        sink[QStringLiteral("samplesEncoded")] = static_cast<qint64>(statistics.encoded);
        sink[QStringLiteral("samplesProcessed")] = static_cast<qint64>(statistics.processed);
        sink[QStringLiteral("lastReceived100ns")] = static_cast<qint64>(statistics.lastReceived);
        sink[QStringLiteral("lastEncoded100ns")] = static_cast<qint64>(statistics.lastEncoded);
        sink[QStringLiteral("lastProcessed100ns")] = static_cast<qint64>(statistics.lastProcessed);
        sink[QStringLiteral("lastSubmitted100ns")] = static_cast<qint64>(statistics.lastSubmitted);
        sink[QStringLiteral("lastSubmittedDuration100ns")] = static_cast<qint64>(statistics.lastSubmittedDuration);
        sink[QStringLiteral("queuedBytes")] = static_cast<qint64>(statistics.queuedBytes);
        item[QStringLiteral("sinkStatistics")] = sink;
    }
    item[QStringLiteral("finalized")] = result.backend.media.finalized;
    item[QStringLiteral("stopRequested")] = result.backend.stopRequested;
    item[QStringLiteral("durationLimitReached")] = result.backend.durationLimitReached;
    item[QStringLiteral("elapsed100ns")] = static_cast<qint64>(result.backend.elapsed.count());
    item[QStringLiteral("failure")] = result.failure.detail;
    item[QStringLiteral("archivePath")] = result.archive.path;
    item[QStringLiteral("archiveDurable")] = result.archive.durable();
    item[QStringLiteral("hasArchivedEntry")] = result.archivedEntry.has_value();
    if (result.archivedEntry) {
        item[QStringLiteral("archiveEntryName")] = result.archivedEntry->name;
        item[QStringLiteral("archiveEntryBytes")] = static_cast<qint64>(result.archivedEntry->snapshot.bytes);
    }
    item[QStringLiteral("recoveryPath")] = result.recoveryPath;
    if (result.backend.media.firstError) {
        item[QStringLiteral("nativeStage")] = static_cast<int>(result.backend.media.firstError->stage);
        item[QStringLiteral("nativeError")] = static_cast<qint64>(result.backend.media.firstError->code);
    }
    if (result.backend.prepared) {
        const auto& ready = *result.backend.prepared;
        item[QStringLiteral("source")] = static_cast<int>(ready.source);
        item[QStringLiteral("codec")] = static_cast<int>(ready.codec);
        item[QStringLiteral("region")] = QJsonArray{ready.region.x, ready.region.y, ready.region.width, ready.region.height};
        item[QStringLiteral("systemAudio")] = ready.systemAudio;
        item[QStringLiteral("microphone")] = ready.microphone;
    }
    return item;
}
QString digest(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Read the real finalized media for its digest");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Hash every actual media byte");
    return QString::fromLatin1(hash.result().toHex());
}
void checkCover(const QImage& image)
{
    require(!image.isNull() && image.width() >= 32 && image.height() >= 24,
        "Decode actual desktop recording pixels for cover verification");
    const auto colors = PatternWindow::colors();
    for (unsigned index = 0; index != colors.size(); ++index) {
        const int x = (index % 2 ? 3 : 1) * image.width() / 4;
        const int y = (index / 2 ? 3 : 1) * image.height() / 4;
        const auto actual = image.pixelColor(x, y);
        // Encoding and RGB/YUV conversion are lossy; these far-from-boundary
        // color patches still distinguish flips, wrong crop and blank capture.
        require(std::abs(actual.red() - colors[index].red()) <= 45 &&
            std::abs(actual.green() - colors[index].green()) <= 45 &&
            std::abs(actual.blue() - colors[index].blue()) <= 45,
            "Real decoded quadrants do not match the visible desktop orientation and crop");
    }
}
void inspectRecording(const QString& path, const recording::BackendResult& recorded,
    const QString& coverPath, QJsonObject& evidence)
{
    const auto inspection = worker([path] { return media::inspectVideo(path); });
    evidence[QStringLiteral("inspectionFailure")] = inspection.failure.detail;
    evidence[QStringLiteral("framesDecoded")] = static_cast<qint64>(inspection.frames);
    evidence[QStringLiteral("sha256")] = digest(path);
    require(!inspection.failure && recorded.prepared &&
        inspection.dimensions == QSize(recorded.prepared->region.width, recorded.prepared->region.height) &&
        inspection.frames >= 2,
        "The actual MF recording must retain complete geometry and pass strict all-track inspection");
    const auto timeline = worker([path, recorded] {
        return HuhuCapture::Tests::verifyNativeRecordingTimeline(path, recorded.media, 30,
            QSize(recorded.prepared->region.width, recorded.prepared->region.height));
    });
    evidence[QStringLiteral("timeline")] = timeline;
    require(timeline.value(QStringLiteral("framesDecoded")).toInteger() == static_cast<qint64>(inspection.frames),
        "The independent complete timeline decoder and archive inspector must agree on every output frame");
    const auto cover = worker([path] { return media::videoThumbnail(path, 256); });
    evidence[QStringLiteral("coverFailure")] = cover.failure.detail;
    evidence[QStringLiteral("coverWidth")] = cover.image.width();
    evidence[QStringLiteral("coverHeight")] = cover.image.height();
    require(!cover.failure, "Generate a real native/software-decoded recording cover");
    require(cover.image.save(coverPath, "PNG"), "Preserve actual decoded cover evidence");
    checkCover(cover.image);
}
void assertEntryCount(const media::MediaArchiveService& archive, std::size_t count)
{
    const auto files = archive.list();
    require(!files.failure && files.entries.size() == count, "Only the intended finalized recordings belong to history");
}
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QJsonObject report;
    QJsonArray cases;
    QString reportPath;
    const auto saveReport = [&] {
        if (reportPath.isEmpty()) return;
        report[QStringLiteral("cases")] = cases;
        QFile file(reportPath);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Open private recording evidence JSON");
        const auto bytes = QJsonDocument(report).toJson();
        require(file.write(bytes) == bytes.size() && file.flush(), "Persist actual recording probe outcomes");
    };
    try {
        require(argc == 2, "Provide an evidence parent directory for actual native recording");
        require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "This probe requires the real Windows platform, not an offscreen substitute");
        const auto parent = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
        require(QDir().mkpath(parent), "Create recording evidence parent directory");
        QTemporaryDir run(parent + QStringLiteral("/run-XXXXXX"));
        require(run.isValid(), "Claim an exclusively owned recording evidence directory");
        run.setAutoRemove(false);
        reportPath = run.path() + QStringLiteral("/native-recording.json");
        report[QStringLiteral("schema")] = 1;
        report[QStringLiteral("status")] = QStringLiteral("running");
        report[QStringLiteral("scope")] = QStringLiteral("Actual visible Windows desktop -> retained DXGI/GDI -> MF MP4 -> strict all-track FFmpeg inspection and decoded cover. Audio disabled. Actual capture source and codec recorded per case. No physical multi-monitor, mixed-DPI, HDR, audio, sustained-performance or installation acceptance is claimed.");
        saveReport();

        PatternWindow pattern;
        QObject observation;
        auto* screen = QGuiApplication::primaryScreen();
        require(screen != nullptr, "A real desktop screen is required");
        pattern.move(screen->availableGeometry().center() - QPoint(pattern.width() / 2, pattern.height() / 2));
        pattern.show(); pattern.raise();
        waitFor([&] { return pattern.windowHandle() && pattern.windowHandle()->isExposed() && pattern.painted() >= 3; },
            "The actual pattern window was not exposed and painted by the desktop");
        recording::BackendRequest request;
        request.region = physicalClient(pattern);
        request.systemAudio = false; request.microphone = false; request.cursor = false;
        request.maximumDuration = 8s;
        const auto aligned = recording::alignedMp4Region(request.region);
        require(aligned.has_value(), "The actual client area must yield a valid aligned recording region");
        auto archive = archiveAt(run.path() + QStringLiteral("/archive"));
        recording::RecordingSession session(archive, HuhuCapture::Platform::Windows::runNativeMp4);
        QTimer heartbeat;
        heartbeat.setInterval(5);
        std::uint64_t beats{};
        auto previousBeat = core::MediaClock::now();
        std::chrono::milliseconds longestGap{};
        QObject::connect(&heartbeat, &QTimer::timeout, &observation, [&] {
            const auto now = core::MediaClock::now();
            longestGap = (std::max)(longestGap, std::chrono::duration_cast<std::chrono::milliseconds>(now - previousBeat));
            previousBeat = now; ++beats;
        });
        heartbeat.start();
        std::optional<recording::SessionResult> completed;
        std::optional<recording::PreparedRecording> prepared;
        recording::StopIntent selected{recording::StopIntent::Save};
        bool closeInstead{}, stopAccepted{}, repeatedStopRejected{}, concurrentStartRejected{};
        unsigned completions{}, shutdowns{};
        QObject::connect(&session, &recording::RecordingSession::finished, &observation,
            [&](const auto& result) { completed = result; ++completions; });
        QObject::connect(&session, &recording::RecordingSession::shutdownFinished, &observation, [&] { ++shutdowns; });
        QObject::connect(&session, &recording::RecordingSession::prepared, &observation,
            [&](quint64 token, const auto& ready) {
                prepared = ready;
                concurrentStartRejected = session.start(request) == 0;
                QTimer::singleShot(750, &session, [&, token] {
                    if (session.token() != token || !session.busy()) return;
                    if (closeInstead) session.shutdown();
                    else {
                        stopAccepted = session.requestStop(selected);
                        repeatedStopRejected = !session.requestStop(recording::StopIntent::Discard);
                    }
                });
            });
        std::size_t expectedEntries{};
        for (const auto intent : {recording::StopIntent::Save, recording::StopIntent::Copy, recording::StopIntent::Discard}) {
            selected = intent; completed.reset(); prepared.reset(); stopAccepted = false; repeatedStopRejected = false;
            concurrentStartRejected = false;
            const auto before = completions;
            const auto token = session.start(request);
            require(token != 0, "Start the actual native recording worker");
            waitFor([&] { return completed.has_value(); }, "Actual native recording did not finish");
            auto evidence = resultJson(*completed);
            evidence[QStringLiteral("case")] = intent == recording::StopIntent::Save ? QStringLiteral("save") :
                intent == recording::StopIntent::Copy ? QStringLiteral("copy-intent") : QStringLiteral("discard");
            cases.append(evidence); saveReport();
            require(prepared && prepared->region == *aligned && !prepared->systemAudio && !prepared->microphone &&
                stopAccepted && repeatedStopRejected && concurrentStartRejected && completions == before + 1 &&
                completed->token == token && completed->intent == intent && !session.busy() &&
                completed->backend.media.canArchive() && completed->backend.stopRequested,
                "Prepared, one admitted session, first stop intent and finalized real media must agree");
            if (intent == recording::StopIntent::Discard) {
                require(completed->state == recording::SessionState::Discarded && completed->archive.path.isEmpty() &&
                    !completed->archivedEntry && completed->recoveryPath.isEmpty(),
                    "Explicit discard removes owned staging without publishing media");
            } else {
                require(completed->state == recording::SessionState::Completed && completed->archive.durable() &&
                    completed->archivedEntry && completed->archivedEntry->path == completed->archive.path &&
                    completed->archivedEntry->name == QFileInfo(completed->archive.path).fileName() &&
                    completed->archivedEntry->kind == media::Kind::Mp4 && completed->archivedEntry->snapshot.bytes,
                    "Save/copy intent must first publish a fully inspected durable recording");
                const auto verifiedEntry = *completed->archivedEntry;
                require(worker([archive, verifiedEntry] {
                    const auto leased = archive->lease(verifiedEntry);
                    return !leased.failure && leased.lease && leased.lease->unchanged();
                }), "The worker-provided archive identity must acquire a real unchanged media lease");
                ++expectedEntries;
                try {
                    inspectRecording(completed->archive.path, completed->backend,
                        run.path() + QStringLiteral("/cover-%1.png").arg(token), evidence);
                } catch (...) { cases[cases.size() - 1] = evidence; saveReport(); throw; }
                cases[cases.size() - 1] = evidence; saveReport();
            }
            assertEntryCount(*archive, expectedEntries);
            require(QDir(archive->directory()).entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden).isEmpty(),
                "Clean save/copy/discard must leave no owned staging residue");
        }

        // Normal application shutdown stops, finalizes and retains the original
        // recording. It never silently turns closing the service into discard.
        closeInstead = true; completed.reset(); prepared.reset();
        require(session.start(request) != 0, "Start actual recording before graceful shutdown");
        waitFor([&] { return completed.has_value() && shutdowns == 1; }, "Graceful recording shutdown did not complete asynchronously");
        auto stopped = resultJson(*completed);
        stopped[QStringLiteral("case")] = QStringLiteral("graceful-shutdown");
        cases.append(stopped); saveReport();
        require(completed->state == recording::SessionState::RecoverableFailure &&
            completed->failure.code == media::Error::Cancelled && !completed->recoveryPath.isEmpty() &&
            completed->archive.path.isEmpty() && !completed->archivedEntry && completed->backend.media.canArchive(),
            "Shutdown must retain actual finalized staging instead of claiming it was saved or deleting it");
        try {
            inspectRecording(completed->recoveryPath, completed->backend,
                run.path() + QStringLiteral("/shutdown-cover.png"), stopped);
        } catch (...) { cases[cases.size() - 1] = stopped; saveReport(); throw; }
        cases[cases.size() - 1] = stopped; saveReport();
        assertEntryCount(*archive, expectedEntries);
        require(!session.start(request), "A closed service cannot start a new recording");

        // Abrupt QObject owner destruction also leaves the worker owning its
        // real handles and staging. Observe the real worker return separately;
        // this wrapper only records evidence, it delegates all media operations.
        auto orphanArchive = archiveAt(run.path() + QStringLiteral("/owner-destroyed"));
        auto nativeResult = std::make_shared<recording::BackendResult>();
        auto returned = std::make_shared<std::atomic<bool>>(false);
        auto orphanOwner = std::make_unique<recording::RecordingSession>(orphanArchive,
            [nativeResult, returned](const auto& arguments, auto writer, const auto& events, auto stop) {
                auto result = HuhuCapture::Platform::Windows::runNativeMp4(arguments, std::move(writer), events, stop);
                *nativeResult = result; returned->store(true, std::memory_order_release); return result;
            });
        QPointer<recording::RecordingSession> orphan = orphanOwner.get();
        bool ownerDeleted{};
        std::chrono::milliseconds deletionDuration{};
        QObject orphanObservation;
        QObject::connect(orphan.data(), &recording::RecordingSession::prepared, &orphanObservation,
            [&](quint64, const auto&) {
                QTimer::singleShot(750, &orphanObservation, [&] {
                    const auto started = core::MediaClock::now();
                    orphanOwner.reset();
                    deletionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(core::MediaClock::now() - started);
                    ownerDeleted = true;
                });
            });
        require(orphan->start(request) != 0, "Start real worker before deleting its QObject owner");
        waitFor([&] { return ownerDeleted && returned->load(std::memory_order_acquire) &&
            QThreadPool::globalInstance()->activeThreadCount() == 0; }, "Destroyed owner did not leave a terminating independent worker");
        require(orphan.isNull() && deletionDuration < 1s && nativeResult->media.canArchive(),
            "QObject destruction must return without joining the native recorder or losing its result");
        const auto files = QDir(orphanArchive->directory()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        require(files.size() == 1, "Destroyed-owner recording must retain exactly its owned unpublished file");
        const auto recovery = QDir(orphanArchive->directory()).filePath(files.front());
        QJsonObject destroyed;
        destroyed[QStringLiteral("case")] = QStringLiteral("owner-destroyed");
        destroyed[QStringLiteral("recoveryPath")] = recovery;
        destroyed[QStringLiteral("destructorMilliseconds")] = static_cast<qint64>(deletionDuration.count());
        destroyed[QStringLiteral("source")] = nativeResult->prepared ? static_cast<int>(nativeResult->prepared->source) : -1;
        destroyed[QStringLiteral("framesSubmitted")] = static_cast<qint64>(nativeResult->media.framesWritten);
        cases.append(destroyed); saveReport();
        try {
            inspectRecording(recovery, *nativeResult, run.path() + QStringLiteral("/destroyed-owner-cover.png"), destroyed);
        } catch (...) { cases[cases.size() - 1] = destroyed; saveReport(); throw; }
        cases[cases.size() - 1] = destroyed; saveReport();
        assertEntryCount(*orphanArchive, 0);

        require(beats >= 20 && longestGap < 1s, "The Qt owner event loop must continue processing while native workers capture and finish");
        report[QStringLiteral("heartbeatCount")] = static_cast<qint64>(beats);
        report[QStringLiteral("longestHeartbeatGapMilliseconds")] = static_cast<qint64>(longestGap.count());
        report[QStringLiteral("status")] = QStringLiteral("passed");
        saveReport();
        std::cout << "PASS actual native recording, finalization, strict frame counts, crop/orientation, durable archive, discard and asynchronous owner shutdown\n";
        std::cout << "Evidence: " << run.path().toStdString() << '\n';
        return 0;
    } catch (const std::exception& error) {
        report[QStringLiteral("status")] = QStringLiteral("failed");
        report[QStringLiteral("failure")] = QString::fromUtf8(error.what());
        try { saveReport(); } catch (...) { }
        std::cerr << "FAIL actual native recording: " << error.what() << '\n';
        return 1;
    }
}
