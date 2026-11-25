#include "services/capture/LiveCaptureSource.h"
#include "services/media/FrameNormalizer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <utility>

using namespace HuhuCapture;
namespace {
constexpr std::array<std::array<int, 3>, 4> colors{{
    {210, 35, 45}, {30, 180, 90}, {45, 90, 220}, {230, 230, 230}}};
void record(QJsonObject value)
{ QTextStream(stdout) << QJsonDocument(value).toJson(QJsonDocument::Compact) << Qt::endl; }
class Pattern final : public QWidget {
public:
    int phase{};
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        for (int column = 0; column < 4; ++column) {
            const auto& color = colors[static_cast<std::size_t>((column + phase) % 4)];
            painter.fillRect(QRect(column * width() / 4, 0, (column + 1) * width() / 4 - column * width() / 4, height()),
                QColor(color[0], color[1], color[2]));
        }
    }
};
int inspect(const QImage& image)
{
    if (image.width() < 8 || image.height() < 1) return -1;
    for (int phase = 0; phase < 4; ++phase) {
        bool matched = true;
        for (int column = 0; column < 4; ++column) {
            const auto pixel = image.pixel((2 * column + 1) * image.width() / 8, image.height() / 2);
            const auto& color = colors[static_cast<std::size_t>((column + phase) % 4)];
            matched &= std::abs(qRed(pixel) - color[0]) <= 3 && std::abs(qGreen(pixel) - color[1]) <= 3 &&
                std::abs(qBlue(pixel) - color[2]) <= 3 && qAlpha(pixel) == 255;
        }
        if (matched) return phase;
    }
    return -1;
}
QJsonArray samplePixels(const QImage& image)
{
    QJsonArray pixels;
    if (image.width() < 8 || image.height() < 1) return pixels;
    for (int column = 0; column < 4; ++column)
        pixels.append(QStringLiteral("%1").arg(image.pixel((2 * column + 1) * image.width() / 8,
            image.height() / 2), 8, 16, QChar(u'0')));
    return pixels;
}
QJsonObject frameEvidence(const Core::FrameLease& frame)
{
    const auto& format = frame.format();
    const auto& metadata = frame.metadata();
    QJsonObject result{{"width", format.width}, {"height", format.height}, {"pixelFormat", int(format.pixelFormat)},
        {"alpha", int(format.alpha)}, {"stride", qint64(format.planes[0].stride)}, {"rowBytes", qint64(format.planes[0].rowBytes)},
        {"poolGeneration", QString::number(frame.formatGeneration())}, {"transform", int(metadata.transform)},
        {"sourceId", QString::number(metadata.sourceId)}, {"sessionId", QString::number(metadata.sessionId)},
        {"sourceClock", int(metadata.timing.sourceClock)}, {"discontinuity", metadata.timing.discontinuity}};
    if (metadata.timing.sourcePts) result.insert(QStringLiteral("sourcePtsNs"), QString::number(metadata.timing.sourcePts->count()));
    if (metadata.timing.sourceSequence) result.insert(QStringLiteral("sourceSequence"), QString::number(*metadata.timing.sourceSequence));
    if (metadata.crop) result.insert(QStringLiteral("crop"), QJsonObject{{"x", int(metadata.crop->x)}, {"y", int(metadata.crop->y)},
        {"width", int(metadata.crop->width)}, {"height", int(metadata.crop->height)}});
    return result;
}
struct ConsumerResult {
    quint64 matched{};
    unsigned phases{};
    bool timestamp{}, streaming{}, identity{};
    Core::FrameSourceTerminal terminal;
    Core::FrameQueueStatistics queue;
    QImage last, failedFrame;
    QJsonObject failedNative;
    QString failure;
};
class Probe final : public QObject {
public:
    Capture::LiveCaptureSource source;
    Pattern pattern;
    QTimer painting, heartbeat;
    QThreadPool pool;
    QFutureWatcher<ConsumerResult> consumer;
    QString mode, path, failure;
    quint64 token{}, refresh{}, sourceId{}, generation{};
    unsigned preparations{}, refreshCompletions{};
    QSize originalSize;
    std::atomic<quint64> frames{};
    std::atomic<unsigned> phases{};
    int beats{};
    bool announced{}, consumed{}, stopped{}, sourceClosed{}, expired{};
    Capture::Failure sourceFailure;
    ConsumerResult result;

    Probe(QString scenario, QString evidence) : mode(std::move(scenario)), path(std::move(evidence))
    {
        pool.setMaxThreadCount(1);
        pattern.setWindowTitle(QStringLiteral("huhucapture real shared live source")); pattern.showFullScreen();
        painting.setInterval(60);
        connect(&painting, &QTimer::timeout, this, [this] { pattern.phase = (pattern.phase + 1) % 4; pattern.update(); });
        painting.start();
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [this] {
            ++beats;
            const auto observed = phases.load(std::memory_order_acquire);
            if (!announced && frames.load(std::memory_order_acquire) >= 6 && observed && (observed & (observed - 1))) {
                announced = true;
                record({{"event", "streamReady"}, {"mode", mode}, {"sourceId", QString::number(sourceId)}});
            }
        });
        heartbeat.start();
        connect(&source, &Capture::LiveCaptureSource::initialized, this, [this](const Capture::Capability& capability, const Capture::Failure& error) {
            if (error || !capability.canRequest()) { fail(QStringLiteral("Actual shared live source is unavailable: ") + error.detail); return; }
            if (!token) token = source.request(pattern.windowHandle());
            if (!token) fail(QStringLiteral("The actual source owner did not accept a request"));
        });
        connect(&source, &Capture::LiveCaptureSource::prepared, this,
            [this](quint64 request, const Capture::Snapshot& snapshot, const Capture::Failure& error) { prepared(request, snapshot, error); });
        connect(&source, &Capture::LiveCaptureSource::captureReady, this,
            [this](quint64 request, quint64 id, quint64 version, const Capture::Failure& error) {
                if (request != refresh || id != sourceId || version != generation || ++refreshCompletions != 1 || error) {
                    fail(error.detail); return;
                }
                claim();
            });
        connect(&consumer, &QFutureWatcher<ConsumerResult>::finished, this, [this] {
            consumed = true;
            try { result = consumer.result(); }
            catch (const std::exception& error) { failure = QString::fromUtf8(error.what()); }
            if (!result.failure.isEmpty() && failure.isEmpty()) failure = result.failure;
            source.consumerFinished(sourceId, generation);
        });
        connect(&source, &Capture::LiveCaptureSource::sourceClosed, this,
            [this](quint64 id, quint64 currentGeneration, const Capture::Failure& error) {
                sourceClosed = id == sourceId && currentGeneration == generation;
                sourceFailure = error;
            });
        connect(&source, &Capture::LiveCaptureSource::stopped, this, [this](quint64 request, const Capture::Failure&) {
            stopped = request == token; source.shutdown();
        });
        connect(&source, &Capture::LiveCaptureSource::shutdownFinished, this, [this] { finish(); });
        QTimer::singleShot(45000, this, [this] { expired = true; fail(QStringLiteral("The real shared live-source probe exceeded its deadline")); });
        QTimer::singleShot(0, &source, &Capture::LiveCaptureSource::initialize);
    }
    void prepared(quint64 request, const Capture::Snapshot& snapshot, const Capture::Failure& error)
    {
        if (request != token || ++preparations != 1 || error || snapshot.image.isNull()) { fail(error.detail); return; }
        if (snapshot.coordinates != Capture::CoordinateSpace::AuthorizedSourcePixels || snapshot.desktopOrigin ||
            !snapshot.screens.isEmpty() || !snapshot.windows.isEmpty() || inspect(snapshot.image) < 0) {
            fail(QStringLiteral("The first actual stream image or its source-local identity is invalid")); return;
        }
        sourceId = snapshot.sourceId; generation = snapshot.generation; originalSize = snapshot.image.size();
        record({{"event", "prepared"}, {"sourceId", QString::number(sourceId)}, {"generation", QString::number(generation)},
            {"width", originalSize.width()}, {"height", originalSize.height()}, {"globalCoordinates", false},
            {"samplesArgb", samplePixels(snapshot.image)}, {"pngSaved", snapshot.image.save(path + QStringLiteral(".prepared.png"))}});
        // This generic stream probe exercises the shared refresh handoff; the
        // separate controls probe checks exact before/after native pixels.
        refresh = source.refreshForCapture(sourceId, generation);
        if (!refresh) fail(QStringLiteral("The exact authorized source did not accept its asynchronous refresh"));
    }
    void claim()
    {
        const Core::FrameCrop selected{0, 0, static_cast<unsigned>(originalSize.width()), static_cast<unsigned>(originalSize.height())};
        auto claim = source.claim(sourceId, generation, selected);
        if (!claim || !claim->firstFrame || claim->firstFrame.metadata().sourceId != sourceId ||
            claim->claimedAt == Core::MediaClock::time_point{} || source.claim(sourceId, generation, selected) ||
            !source.isCurrent(sourceId, generation)) {
            fail(QStringLiteral("One exact authorized source cannot be claimed once")); return;
        }
        consumer.setFuture(QtConcurrent::run(&pool, [this, handle = std::move(*claim), scenario = mode]() mutable {
            return consume(std::move(handle), scenario);
        }));
    }
    ConsumerResult consume(Core::LiveSourceConsumer handle, const QString& scenario)
    {
        ConsumerResult output;
        Media::FrameNormalizer normalizer({2, 128u * 1024u * 1024u, 16384});
        Core::FrameLease last;
        try {
            auto first = handle.frames->takeLatest();
            if (!first.frame && !first.terminal) first.frame = std::move(handle.firstFrame);
            handle.firstFrame = {};
            while (true) {
                auto read = first.frame || first.terminal ? std::exchange(first, Core::FrameQueueRead{}) : handle.frames->waitNext();
                if (read.terminal) { output.terminal = std::move(*read.terminal); break; }
                if (!read.frame) throw std::runtime_error("An open source returned no frame or terminal");
                auto normalized = normalizer.normalize(read.frame, handle.selection);
                if (!normalized.frame) throw std::runtime_error("An actual source frame did not normalize with its confirmed selection");
                const auto& frame = normalized.frame;
                const QImage view(frame.row(0, 0).data(), frame.format().width, frame.format().height,
                    frame.format().planes[0].stride, QImage::Format_ARGB32);
                const int phase = inspect(view);
                if (phase < 0) {
                    output.failedFrame = view.copy();
                    output.failedNative = frameEvidence(read.frame);
                    output.failedNative.insert(QStringLiteral("normalized"), frameEvidence(frame));
                    output.failedNative.insert(QStringLiteral("samplesArgb"), samplePixels(view));
                    output.failedNative.insert(QStringLiteral("streamingAtFailure"), handle.streaming());
                    throw std::runtime_error("The live source no longer contains the actual painted surface");
                }
                ++output.matched; output.phases |= 1u << static_cast<unsigned>(phase);
                output.timestamp |= frame.metadata().timing.sourcePts.has_value() &&
                    frame.metadata().timing.sourceClock == Core::FrameClockDomain::PipeWire;
                output.streaming |= handle.streaming();
                output.identity = frame.metadata().sourceId == handle.sourceId && frame.metadata().sessionId != 0;
                frames.store(output.matched, std::memory_order_release);
                phases.store(output.phases, std::memory_order_release);
                last = std::move(normalized.frame);
                if (scenario == QStringLiteral("capture") && output.matched >= 16) handle.stop.request_stop();
            }
            if (last) {
                const QImage view(last.row(0, 0).data(), last.format().width, last.format().height,
                    last.format().planes[0].stride, QImage::Format_ARGB32);
                output.last = view.copy();
            }
        } catch (const std::exception& error) {
            output.failure = QString::fromUtf8(error.what()); handle.stop.request_stop();
            // Observe actual producer completion before releasing this consumer.
            while (true) {
                auto read = handle.frames->waitNext();
                if (read.terminal) { output.terminal = std::move(*read.terminal); break; }
            }
        }
        output.queue = handle.frames->statistics();
        return output;
    }
    void fail(QString message)
    {
        if (failure.isEmpty()) failure = message.isEmpty() ? QStringLiteral("The actual live source did not prepare") : std::move(message);
        if (refresh) source.cancelRefresh(refresh);
        if (token) source.cancel(token);
        source.shutdown();
    }
    void finish()
    {
        const bool saved = !result.last.isNull() && result.last.save(path);
        const bool failureSaved = !result.failedFrame.isNull() && result.failedFrame.save(path + QStringLiteral(".failure.png"));
        const bool expectedEnd = mode == QStringLiteral("capture") ? result.terminal.reason == Core::FrameSourceEnd::Stopped && !sourceFailure :
            mode == QStringLiteral("resize") ? result.terminal.reason == Core::FrameSourceEnd::SourceClosed &&
                sourceFailure.code == Capture::Failure::Code::SourceChanged :
                result.terminal.reason != Core::FrameSourceEnd::Stopped && static_cast<bool>(sourceFailure);
        const bool passed = failure.isEmpty() && !expired && preparations == 1 && refreshCompletions == 1 &&
            announced && consumed && stopped && sourceClosed &&
            beats > 10 && result.matched >= 6 && result.phases && (result.phases & (result.phases - 1)) &&
            result.timestamp && result.streaming && result.identity && saved && expectedEnd &&
            inspect(result.last) >= 0 && result.last.size() == originalSize && !source.isCurrent(sourceId, generation) &&
            result.queue.queued == 0 && result.queue.highWater <= 2 &&
            result.queue.submitted == result.queue.consumed + result.queue.droppedOldest + result.queue.droppedOnClose;
        record({{"event", "result"}, {"passed", passed}, {"mode", mode}, {"failure", failure},
            {"sourceFailure", sourceFailure.detail}, {"queueTerminal", QString::fromStdString(result.terminal.detail)},
            {"matchedFrames", static_cast<qint64>(result.matched)}, {"queueHighWater", static_cast<qint64>(result.queue.highWater)},
            {"sourceTimestampSeen", result.timestamp}, {"actualStreamingObserved", result.streaming},
            {"preparations", int(preparations)}, {"refreshTerminals", int(refreshCompletions)},
            {"singleConsumer", consumed}, {"allSourceOwnersDrained", stopped}, {"qtHeartbeat", beats},
            {"pngSaved", saved}, {"physicalDisplay", false}, {"userSelectionUiTested", false}});
        if (!result.failedNative.isEmpty()) record({{"event", "firstFrameMismatch"}, {"native", result.failedNative},
            {"pngSaved", failureSaved}, {"matchedBeforeFailure", qint64(result.matched)}});
        QCoreApplication::exit(passed ? 0 : 1);
    }
};
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    app.setApplicationName(QStringLiteral("huhucapture-live-source-probe"));
    app.setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture"));
    QCommandLineParser arguments; arguments.addHelpOption();
    arguments.addOption({"mode", "capture, resize or daemon-loss with the real owned Portal/PipeWire wrapper", "mode", "capture"});
    arguments.addOption({"evidence", "PNG from the last complete actual stream frame", "path"});
    arguments.process(app);
    const auto mode = arguments.value("mode");
    if ((mode != QStringLiteral("capture") && mode != QStringLiteral("resize") && mode != QStringLiteral("daemon-loss")) ||
        arguments.value("evidence").isEmpty() || !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) return 2;
    Probe probe(mode, arguments.value("evidence"));
    return app.exec();
}
