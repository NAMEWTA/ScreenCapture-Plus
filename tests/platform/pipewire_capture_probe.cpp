#include <pipewire/pipewire.h>
#include "platform/linux/portal/PortalCapture.h"
#include "platform/linux/pipewire/PipeWireCapture.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <thread>

using namespace HuhuCapture;
using namespace HuhuCapture::Platform::Linux;

namespace {

constexpr std::array<std::array<int, 3>, 4> colors{{
    {210, 35, 45}, {30, 180, 90}, {45, 90, 220}, {230, 230, 230}
}};

void print(QJsonObject object)
{
    QTextStream(stdout) << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
}

class Pattern final : public QWidget {
public:
    int phase{};
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        for (int column = 0; column < 4; ++column) {
            const auto& color = colors[static_cast<std::size_t>((column + phase) % 4)];
            painter.fillRect(QRect(column * width() / 4, 0,
                (column + 1) * width() / 4 - column * width() / 4, height()),
                QColor(color[0], color[1], color[2]));
        }
    }
};

int inspectPattern(const Core::FrameLease& frame)
{
    const auto& format = frame.format();
    if (format.width < 4 || format.height < 1 || format.planeCount != 1 ||
        frame.metadata().transform != Core::FrameTransform::None)
        throw std::runtime_error("isolated test output has unexpected pixel geometry/transform");
    if (const auto& crop = frame.metadata().crop; crop &&
        (crop->x != 0 || crop->y != 0 || crop->width != static_cast<unsigned>(format.width) ||
         crop->height != static_cast<unsigned>(format.height)))
        throw std::runtime_error("isolated test output is unexpectedly cropped");
    if (format.pixelFormat != Core::FramePixelFormat::Bgra8 && format.pixelFormat != Core::FramePixelFormat::Rgba8)
        throw std::runtime_error("the SHM probe received an unsupported pixel format");
    const auto row = frame.row(0, static_cast<unsigned>(format.height / 2));
    for (int phase = 0; phase < 4; ++phase) {
        bool match = true;
        for (int column = 0; column < 4; ++column) {
            const auto x = static_cast<std::size_t>((2 * column + 1) * format.width / 8);
            const auto* pixel = row.data() + x * 4;
            const bool bgra = format.pixelFormat == Core::FramePixelFormat::Bgra8;
            const std::array<int, 3> actual{pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
            const auto& expected = colors[static_cast<std::size_t>((column + phase) % 4)];
            for (std::size_t channel = 0; channel < 3; ++channel)
                match &= std::abs(actual[channel] - expected[channel]) <= 3;
            match &= pixel[3] == 255;
        }
        if (match) return phase;
    }
    return -1;
}

class Probe final : public QObject {
public:
    PortalCapture portal;
    Pattern pattern;
    QTimer paintTimer;
    QTimer heartbeat;
    QString mode;
    QString evidencePath;
    std::jthread worker;
    quint64 request{};
    int beats{};
    bool closing{};
    bool deadlineExpired{};
    bool sessionClosed{};
    QString failure;

    Probe(QString testMode, QString output) : mode(std::move(testMode)), evidencePath(std::move(output))
    {
        pattern.setWindowTitle(QStringLiteral("huhucapture real PipeWire test surface"));
        pattern.showFullScreen();
        paintTimer.setInterval(60);
        connect(&paintTimer, &QTimer::timeout, this, [this] {
            pattern.phase = (pattern.phase + 1) % 4;
            pattern.update();
        });
        paintTimer.start();
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [this] { ++beats; });
        heartbeat.start();
        connect(&portal, &PortalCapture::initialized, this,
            [this](const PortalCapabilities& capabilities, const PortalFailure& error) {
                if (error || !capabilities.screenCast.available()) {
                    fail(QStringLiteral("Real ScreenCast Portal is unavailable: ") +
                         (error ? error.message : capabilities.screenCast.failure.message));
                    return;
                }
                ScreenCastOptions options;
                options.cursor = ScreenCastOptions::Cursor::Hidden;
                request = portal.requestScreenCast(pattern.windowHandle(), options);
            });
        connect(&portal, &PortalCapture::screenCastFinished, this,
            [this](quint64, const QVector<PortalStream>& streams, const QString&, const PortalFailure& error) {
                request = 0;
                if (error || streams.size() != 1) {
                    fail(QStringLiteral("Real ScreenCast authorization did not produce one source: ") + error.message);
                    return;
                }
                UniqueFd remote(portal.takePipeWireFd());
                if (!remote) { fail(QStringLiteral("Portal did not transfer a remote FD")); return; }
                const auto& stream = streams.front();
                PipeWireTarget target{stream.nodeId, stream.pipeWireSerial};
                print({{"event", "authorized"}, {"nodeId", static_cast<qint64>(target.nodeId)},
                       {"serial", target.serial ? QString::number(*target.serial) : QString{}},
                       {"platform", QGuiApplication::platformName()},
                       {"pipeWireClientVersion", QString::fromLatin1(pw_get_library_version())}});
                start(std::move(remote), target);
            });
        connect(&portal, &PortalCapture::screenCastClosed, this, [this](const PortalFailure& error) {
            if (closing) return;
            sessionClosed = true;
            if (failure.isEmpty()) failure = error ? error.message : QStringLiteral("Portal session closed during capture");
            if (worker.joinable()) worker.request_stop();
            else fail(failure);
        });
        QTimer::singleShot(45000, this, [this] {
            deadlineExpired = true;
            fail(QStringLiteral("Real capture probe deadline expired"));
        });
        QTimer::singleShot(0, &portal, &PortalCapture::initialize);
    }

    void fail(QString message)
    {
        if (failure.isEmpty()) failure = std::move(message);
        if (request) { portal.cancelRequest(request); return; }
        if (worker.joinable()) { worker.request_stop(); return; }
        closing = true;
        portal.closeScreenCast();
        print({{"event", "result"}, {"passed", false}, {"failure", failure}});
        QCoreApplication::exit(1);
    }

    void start(UniqueFd remote, PipeWireTarget target)
    {
        const auto testMode = mode;
        const auto path = evidencePath;
        worker = std::jthread([this, remote = std::move(remote), target, testMode, path](std::stop_token stop) mutable {
            try {
                std::array<bool, 4> phases{};
                std::uint64_t matched{};
                std::uint64_t afterResize{};
                bool announced{};
                bool geometryChanged{};
                bool sourceTimestampSeen{};
                std::optional<std::pair<int, int>> firstSize;
                Core::FrameLease heldFirst;
                Core::FrameLease heldLast;
                PipeWireCaptureOptions options;
                options.sessionId = 1;
                options.sourceId = 1;
                options.maximumFrameRate = 30;
                PipeWireCaptureCallbacks callbacks;
                callbacks.formatChanged = [&](const Core::FrameFormat& format, std::uint64_t) {
                    const auto dimensions = std::pair{format.width, format.height};
                    if (!firstSize) firstSize = dimensions;
                    else if (dimensions != *firstSize) geometryChanged = true;
                    return true;
                };
                callbacks.frame = [&](Core::FrameLease frame) {
                    const auto phase = inspectPattern(frame);
                    if (phase < 0) return true; // Only frames of the exposed test surface count as evidence.
                    phases[static_cast<std::size_t>(phase)] = true;
                    ++matched;
                    sourceTimestampSeen |= frame.metadata().timing.sourcePts.has_value() &&
                        frame.metadata().timing.sourceClock == Core::FrameClockDomain::PipeWire;
                    if (geometryChanged) ++afterResize;
                    if (!announced && matched >= 6 && std::ranges::count(phases, true) >= 2) {
                        announced = true;
                        QMetaObject::invokeMethod(this, [this] { print({{"event", "streamReady"}, {"mode", mode}}); },
                                                  Qt::QueuedConnection);
                    }
                    if (!heldFirst) heldFirst = std::move(frame);
                    else heldLast = std::move(frame);
                    if (testMode == QStringLiteral("daemon-loss")) return true;
                    if (testMode == QStringLiteral("resize")) return !(geometryChanged && afterResize >= 12);
                    return matched < 16;
                };
                const auto result = runPipeWireCapture(std::move(remote), target, options, std::move(callbacks), stop);
                const int distinct = static_cast<int>(std::ranges::count(phases, true));
                bool retainedFramesValid = heldFirst && inspectPattern(heldFirst) >= 0 && heldLast && inspectPattern(heldLast) >= 0;
                bool pngSaved = false;
                if (heldLast) {
                    const auto& format = heldLast.format();
                    const auto imageFormat = format.pixelFormat == Core::FramePixelFormat::Bgra8 ?
                        QImage::Format_ARGB32 : QImage::Format_RGBA8888;
                    const QImage snapshot(heldLast.plane(0).data(), format.width, format.height,
                        static_cast<qsizetype>(format.planes[0].stride), imageFormat);
                    pngSaved = snapshot.save(path);
                }
                QJsonObject record{{"event", "result"}, {"mode", testMode},
                    {"framesDelivered", static_cast<qint64>(result.stats.framesDelivered)},
                    {"matchedFrames", static_cast<qint64>(matched)}, {"distinctPatterns", distinct},
                    {"buffersDequeued", static_cast<qint64>(result.stats.buffersDequeued)},
                    {"buffersReturned", static_cast<qint64>(result.stats.buffersReturned)},
                    {"poolDrops", static_cast<qint64>(result.stats.poolDrops)},
                    {"corruptedDrops", static_cast<qint64>(result.stats.corruptedDrops)},
                    {"memFdFrames", static_cast<qint64>(result.stats.memFdFrames)},
                    {"memPtrFrames", static_cast<qint64>(result.stats.memPtrFrames)},
                    {"formatChanges", static_cast<qint64>(result.stats.formatChanges)},
                    {"geometryChanged", geometryChanged}, {"sourceTimestampSeen", sourceTimestampSeen},
                    {"peakPoolBytes", static_cast<qint64>(result.stats.peakPoolBytes)},
                    {"retainedFramesAfterTeardown", retainedFramesValid}, {"pngSaved", pngSaved},
                    {"captureFailure", result.failure ? QString::fromStdString(result.failure->message) : QString{}},
                    {"captureFailureCode", result.failure ? static_cast<int>(result.failure->code) : -1},
                    {"stopped", result.stopped}, {"physicalDisplay", false}, {"audioTested", false}};
                const bool pixelsValid = matched >= 4 && distinct >= 2 && retainedFramesValid && pngSaved &&
                    result.stats.buffersDequeued == result.stats.buffersReturned && result.stats.memFdFrames > 0 && sourceTimestampSeen;
                const bool captureSucceeded = !result.failure && result.stopped && matched >= 16 &&
                    (testMode != QStringLiteral("resize") || (geometryChanged && afterResize >= 12));
                const bool nativeFailure = result.failure &&
                    (result.failure->code == PipeWireFailure::Code::Connection ||
                     result.failure->code == PipeWireFailure::Code::Stream);
                QMetaObject::invokeMethod(this, [this, record = std::move(record), pixelsValid, captureSucceeded, nativeFailure]() mutable {
                    const bool failureObserved = nativeFailure || sessionClosed;
                    const bool passed = pixelsValid && beats >= 10 && !deadlineExpired &&
                        (mode == QStringLiteral("daemon-loss") ? failureObserved : captureSucceeded && failure.isEmpty());
                    record["passed"] = passed;
                    record["qtHeartbeat"] = beats;
                    record["portalFailure"] = failure;
                    record["deadlineExpired"] = deadlineExpired;
                    record["sessionClosed"] = sessionClosed;
                    closing = true;
                    portal.closeScreenCast();
                    print(std::move(record));
                    QCoreApplication::exit(passed ? 0 : 1);
                }, Qt::QueuedConnection);
            } catch (const std::exception& error) {
                const auto message = QString::fromUtf8(error.what());
                QMetaObject::invokeMethod(this, [this, message] {
                    closing = true;
                    portal.closeScreenCast();
                    print({{"event", "result"}, {"passed", false}, {"failure", message}});
                    QCoreApplication::exit(1);
                }, Qt::QueuedConnection);
            }
        });
    }
};

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("huhucapture-pipewire-probe"));
    app.setDesktopFileName(QStringLiteral("io.github.NAMEWTA.huhucapture"));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser arguments;
    arguments.addHelpOption();
    arguments.addOption({"mode", "capture, resize, or daemon-loss in an isolated real desktop", "mode", "capture"});
    arguments.addOption({"evidence", "Path for a PNG made from the captured CPU frame", "path"});
    arguments.process(app);
    const auto mode = arguments.value("mode");
    if ((mode != QStringLiteral("capture") && mode != QStringLiteral("resize") && mode != QStringLiteral("daemon-loss")) ||
        arguments.value("evidence").isEmpty() || QGuiApplication::platformName() != QStringLiteral("wayland") ||
        QString::fromLatin1(pw_get_library_version()) != QStringLiteral("1.6.11"))
        return 2;
    Probe probe(mode, arguments.value("evidence"));
    return app.exec();
}
