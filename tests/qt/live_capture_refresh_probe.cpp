#include "services/capture/LiveCaptureSource.h"
#include "services/media/FrameNormalizer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <set>
#include <sys/stat.h>
#include <utility>

using namespace HuhuCapture;
namespace {
constexpr std::array<std::array<int, 3>, 4> colors{{
    {210, 35, 45}, {30, 180, 90}, {45, 90, 220}, {230, 230, 230}}};
using Socket = std::pair<std::uint64_t, std::uint64_t>;
using Sockets = std::set<Socket>;
void record(const QJsonObject& value)
{ QTextStream(stdout) << QJsonDocument(value).toJson(QJsonDocument::Compact) << Qt::endl; }
Sockets sockets()
{
    Sockets result;
    for (const auto& entry : QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        bool valid{}; const int descriptor = entry.toInt(&valid);
        struct stat info{};
        if (valid && ::fstat(descriptor, &info) == 0 && S_ISSOCK(info.st_mode))
            result.emplace(static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino));
    }
    return result;
}
Sockets difference(const Sockets& a, const Sockets& b)
{
    Sockets result;
    for (const auto& entry : a) if (!b.contains(entry)) result.insert(entry);
    return result;
}
QJsonArray socketEvidence(const Sockets& values)
{
    QJsonArray result;
    for (const auto& [device, inode] : values)
        result.append(QString::number(device) + QChar(u':') + QString::number(inode));
    return result;
}
class Pattern final : public QWidget {
public:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        for (int column = 0; column < 4; ++column) {
            const auto& color = colors[static_cast<std::size_t>(column)];
            painter.fillRect(QRect(column * width() / 4, 0, (column + 1) * width() / 4 - column * width() / 4, height()),
                QColor(color[0], color[1], color[2]));
        }
    }
};
class Controls final : public QWidget {
public:
    explicit Controls(QWidget* parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint)
    { resize(176, 96); setWindowTitle(QStringLiteral("huhucapture visible refresh witness")); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(255, 0, 255));
        painter.fillRect(rect().adjusted(16, 16, -16, -16), QColor(5, 5, 5));
    }
};
quint64 wrongPixels(const QImage& image)
{
    quint64 wrong{};
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const auto& expected = colors[static_cast<std::size_t>(std::min(3, x * 4 / image.width()))];
        const auto value = image.pixel(x, y);
        wrong += std::abs(qRed(value) - expected[0]) > 3 || std::abs(qGreen(value) - expected[1]) > 3 ||
            std::abs(qBlue(value) - expected[2]) > 3 || qAlpha(value) != 255;
    }
    return wrong;
}
quint64 witnessPixels(const QImage& image)
{
    quint64 count{};
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const auto value = image.pixel(x, y);
        count += qRed(value) >= 250 && qGreen(value) <= 3 && qBlue(value) >= 250 && qAlpha(value) == 255;
    }
    return count;
}
struct Result {
    QImage first;
    quint64 wrong{}, discarded{};
    Core::FrameSourceTerminal terminal;
    bool firstLease{}, identity{};
    QString failure;
};
class Probe final : public QObject {
public:
    Capture::LiveCaptureSource source;
    Pattern background;
    Controls controls{&background};
    QThreadPool pool;
    QFutureWatcher<Result> consumer;
    QTimer heartbeat;
    Sockets baseline, previewSockets, refreshedSockets;
    QString path, failure;
    quint64 request{}, refresh{}, sourceId{}, generation{}, witness{};
    QSize extent;
    unsigned preparedCount{}, readyCount{}, beats{};
    bool oldSocketsClosed{}, claimed{}, consumerReturned{}, stopped{}, expired{};
    Result result;

    explicit Probe(QString output) : path(std::move(output))
    {
        pool.setMaxThreadCount(1);
        background.setWindowTitle(QStringLiteral("huhucapture real refresh background"));
        background.showFullScreen(); controls.show();
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [this] { ++beats; }); heartbeat.start();
        connect(&source, &Capture::LiveCaptureSource::initialized, this,
            [this](const Capture::Capability& capability, const Capture::Failure& error) {
                if (error || !capability.canRequest()) { fail(error.detail); return; }
                baseline = sockets();
                request = source.request(background.windowHandle());
                if (!request) fail(QStringLiteral("The original native authorization was not admitted"));
            });
        connect(&source, &Capture::LiveCaptureSource::prepared, this,
            [this](quint64 token, const Capture::Snapshot& snapshot, const Capture::Failure& error) {
                ++preparedCount;
                if (token != request || preparedCount != 1 || error || snapshot.image.isNull()) { fail(error.detail); return; }
                sourceId = snapshot.sourceId; generation = snapshot.generation; extent = snapshot.image.size();
                witness = witnessPixels(snapshot.image);
                previewSockets = difference(sockets(), baseline);
                const bool saved = snapshot.image.save(path + QStringLiteral(".before.png"));
                auto forbidden = source.claim(sourceId, generation, {0, 0, 1, 1});
                const bool staleAdmitted = bool(forbidden);
                if (forbidden) {
                    forbidden->firstFrame = {}; forbidden->stop.request_stop(); forbidden.reset();
                    source.consumerFinished(sourceId, generation);
                }
                if (witness < 1000 || wrongPixels(snapshot.image) < witness || previewSockets.empty() || !saved ||
                    snapshot.coordinates != Capture::CoordinateSpace::AuthorizedSourcePixels || snapshot.desktopOrigin ||
                    staleAdmitted) {
                    fail(QStringLiteral("The first real frame did not prove visible controls and closed pre-refresh admission")); return;
                }
                record({{"event", "witnessCaptured"}, {"sourceId", QString::number(sourceId)},
                    {"generation", QString::number(generation)}, {"visibleControlPixels", qint64(witness)},
                    {"previewNativeSockets", socketEvidence(previewSockets)}});
                // A real separate native window is unmapped. No sleep, dropped
                // frame count, fabricated pixel or clock-domain inference gates
                // the following replacement connection.
                controls.hide();
                if (controls.isVisible() || (controls.windowHandle() && controls.windowHandle()->isVisible())) {
                    fail(QStringLiteral("The actual witness window did not become hidden")); return;
                }
                refresh = source.refreshForCapture(sourceId, generation);
                if (!refresh) fail(QStringLiteral("The same authorized source did not admit refresh"));
            });
        connect(&source, &Capture::LiveCaptureSource::captureReady, this,
            [this](quint64 token, quint64 id, quint64 version, const Capture::Failure& error) {
                ++readyCount;
                if (token != refresh || id != sourceId || version != generation || readyCount != 1 || error) { fail(error.detail); return; }
                const auto current = sockets();
                oldSocketsClosed = true;
                for (const auto& socket : previewSockets) oldSocketsClosed &= !current.contains(socket);
                refreshedSockets = difference(current, baseline);
                if (!oldSocketsClosed || refreshedSockets.empty()) {
                    fail(QStringLiteral("The old native producer retained a socket across the replacement handoff")); return;
                }
                auto handle = source.claim(sourceId, generation,
                    {0, 0, static_cast<unsigned>(extent.width()), static_cast<unsigned>(extent.height())});
                auto duplicate = source.claim(sourceId, generation, {0, 0, 1, 1});
                if (!handle || !handle->firstFrame || duplicate) {
                    if (duplicate) { duplicate->firstFrame = {}; duplicate->stop.request_stop(); duplicate.reset(); }
                    if (handle) { handle->firstFrame = {}; handle->stop.request_stop(); handle.reset(); }
                    source.consumerFinished(sourceId, generation);
                    fail(QStringLiteral("Exactly one actual replacement first frame was not transferred")); return;
                }
                claimed = true;
                consumer.setFuture(QtConcurrent::run(&pool, [this, handle = std::move(*handle)]() mutable {
                    return consume(std::move(handle));
                }));
            });
        connect(&consumer, &QFutureWatcher<Result>::finished, this, [this] {
            consumerReturned = true;
            try { result = consumer.result(); }
            catch (const std::exception& error) { result.failure = QString::fromUtf8(error.what()); }
            if (failure.isEmpty()) failure = result.failure;
            record({{"event", "streamReady"}, {"mode", "capture"}, {"sourceId", QString::number(sourceId)},
                {"actualFirstFrameWrongPixels", qint64(result.wrong)}, {"oldProducerSocketsClosed", oldSocketsClosed},
                {"replacementNativeSockets", socketEvidence(refreshedSockets)}});
            source.consumerFinished(sourceId, generation);
        });
        connect(&source, &Capture::LiveCaptureSource::stopped, this,
            [this](quint64 token, const Capture::Failure& error) {
                stopped = token == request;
                if (error && failure.isEmpty()) failure = error.detail;
                source.shutdown();
            });
        connect(&source, &Capture::LiveCaptureSource::shutdownFinished, this, [this] { finish(); });
        QTimer::singleShot(45000, this, [this] { expired = true; fail(QStringLiteral("The actual refresh and drain exceeded its independent deadline")); });
        QTimer::singleShot(0, &source, &Capture::LiveCaptureSource::initialize);
    }
    Result consume(Core::LiveSourceConsumer handle)
    {
        Result output;
        try {
            // Deliberately inspect the actual first lease. takeLatest or dropping
            // an arbitrary number of frames could conceal the exact regression.
            output.firstLease = bool(handle.firstFrame);
            output.identity = handle.firstFrame && handle.firstFrame.metadata().sourceId == handle.sourceId &&
                handle.sourceId == sourceId && handle.generation == generation;
            Media::FrameNormalizer normalizer({1, 128u * 1024u * 1024u, 16384});
            auto normalized = normalizer.normalize(handle.firstFrame, handle.selection);
            if (!normalized.frame) output.failure = QStringLiteral("The actual replacement first frame did not normalize");
            else {
                const auto& frame = normalized.frame;
                const QImage view(frame.row(0, 0).data(), frame.format().width, frame.format().height,
                    frame.format().planes[0].stride, QImage::Format_ARGB32);
                output.first = view.copy();
                output.wrong = wrongPixels(output.first);
                if (output.first.size() != extent || output.wrong)
                    output.failure = QStringLiteral("The first refreshed frame still contains controls or incorrect background pixels");
            }
            normalized.frame = {}; handle.firstFrame = {};
        } catch (const std::exception& error) { output.failure = QString::fromUtf8(error.what()); handle.firstFrame = {}; }
        handle.stop.request_stop();
        for (;;) {
            auto next = handle.frames->waitNext();
            if (next.terminal) { output.terminal = std::move(*next.terminal); break; }
            if (next.frame) ++output.discarded; // after the first-frame assertion, only actual shutdown draining
        }
        return output;
    }
    void fail(QString reason)
    {
        if (failure.isEmpty()) failure = reason.isEmpty() ? QStringLiteral("The actual live source failed") : std::move(reason);
        if (refresh) source.cancelRefresh(refresh);
        if (request) source.cancel(request);
        source.shutdown();
    }
    void finish()
    {
        const auto current = sockets();
        bool noProducerSockets = true;
        for (const auto& socket : previewSockets) noProducerSockets &= !current.contains(socket);
        for (const auto& socket : refreshedSockets) noProducerSockets &= !current.contains(socket);
        const bool saved = !result.first.isNull() && result.first.save(path);
        const bool passed = failure.isEmpty() && !expired && preparedCount == 1 && readyCount == 1 &&
            witness >= 1000 && oldSocketsClosed && noProducerSockets && claimed && consumerReturned && stopped &&
            result.firstLease && result.identity && !result.wrong && saved && beats > 2 &&
            result.terminal.reason == Core::FrameSourceEnd::Stopped && !source.isCurrent(sourceId, generation);
        record({{"event", "result"}, {"mode", "capture"}, {"passed", passed}, {"failure", failure},
            {"sameAuthorizationIdentity", result.identity}, {"preparations", int(preparedCount)}, {"refreshTerminals", int(readyCount)},
            {"initialVisibleControlPixels", qint64(witness)}, {"firstFrameWrongPixels", qint64(result.wrong)},
            {"actualFirstLeaseChecked", result.firstLease}, {"oldProducerSocketsClosedBeforeClaim", oldSocketsClosed},
            {"producerSocketsReleasedAfterDrain", noProducerSockets}, {"consumerReturned", consumerReturned},
            {"shutdownDrained", stopped}, {"qtHeartbeat", int(beats)}, {"pngSaved", saved},
            {"scope", "Actual selected compositor only; no universal presentation fence, physical display or GNOME/Plasma claim"}});
        QCoreApplication::exit(passed ? 0 : 1);
    }
};
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    app.setApplicationName(QStringLiteral("huhucapture-live-refresh-probe"));
    app.setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture"));
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"mode", "Existing real-compositor wrapper mode (capture only)", "mode", "capture"});
    parser.addOption({"evidence", "Actual first refreshed frame PNG", "path"}); parser.process(app);
    if (parser.value("mode") != QStringLiteral("capture") || parser.value("evidence").isEmpty() ||
        !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) return 2;
    Probe probe(parser.value("evidence"));
    return app.exec();
}
