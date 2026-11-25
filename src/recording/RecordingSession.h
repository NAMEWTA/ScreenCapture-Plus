#pragma once

#include "recording/RecordingBackend.h"
#include "services/media/MediaArchiveService.h"

#include <QObject>
#include <QString>
#include <memory>

namespace HuhuCapture::Recording {

enum class SessionState {
    Idle, Preparing, Recording, Stopping, Finalizing, Archiving,
    Completed, Cancelled, Discarded, Failed, RecoverableFailure
};

struct SessionResult {
    quint64 token{};
    SessionState state{SessionState::Failed};
    StopIntent intent{StopIntent::Save};
    BackendResult backend;
    Media::Failure failure;
    Media::ArchiveResult archive;
    // Created and verified by the worker after durable publication. Output
    // actions lease this exact identity; the GUI must not resnapshot the path.
    std::optional<Media::Entry> archivedEntry;
    QString recoveryPath;
};

// One application-owned session coordinates both platform workers. Admission,
// state and signals belong to the owner thread; native capture, finish, complete
// media inspection and archive publication run on one worker. No worker waits
// for Qt. Keep the service alive through shutdownFinished during normal exit.
class RecordingSession final : public QObject {
    Q_OBJECT
public:
    RecordingSession(std::shared_ptr<Media::MediaArchiveService> archive, Backend backend,
        QObject* parent = nullptr);
    ~RecordingSession() override;
    // Zero means rejected admission (busy, closed, missing service/backend or
    // wrong thread). While the service remains alive, a nonzero token receives
    // exactly one terminal result. Normal application exit awaits shutdownFinished.
    quint64 start(BackendRequest request);
    // The first intent wins. Repeated stop clicks cannot change an in-flight
    // save into a discard or publish the same recording twice.
    bool requestStop(StopIntent intent);
    // Stops capture and remaining validation without discarding owned media.
    // Partial/completed staging is retained for recovery if not yet committed.
    void shutdown();
    bool busy() const noexcept;
    SessionState state() const noexcept;
    quint64 token() const noexcept;
    Core::MediaTime elapsed() const noexcept;
    std::optional<SessionResult> lastResult() const;

Q_SIGNALS:
    void stateChanged(quint64 token, HuhuCapture::Recording::SessionState state);
    void prepared(quint64 token, const HuhuCapture::Recording::PreparedRecording& recording);
    // Completed means the full recording is durably archived. A shared output
    // controller uses intent + archivedEntry for Save As or clipboard, and must
    // report that subsequent action's actual result separately.
    void finished(const HuhuCapture::Recording::SessionResult& result);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Recording

Q_DECLARE_METATYPE(HuhuCapture::Recording::SessionState)
Q_DECLARE_METATYPE(HuhuCapture::Recording::PreparedRecording)
Q_DECLARE_METATYPE(HuhuCapture::Recording::SessionResult)
