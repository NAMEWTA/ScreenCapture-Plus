#include "recording/FrameSource.h"
#include "recording/GifRecording.h"
#include "recording/RecordingSession.h"
#include "services/media/GifInspection.h"
#include "services/media/VideoInspection.h"
#include "ui/qt/RecordingWindow.h"
#ifndef _WIN32
#include "recording/SoftwareMp4Recording.h"
extern "C" {
#include <libavformat/avformat.h>
}
#endif

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace core = HuhuCapture::Core;
namespace rec = HuhuCapture::Recording;
namespace media = HuhuCapture::Media;
namespace ui = HuhuCapture::UI::Qt;
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
    const auto until = core::MediaClock::now() + 20s;
    while (!predicate()) {
        require(core::MediaClock::now() < until, message); settle(); std::this_thread::sleep_for(1ms);
    }
}
template<class Work> auto worker(Work work)
{
    auto future = std::async(std::launch::async, std::move(work));
    waitFor([&] { return future.wait_for(0ms) == std::future_status::ready; }, "Real codec/archive worker timed out");
    return future.get();
}
std::shared_ptr<media::MediaArchiveService> archive(const QString& path)
{
    return std::make_shared<media::MediaArchiveService>(path, [](const QString& file, media::Kind kind, std::stop_token stop) {
        if (kind == media::Kind::Gif) return media::inspectGif(file, {}, stop);
        return media::inspectVideo(file, {}, stop);
    });
}

// Controlled source pixels exercise the production bounded frame adapter and
// actual codecs. They are not desktop-capture or permission acceptance evidence.
std::unique_ptr<rec::FrameSource> source(const rec::BackendRequest& request,
    std::chrono::milliseconds latency = {}, unsigned incompleteAfter = 0)
{
    struct Pixels { std::vector<std::uint8_t> data; unsigned frames{}; };
    auto pixels = std::make_shared<Pixels>();
    pixels->data.resize(static_cast<std::size_t>(request.region.width) * request.region.height * 4);
    return rec::makePollingFrameSource(request, rec::CaptureSource::Gdi,
        [pixels, latency, incompleteAfter](rec::PixelRegion area, bool) {
            if (latency.count()) std::this_thread::sleep_for(latency);
            ++pixels->frames;
            for (int y = 0; y < area.height; ++y) for (int x = 0; x < area.width; ++x) {
                auto* pixel = pixels->data.data() + (static_cast<std::size_t>(y) * area.width + x) * 4;
                pixel[0] = static_cast<std::uint8_t>(x * 3 + pixels->frames * 23);
                pixel[1] = static_cast<std::uint8_t>(y * 5 + pixels->frames * 31);
                pixel[2] = static_cast<std::uint8_t>((x ^ y) * 7 + pixels->frames * 17); pixel[3] = 255;
            }
            const auto bytes = incompleteAfter && pixels->frames > incompleteAfter ? 4 : pixels->data.size();
            return rec::BorrowedBgraFrame{std::span(pixels->data).first(bytes), static_cast<std::size_t>(area.width) * 4, 0, {}};
        });
}
#ifndef _WIN32
// This is an explicit PCM codec fixture; native monitor/microphone acquisition,
// latency and long-running drift have their own real-desktop acceptance gate.
class PcmFixture final : public rec::AudioSource {
public:
    rec::AudioReadResult prepare(core::MediaClock::time_point, std::stop_token stop) override
    { return {0, {}, stop.stop_requested()}; }
    rec::AudioReadResult read(core::MediaClock::time_point begin, std::span<float> output,
        core::MediaClock::time_point, std::stop_token stop) override
    {
        if (stop.stop_requested()) return {0, {}, true};
        if (!origin_) origin_ = begin;
        const double offset = std::chrono::duration<double>(begin - *origin_).count();
        for (std::size_t sample = 0; sample < output.size() / 2; ++sample) {
            const auto t = offset + static_cast<double>(sample) / sampleRate;
            output[sample * 2] = static_cast<float>(0.25 * std::sin(440 * 6.283185307179586 * t));
            output[sample * 2 + 1] = static_cast<float>(0.125 * std::sin(660 * 6.283185307179586 * t));
        }
        return {};
    }
private:
    std::optional<core::MediaClock::time_point> origin_;
};
struct ContainerSummary { unsigned audio{}, video{}; double duration{}; };
ContainerSummary inspectContainer(const QString& path)
{
    AVFormatContext* format{};
    const auto bytes = QFile::encodeName(path);
    require(avformat_open_input(&format, bytes.constData(), nullptr, nullptr) == 0, "Open the actual encoded MP4");
    struct Close { AVFormatContext*& value; ~Close() { avformat_close_input(&value); } } close{format};
    require(avformat_find_stream_info(format, nullptr) >= 0, "Read actual muxed stream metadata");
    ContainerSummary summary;
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        const auto* stream = format->streams[index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++summary.video;
            require(stream->codecpar->codec_id == AV_CODEC_ID_H264, "Software fallback really uses H.264");
            summary.duration = stream->duration * av_q2d(stream->time_base);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++summary.audio;
            require(stream->codecpar->codec_id == AV_CODEC_ID_AAC && stream->codecpar->sample_rate == 48000 &&
                stream->codecpar->ch_layout.nb_channels == 2, "PCM is actually encoded as stereo 48 kHz AAC");
        }
    }
    return summary;
}
#endif
rec::Backend gifBackend(unsigned incompleteAfter = 0)
{
    return [incompleteAfter](const auto& request, auto writer, const auto& events, auto stop) {
        return rec::runGifRecording(request, std::move(writer), source(request, {}, incompleteAfter), events, stop);
    };
}
void pausedSources(const QString& root)
{
    struct State { unsigned reads{}; bool joined{}, destroyed{}; };
    class PausingSource final : public rec::FrameSource {
    public:
        PausingSource(std::unique_ptr<rec::FrameSource> source, std::shared_ptr<State> state)
            : source_(std::move(source)), state_(std::move(state)) {}
        ~PausingSource() override { state_->destroyed = true; }
        core::FrameSourceKind kind() const noexcept override { return core::FrameSourceKind::PipeWire; }
        core::FrameRead read(core::MediaClock::time_point deadline, std::stop_token stop) override
        {
            if (++state_->reads == 4)
                return {core::FrameReadStatus::Paused, {}, 0, "Explicit controlled-source pause"};
            require(state_->reads < 4, "A paused source must not be read again as a streaming source");
            return source_->read(deadline, stop);
        }
        void stopAndWait() noexcept override { source_->stopAndWait(); state_->joined = true; }
    private:
        std::unique_ptr<rec::FrameSource> source_;
        std::shared_ptr<State> state_;
    };
    auto files = archive(root + QStringLiteral("/paused-files"));
    for (const auto format : {rec::Format::Gif, rec::Format::Mp4}) {
#ifdef _WIN32
        if (format == rec::Format::Mp4) continue; // native MF has a separate real capture probe
#endif
        worker([&] {
            auto request = rec::recordingDefaults(format);
            request.region = {0, 0, 64, 48}; request.maximumDuration = 500ms;
            request.systemAudio = request.microphone = false;
            auto staging = files->createStaging(); require(!staging.failure, "Create actual pause recovery staging");
            std::error_code error; auto writer = staging.file.openWriter(error);
            require(writer && !error, "Open the exact pause recovery descriptor");
            auto state = std::make_shared<State>();
            auto frames = std::make_unique<PausingSource>(source(request), state);
            unsigned finalizing{};
            rec::BackendEvents events{{}, [&] {
                require(state->joined && state->destroyed, "The native source barrier precedes codec finalization");
                ++finalizing;
            }};
            rec::BackendResult result;
            if (format == rec::Format::Gif)
                result = rec::runGifRecording(request, std::move(writer), std::move(frames), events);
#ifndef _WIN32
            else result = rec::runSoftwareMp4Recording(request, std::move(writer), std::move(frames), {}, events);
#endif
            require(state->reads == 4 && state->joined && state->destroyed && finalizing == 1 &&
                result.media.framesWritten == 3 && result.media.finalized && !result.media.canArchive() &&
                result.media.firstError && result.media.firstError->stage == core::RecordingStage::Capture &&
                !result.durationLimitReached && !result.stopRequested,
                "A real pause terminates capture, preserves only the three actual frames, and retains its failure after codec drain");
            const auto decoded = format == rec::Format::Gif ? media::inspectGif(staging.file.path()) :
                media::inspectVideo(staging.file.path());
            require(!decoded.failure && decoded.frames == 3,
                "The retained paused-source bytes fully decode without invented streaming frames");
            return true;
        });
    }
    std::cout << "PASS explicit Paused versus Streaming, actual codec drain, source join and complete retained frames (controlled inputs)\n";
}
void encoders(const QString& root)
{
    auto files = archive(root + QStringLiteral("/codec-files"));
    auto request = rec::recordingDefaults(rec::Format::Gif); request.region = {0, 0, 64, 48}; request.maximumDuration = 300ms;
    const auto record = [&](const rec::BackendRequest& parameters, std::chrono::milliseconds latency, bool audio) {
        return worker([&] {
            auto staged = files->createStaging(); require(!staged.failure, "Create actual owned codec staging");
            std::error_code error; auto writer = staged.file.openWriter(error); require(bool(writer), "Open the exact checked staging descriptor");
            unsigned prepared{}, finalizing{};
            rec::BackendEvents events{
                [&](const rec::PreparedRecording& value) { ++prepared; require(value.region == parameters.region, "Preparation preserves the requested physical geometry"); },
                [&] { ++finalizing; }};
            rec::BackendResult result;
            if (parameters.format == rec::Format::Gif)
                result = rec::runGifRecording(parameters, std::move(writer), source(parameters, latency), events);
#ifndef _WIN32
            else result = rec::runSoftwareMp4Recording(parameters, std::move(writer), source(parameters, latency),
                audio ? std::make_unique<PcmFixture>() : nullptr, events);
#else
            static_cast<void>(audio);
#endif
            if (!result.media.canArchive()) throw std::runtime_error("Actual encoder did not finish: " + result.detail);
            require(prepared == 1 && finalizing == 1 && result.durationLimitReached && result.media.framesWritten >= 2,
                "Real preparation, bounded automatic stop and finalization happen once");
            const auto saved = files->commitStaged(staged.file, parameters.format == rec::Format::Gif ? media::Kind::Gif : media::Kind::Mp4);
            if (!saved.durable()) throw std::runtime_error("Full encoded-file inspection/publication failed: " + saved.failure.detail.toStdString());
            require(saved.media.dimensions == QSize(parameters.region.width, parameters.region.height) && saved.media.frames >= 2,
                "Complete strict decoding sees the actual dimensions and frames");
            if (parameters.format == rec::Format::Mp4) require(saved.media.frames == result.media.framesWritten,
                "Every submitted H.264 frame survives finalization and complete decode");
            return std::pair{saved.path, result};
        });
    };
    const auto gif = record(request, {}, false);
    require(QFileInfo(gif.first).size() > 0 && files->list().entries.size() == 1, "Real GIF result is present in managed history");
#ifndef _WIN32
    request = rec::recordingDefaults(rec::Format::Mp4); request.region = {0, 0, 64, 48}; request.maximumDuration = 300ms;
    request.systemAudio = false;
    const auto silent = record(request, {}, false);
    auto metadata = inspectContainer(silent.first);
    require(metadata.video == 1 && metadata.audio == 0 && std::abs(metadata.duration - .3) < .02,
        "A silent MP4 contains no synthesized audio track and retains its monotonic duration");
    request.systemAudio = true;
    const auto sound = record(request, {}, true); metadata = inspectContainer(sound.first);
    require(metadata.video == 1 && metadata.audio == 1, "Requested fixture audio produces a genuine fully decodable AAC track");
    request.systemAudio = false; request.maximumDuration = 450ms;
    const auto slow = record(request, 90ms, false); metadata = inspectContainer(slow.first);
    require(metadata.duration >= .44 && metadata.duration <= .47 && slow.second.media.framesWritten < 10,
        "A slow source keeps real elapsed duration rather than speeding up by frameCount/fps");
#endif
    worker([&] {
        auto staged = files->createStaging(); require(!staged.failure, "Create actual cancelled staging");
        std::error_code error; auto writer = staged.file.openWriter(error); std::stop_source stop; stop.request_stop();
        auto parameters = rec::recordingDefaults(rec::Format::Gif); parameters.region = {0, 0, 64, 48};
        const auto cancelled = rec::runGifRecording(parameters, std::move(writer), source(parameters), {}, stop.get_token());
        require(cancelled.media.completion(cancelled.stopRequested) == core::RecordingCompletion::Cancelled &&
            QFileInfo(staged.file.path()).size() == 0, "Stop before the first frame remains cancellation with a closed empty writer");
        return true;
    });
    std::cout << "PASS actual GIF/H.264/AAC encoding, full decoding, duration, descriptor closure and durable archive (controlled media inputs)\n";
}
void sessionLifecycle(const QString& root)
{
    auto files = archive(root + QStringLiteral("/session-files"));
    auto request = rec::recordingDefaults(rec::Format::Gif); request.region = {0, 0, 64, 48}; request.maximumDuration = 800ms;
    rec::RecordingSession session(files, gifBackend());
    std::vector<rec::SessionResult> completed;
    QObject::connect(&session, &rec::RecordingSession::finished, &session, [&](const auto& result) { completed.push_back(result); });
    unsigned heartbeat{}; QTimer pulse; pulse.setInterval(5); QObject::connect(&pulse, &QTimer::timeout, &session, [&] { ++heartbeat; }); pulse.start();
    auto intent = rec::StopIntent::Copy;
    QObject::connect(&session, &rec::RecordingSession::prepared, &session, [&](quint64, const auto&) {
        QTimer::singleShot(180, &session, [&] {
            require(session.requestStop(intent), "First live stop intent is admitted");
            require(!session.requestStop(rec::StopIntent::Save), "A duplicate stop cannot replace the user's first intent");
        });
    });
    const auto first = session.start(request);
    require(first && !session.start(request), "One live recording owns the source and writer");
    waitFor([&] { return completed.size() == 1; }, "Real GIF session finalization and archive completion");
    require(completed[0].state == rec::SessionState::Completed && completed[0].intent == rec::StopIntent::Copy &&
        completed[0].archive.durable() && files->list().entries.size() == 1 && heartbeat > 10,
        "Copy intent waits for actual complete media and durable archive without blocking Qt");
    intent = rec::StopIntent::Discard;
    require(session.start(request) > first, "A completed session admits a distinct recording token");
    waitFor([&] { return completed.size() == 2; }, "Actual discard finalizes and cleans only its own staging");
    require(completed[1].state == rec::SessionState::Discarded && completed[1].archive.path.isEmpty() &&
        completed[1].recoveryPath.isEmpty() && files->list().entries.size() == 1,
        "Discard does not remove prior history or leave its own media published");

    rec::RecordingSession malformed(files, gifBackend(3)); std::optional<rec::SessionResult> failure;
    QObject::connect(&malformed, &rec::RecordingSession::finished, &malformed, [&](const auto& result) { failure = result; });
    require(malformed.start(request) != 0, "Start the production adapter with an eventually incomplete input buffer");
    waitFor([&] { return failure.has_value(); }, "The actual buffer bounds failure reaches its owner");
    require(failure->state == rec::SessionState::RecoverableFailure && failure->backend.media.firstError &&
        failure->backend.media.firstError->stage == core::RecordingStage::Capture && failure->backend.media.finalized &&
        failure->archive.path.isEmpty() && !failure->recoveryPath.isEmpty() && files->list().entries.size() == 1,
        "Successful codec finish cannot erase an earlier capture failure or publish partial history");
    require(!worker([&] { return media::inspectGif(failure->recoveryPath); }).failure,
        "The retained failure file contains actual recoverable, fully decoded frames");
    QFile recovered(failure->recoveryPath); require(recovered.open(QIODevice::ReadWrite), "Terminal delivery occurs after native output descriptor closure"); recovered.close();

    rec::RecordingSession closing(files, gifBackend()); std::optional<rec::SessionResult> closed; unsigned shutdown{};
    QObject::connect(&closing, &rec::RecordingSession::prepared, &closing, [&](quint64, const auto&) { QTimer::singleShot(180, &closing, &rec::RecordingSession::shutdown); });
    QObject::connect(&closing, &rec::RecordingSession::finished, &closing, [&](const auto& result) { closed = result; });
    QObject::connect(&closing, &rec::RecordingSession::shutdownFinished, &closing, [&] { ++shutdown; });
    require(closing.start(request) != 0, "Start real media before application shutdown");
    waitFor([&] { return shutdown == 1; }, "Shutdown waits for the actual codec worker and file closure");
    require(closed && !closing.busy() && !closing.start(request) && closed->state == rec::SessionState::RecoverableFailure &&
        !closed->recoveryPath.isEmpty() && files->list().entries.size() == 1,
        "Shutdown preserves captured recovery data, rejects new work and does not publish cancelled archive processing");
    closing.shutdown(); require(shutdown == 1, "Shutdown terminal is reported once");
    std::cout << "PASS real session Save/Copy/Discard intent, Qt responsiveness, first-error retention and shutdown recovery\n";
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir root; require(root.isValid(), "Create a private real recording test workspace");
        encoders(root.path()); pausedSources(root.path()); sessionLifecycle(root.path());
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
