// Reuses this project's native_recording_probe.cpp visible-pattern, real
// backend -> shared session -> strict decode/archive verification method.
// Copyright (c) 2026 NAMEWTA. MIT. This probe needs a real isolated X server.
#include "platform/linux/X11Recording.h"
#include "recording/RecordingSession.h"
#include "services/media/GifInspection.h"
#include "services/media/VideoInspection.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <xcb/xcb.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace rec = HuhuCapture::Recording;
namespace media = HuhuCapture::Media;
namespace core = HuhuCapture::Core;
namespace native = HuhuCapture::Platform::Linux;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* detail)
{ if (!value) throw std::runtime_error(detail); }
template<class Predicate> void waitFor(Predicate predicate, const char* detail)
{
    const auto deadline = core::MediaClock::now() + 30s;
    while (!predicate()) {
        require(core::MediaClock::now() < deadline, detail);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
}
template<class Work> auto worker(Work work)
{
    auto result = QtConcurrent::run(std::move(work));
    waitFor([&] { return result.isFinished(); }, "The actual media verification worker did not return");
    return result.takeResult();
}
class Pattern final : public QWidget {
public:
    Pattern() : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
        setWindowTitle(QStringLiteral("huhucapture actual X11 recording validation"));
        setAttribute(Qt::WA_OpaquePaintEvent); setFixedSize(256, 192);
        timer_.setInterval(30);
        connect(&timer_, &QTimer::timeout, this, [this] { ++ticks_; update(); }); timer_.start();
    }
    unsigned painted{};
    static std::array<QColor, 4> colors()
    { return {QColor(220, 30, 40), QColor(30, 210, 45), QColor(25, 50, 220), QColor(170, 170, 170)}; }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const auto palette = colors();
        for (int index = 0; index < 4; ++index)
            painter.fillRect(QRect((index % 2) * width() / 2, (index / 2) * height() / 2,
                width() / 2, height() / 2), palette[static_cast<std::size_t>(index)]);
        painter.fillRect(QRect(width() / 2 - 18 + static_cast<int>(ticks_ % 32), height() / 2 - 8, 8, 16), Qt::white);
        ++painted;
    }
private:
    QTimer timer_; unsigned ticks_{};
};
struct Free { void operator()(void* value) const noexcept { std::free(value); } };
rec::PixelRegion actualClient(QWidget& widget)
{
    int number{};
    auto* connection = xcb_connect(nullptr, &number);
    struct Close { xcb_connection_t* value; ~Close() { if (value) xcb_disconnect(value); } } close{connection};
    require(connection && !xcb_connection_has_error(connection), "Open the actual isolated X server for native test coordinates");
    auto root = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int index = 0; index < number && root.rem; ++index) xcb_screen_next(&root);
    require(root.rem, "Select the actual native X root");
    const auto window = static_cast<xcb_window_t>(widget.winId());
    const std::unique_ptr<xcb_get_geometry_reply_t, Free> geometry(
        xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr));
    const std::unique_ptr<xcb_translate_coordinates_reply_t, Free> position(
        xcb_translate_coordinates_reply(connection, xcb_translate_coordinates(connection, window, root.data->root, 0, 0), nullptr));
    require(geometry && position && position->same_screen, "Read real native client pixels without guessing a Qt DPR");
    return {position->dst_x, position->dst_y, geometry->width, geometry->height};
}
void checkPattern(const QImage& image)
{
    require(!image.isNull() && image.width() > 8 && image.height() > 8, "Decode real captured pixels");
    const auto colors = Pattern::colors();
    for (int index = 0; index < 4; ++index) {
        const auto actual = image.pixelColor((index % 2 * 2 + 1) * image.width() / 4,
            (index / 2 * 2 + 1) * image.height() / 4);
        const auto& expected = colors[static_cast<std::size_t>(index)];
        require(std::abs(actual.red() - expected.red()) <= 32 && std::abs(actual.green() - expected.green()) <= 32 &&
            std::abs(actual.blue() - expected.blue()) <= 32, "The encoded image must contain the actual painted X11 quadrants in their correct positions");
    }
}
QString hash(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Open every archived byte for evidence hashing");
    QCryptographicHash result(QCryptographicHash::Sha256);
    require(result.addData(&file), "Hash actual archived media completely");
    return QString::fromLatin1(result.result().toHex());
}
QJsonArray rectangle(rec::PixelRegion value)
{ return {value.x, value.y, value.width, value.height}; }
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        require(QGuiApplication::platformName() == QStringLiteral("xcb"), "This probe requires a real X server; offscreen and Wayland are not native X11 evidence");
        QString templatePath;
        if (argc == 2) {
            const auto parent = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
            require(QDir().mkpath(parent), "Create the native probe evidence parent");
            templatePath = parent + QStringLiteral("/run-XXXXXX");
        }
        QTemporaryDir directory(templatePath);
        require(directory.isValid(), "Create an exclusive native probe evidence directory");
        if (argc == 2) directory.setAutoRemove(false);
        Pattern pattern; pattern.move(80, 70); pattern.show(); pattern.raise();
        waitFor([&] { return pattern.windowHandle() && pattern.windowHandle()->isExposed() && pattern.painted >= 3; },
            "The real X server did not expose and paint the recording pattern");
        auto region = actualClient(pattern); --region.width; --region.height;
        auto archive = std::make_shared<media::MediaArchiveService>(directory.filePath(QStringLiteral("archive")),
            [](const QString& file, media::Kind kind, std::stop_token stop) {
                return kind == media::Kind::Gif ? media::inspectGif(file, {}, stop) : media::inspectVideo(file, {}, stop);
            });
        rec::RecordingSession session(archive, native::makeX11RecordingBackend());
        unsigned beats{}; QTimer heartbeat; heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, &application, [&] { ++beats; }); heartbeat.start();
        QJsonArray cases;
        for (const auto format : {rec::Format::Mp4, rec::Format::Gif}) {
            rec::BackendRequest request; request.format = format; request.region = region;
            request.systemAudio = false; request.microphone = false; request.cursor = false;
            request.framesPerSecond = format == rec::Format::Gif ? 16 : 30;
            request.maximumDuration = 600ms;
            const auto actual = format == rec::Format::Mp4 ? rec::alignedMp4Region(region) : std::optional<rec::PixelRegion>{region};
            require(actual.has_value(), "The real client selection has a valid aligned capture region");
            std::optional<rec::SessionResult> completed;
            const auto observer = QObject::connect(&session, &rec::RecordingSession::finished, &application,
                [&](const auto& value) { completed = value; });
            const auto before = beats;
            require(session.start(request) != 0, "Admit real native X11 recording through the common session");
            waitFor([&] { return completed.has_value(); }, "The actual X11 recording did not stop and finish its shared media worker");
            QObject::disconnect(observer);
            const auto& result = *completed;
            if (result.failure) throw std::runtime_error("Actual X11 recording failed: " + result.failure.detail.toStdString());
            require(beats > before + 5 && result.state == rec::SessionState::Completed && result.archive.durable() &&
                result.archivedEntry && result.backend.prepared && result.backend.prepared->region == *actual &&
                result.backend.prepared->coordinates == rec::CoordinateSpace::DesktopPixels &&
                result.backend.prepared->source == rec::CaptureSource::X11 && !result.backend.prepared->sourceId &&
                !result.backend.prepared->generation && result.backend.durationLimitReached &&
                result.backend.sourceTerminal && result.backend.sourceTerminal->reason == core::FrameSourceEnd::Stopped &&
                result.backend.media.canArchive() && result.backend.media.framesWritten >= 3,
                "The native source drains, preserves its actual desktop coordinates, keeps GUI responsive and durably archives complete media");
            const auto evidence = worker([path = result.archive.path, format, submitted = result.backend.media.framesWritten] {
                const auto inspection = format == rec::Format::Gif ? media::inspectGif(path) : media::inspectVideo(path);
                require(!inspection.failure && inspection.frames >= 2, "Strictly decode the complete actual native recording");
                if (format == rec::Format::Mp4) require(inspection.frames == submitted, "Every submitted software video frame must survive complete codec finalization");
                QImage image;
                if (format == rec::Format::Gif) { QImageReader reader(path); image = reader.read(); }
                else { auto cover = media::videoThumbnail(path, 512); require(!cover.failure, "Decode the actual software video cover"); image = std::move(cover.image); }
                checkPattern(image);
                return QJsonObject{{QStringLiteral("sha256"), hash(path)},
                    {QStringLiteral("decodedFrames"), static_cast<qint64>(inspection.frames)}};
            });
            auto item = evidence;
            item[QStringLiteral("format")] = format == rec::Format::Gif ? QStringLiteral("gif") : QStringLiteral("mp4");
            item[QStringLiteral("archivePath")] = result.archive.path;
            item[QStringLiteral("requestedPixels")] = rectangle(region); item[QStringLiteral("capturedPixels")] = rectangle(*actual);
            item[QStringLiteral("framesSubmitted")] = static_cast<qint64>(result.backend.media.framesWritten);
            item[QStringLiteral("elapsed100ns")] = static_cast<qint64>(result.backend.elapsed.count());
            item[QStringLiteral("guiHeartbeats")] = static_cast<qint64>(beats - before);
            cases.append(item);
        }
        session.shutdown();
        QFile file(directory.filePath(QStringLiteral("x11-recording.json")));
        const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1},
            {QStringLiteral("source"), QStringLiteral("Actual isolated X server -> existing X11Capture -> finite FrameQueue -> shared GIF/H264 session")},
            {QStringLiteral("cases"), cases}, {QStringLiteral("notRun"), QJsonArray{
                QStringLiteral("physical displays/mixed DPI"), QStringLiteral("audio devices/clock drift"),
                QStringLiteral("monitor hotplug"), QStringLiteral("installation/update/uninstallation")}}}).toJson();
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size() && file.flush(),
            "Save exact native recording evidence without replacing previous output");
        std::cout << "PASS actual X11 readback, shared GIF/H264 encoding, native coordinates, complete inspection, archive and GUI responsiveness\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
