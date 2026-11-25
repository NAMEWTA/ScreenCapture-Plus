#include "recording/RecordingSession.h"
#include "platform/windows/NativeMp4Recording.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace recording = HuhuCapture::Recording;
namespace media = HuhuCapture::Media;
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
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        settle(); std::this_thread::sleep_for(1ms);
    }
}
recording::BackendRequest invalidRequest()
{
    recording::BackendRequest request;
    request.region = {0, 0, 32, 24}; request.framesPerSecond = 0;
    return request;
}
void preflightFailures(const QString& root)
{
    auto archive = std::make_shared<media::MediaArchiveService>(root + QStringLiteral("/archive"));
    recording::RecordingSession session(archive, HuhuCapture::Platform::Windows::runNativeMp4);
    std::vector<recording::SessionResult> completed;
    unsigned prepared{};
    QObject::connect(&session, &recording::RecordingSession::prepared, &session,
        [&](quint64, const auto&) { ++prepared; });
    QObject::connect(&session, &recording::RecordingSession::finished, &session,
        [&](const auto& result) { completed.push_back(result); });
    const auto first = session.start(invalidRequest());
    require(first != 0, "Admit actual asynchronous native preflight");
    waitFor([&] { return completed.size() == 1; }, "Native preflight result did not reach the Qt owner");
    const auto& failure = completed.front();
    require(!session.busy() && !prepared && failure.token == first &&
        failure.state == recording::SessionState::Failed && failure.failure &&
        failure.backend.media.firstError && !failure.backend.media.canArchive() && !failure.archivedEntry,
        "A real native prepare failure cannot report recording or successful media");
    require(!failure.recoveryPath.isEmpty() && QFileInfo(failure.recoveryPath).isFile() &&
        QFileInfo(failure.recoveryPath).size() == 0 && archive->list().entries.empty(),
        "Native failure retains its owned staging file but never publishes a history item");
    QFile retained(failure.recoveryPath);
    require(retained.open(QIODevice::ReadWrite), "Terminal result arrives only after the native writer closes");
    retained.close();

    // A real file in place of the archive directory produces a filesystem
    // admission failure. No fake backend or synthetic media result is used.
    const auto occupied = root + QStringLiteral("/occupied");
    QFile obstacle(occupied);
    require(obstacle.open(QIODevice::WriteOnly | QIODevice::NewOnly) && obstacle.write("keep") == 4,
        "Create an actual unrelated file blocking the archive directory");
    obstacle.close();
    auto blocked = std::make_shared<media::MediaArchiveService>(occupied);
    recording::RecordingSession cannotStage(blocked, HuhuCapture::Platform::Windows::runNativeMp4);
    std::optional<recording::SessionResult> blockedResult;
    QObject::connect(&cannotStage, &recording::RecordingSession::finished, &cannotStage,
        [&](const auto& result) { blockedResult = result; });
    require(cannotStage.start(invalidRequest()) != 0, "Admit actual filesystem preflight");
    waitFor([&] { return blockedResult.has_value(); }, "Blocked directory did not finish");
    require(blockedResult->state == recording::SessionState::Failed && blockedResult->failure &&
        blockedResult->recoveryPath.isEmpty() && !blockedResult->backend.prepared && !blockedResult->archivedEntry,
        "A staging failure remains separate from a native capture result");
    require(obstacle.open(QIODevice::ReadOnly) && obstacle.readAll() == QByteArray("keep"),
        "Failed staging preserves the unrelated blocking file");

    unsigned shutdown{};
    QObject::connect(&session, &recording::RecordingSession::shutdownFinished, &session, [&] { ++shutdown; });
    session.shutdown(); session.shutdown();
    require(shutdown == 1 && !session.start(invalidRequest()) && !session.requestStop(recording::StopIntent::Save),
        "Closed session emits one shutdown terminal and rejects new recording admission");
    std::cout << "PASS real native/filesystem preparation failures, retained identity and closed-session admission\n";
}
void terminalReentry(const QString& root)
{
    auto archive = std::make_shared<media::MediaArchiveService>(root + QStringLiteral("/reentry"));
    QPointer<recording::RecordingSession> session = new recording::RecordingSession(
        archive, HuhuCapture::Platform::Windows::runNativeMp4);
    unsigned finished{};
    quint64 first{}, second{};
    QObject observer;
    QObject::connect(session, &recording::RecordingSession::finished, &observer,
        [&](const recording::SessionResult& result) {
            ++finished;
            if (finished == 1) {
                first = result.token;
                second = session->start(invalidRequest());
            } else {
                // Actual QObject destruction from a completion slot must not
                // leave the delivery code touching its former owner afterward.
                delete session.data();
            }
        });
    require(session->start(invalidRequest()) != 0, "Start first actual preflight in reentrant owner");
    waitFor([&] { return finished == 2; }, "Starting a new recording from terminal callback did not complete");
    require(first && second > first && session.isNull(), "Each generation has one terminal and survives synchronous owner deletion");
    settle();
    require(finished == 2 && archive->list().entries.empty(), "Late queued work cannot publish or repeat old completion");
    std::cout << "PASS actual worker completion reentry, distinct tokens and QObject deletion\n";
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir run;
        require(run.isValid(), "Create an owned session test directory");
        preflightFailures(run.path()); terminalReentry(run.path());
        std::cout << "PASS Qt/native ownership tests; desktop, recording codecs and audio were not exercised here\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL recording session: " << error.what() << '\n'; return 1;
    }
}
