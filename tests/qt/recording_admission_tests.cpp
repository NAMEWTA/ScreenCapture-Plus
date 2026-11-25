#include "recording/RecordingSession.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>

#include <iostream>
#include <memory>
#include <stdexcept>

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

        QPointer<recording::RecordingSession> destroyed = new recording::RecordingSession(archive, {});
        QObject observer;
        QObject::connect(destroyed.data(), &recording::RecordingSession::shutdownFinished, &observer,
            [&] { delete destroyed.data(); });
        destroyed->shutdown();
        require(destroyed.isNull(), "Synchronous owner deletion during shutdown must not access the service afterward");
        std::cout << "PASS real Qt admission, missing-capability refusal, idle shutdown and QObject signal reentry; no capture claimed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL recording admission: " << error.what() << '\n'; return 1;
    }
}
