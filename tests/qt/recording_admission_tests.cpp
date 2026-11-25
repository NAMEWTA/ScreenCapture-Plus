#include "recording/RecordingSession.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QTemporaryDir>

#include <iostream>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace recording = HuhuCapture::Recording;
namespace media = HuhuCapture::Media;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Create isolated admission test directory");
        const auto path = temporary.path() + QStringLiteral("/not-created");
        auto archive = std::make_shared<media::MediaArchiveService>(path);
        // An absent platform capability must refuse admission. No substitute
        // backend, frame or recording result is supplied by this test.
        recording::RecordingSession unavailable(archive, {});
        unsigned changes{}, completions{}, shutdowns{};
        QObject::connect(&unavailable, &recording::RecordingSession::stateChanged, &unavailable,
            [&](quint64, auto) { ++changes; });
        QObject::connect(&unavailable, &recording::RecordingSession::finished, &unavailable,
            [&](const auto&) { ++completions; });
        QObject::connect(&unavailable, &recording::RecordingSession::shutdownFinished, &unavailable,
            [&] { ++shutdowns; });
        require(!unavailable.start({}) && !unavailable.busy() && unavailable.token() == 0 &&
            unavailable.state() == recording::SessionState::Idle && !unavailable.lastResult() &&
            unavailable.elapsed().count() == 0 && !QFileInfo::exists(path),
            "Missing real platform capability must not allocate files or report recording success");
        require(!unavailable.requestStop(recording::StopIntent::Save) &&
            !unavailable.requestStop(recording::StopIntent::Copy) &&
            !unavailable.requestStop(recording::StopIntent::Discard), "No recording means no stop intent can be admitted");
        unavailable.shutdown(); unavailable.shutdown();
        QCoreApplication::processEvents();
        require(shutdowns == 1 && !changes && !completions && !unavailable.start({}),
            "Idle shutdown is idempotent and cannot invent a media terminal result");

        // A real filesystem failure must prevent even the admitted provider
        // from running. The rejection sentinel is never a capture substitute:
        // if invoked it fails, and this test never produces a media result.
        const auto occupied = temporary.filePath(QStringLiteral("occupied"));
        QFile obstacle(occupied);
        require(obstacle.open(QIODevice::WriteOnly | QIODevice::NewOnly) && obstacle.write("preserve") == 8,
            "Create a real non-directory archive obstruction");
        obstacle.close();
        auto blockedArchive = std::make_shared<media::MediaArchiveService>(occupied);
        std::atomic_uint invocations{};
        std::stop_source sourceStop;
        recording::Backend mustNotRun = [&](const auto&, auto, const auto&, auto) -> recording::BackendResult {
            ++invocations; sourceStop.request_stop();
            throw std::runtime_error("A refused or unstaged provider must not consume its claimed source");
        };
        require(!unavailable.start({}, mustNotRun) && !invocations && !sourceStop.stop_requested(),
            "Closed-session overload must not invoke or stop a request provider");
        // A real single-slot lease is held only by this task's provider. It
        // must be released before any terminal observer can finish its source,
        // even though the native provider is never invoked on staging failure.
        namespace core = HuhuCapture::Core;
        core::FramePool pool({1, 16u * 16u * 4u, 16});
        const auto format = core::packedFrameFormat(core::FramePixelFormat::Bgra8, 16, 16);
        require(format && pool.configure(*format) == core::FramePoolStatus::Ready, "Allocate an actual single-slot source pool");
        auto writable = pool.tryAcquire(); require(bool(writable), "Claim the actual initial native-frame slot");
        core::FrameMetadata metadata; metadata.sessionId = metadata.sourceId = 1;
        metadata.timing.receivedAt = core::MediaClock::now();
        auto lease = std::move(*writable).publish(metadata);
        require(bool(lease), "Publish real bounded source ownership");
        std::atomic_uint released{};
        struct Captured {
            core::FrameLease frame;
            std::atomic_uint& released;
            Captured(core::FrameLease value, std::atomic_uint& count) : frame(std::move(value)), released(count) {}
            ~Captured() { frame.reset(); ++released; }
        };
        auto capture = std::make_shared<Captured>(std::move(lease), released);
        const std::weak_ptr<Captured> weakCapture = capture;
        recording::Backend withLease = [capture = std::move(capture), mustNotRun](const auto& request, auto writer,
            const auto& events, auto stop) -> recording::BackendResult {
            return mustNotRun(request, std::move(writer), events, stop);
        };
        require(!pool.tryAcquire() && !weakCapture.expired(), "Only the task provider owns the actual exhausted slot");
        recording::RecordingSession perRequest(blockedArchive, {});
        std::optional<recording::SessionResult> rejectedByFilesystem;
        bool leaseReleasedBeforeTerminal{};
        QObject::connect(&perRequest, &recording::RecordingSession::finished, &perRequest,
            [&](const auto& value) {
                leaseReleasedBeforeTerminal = weakCapture.expired() && released == 1 && bool(pool.tryAcquire());
                rejectedByFilesystem = value;
            });
        const auto admitted = perRequest.start({}, std::move(withLease));
        require(admitted && perRequest.busy() && !perRequest.start({}, mustNotRun) &&
            !invocations && !sourceStop.stop_requested(),
            "One captured per-request provider cannot admit or consume a second provider while busy");
        QElapsedTimer deadline; deadline.start();
        while (!rejectedByFilesystem) {
            require(deadline.elapsed() < 10000, "Actual filesystem failure did not drain to its Qt owner");
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(rejectedByFilesystem->token == admitted && rejectedByFilesystem->failure &&
            rejectedByFilesystem->state == recording::SessionState::Failed && !invocations &&
            !sourceStop.stop_requested() && !perRequest.start({}) && leaseReleasedBeforeTerminal,
            "Failed staging releases the uninvoked provider and its real lease before finished; the default remains absent");
        require(obstacle.open(QIODevice::ReadOnly) && obstacle.readAll() == "preserve",
            "Rejected staging leaves the actual blocking file unchanged");

        QPointer<recording::RecordingSession> destroyed = new recording::RecordingSession(archive, {});
        QObject observer;
        QObject::connect(destroyed.data(), &recording::RecordingSession::shutdownFinished, &observer,
            [&] { delete destroyed.data(); });
        destroyed->shutdown();
        require(destroyed.isNull(), "Synchronous owner deletion during shutdown must not access the service afterward");
        std::cout << "PASS real Qt default/per-request admission, busy/closed refusal, filesystem failure, idle shutdown and QObject reentry; no capture claimed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL recording admission: " << error.what() << '\n'; return 1;
    }
}
