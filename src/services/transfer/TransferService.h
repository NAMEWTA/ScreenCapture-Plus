#pragma once

#include "platform/FileIdentity.h"
#include "services/transfer/ReceiveSink.h"

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace HuhuCapture::Transfer {

// Adapter budgets supplement the native core's protocol limits. They bound
// bytes as well as counts and reserve terminal notification space at admission.
struct TransferLimits {
    static constexpr std::size_t maximumQueuedCommands = 16;
    static constexpr std::size_t maximumNotifications = 64;
    static constexpr std::size_t maximumNotificationBytes = 16 * 1024 * 1024;
    static constexpr std::size_t maximumControlJsonBytes = 1024 * 1024;
    static constexpr std::size_t maximumTerminalJsonBytes = 128 * 1024;
    // Up to 64 exact native paths (each <=32768 UTF-16 code units) plus the
    // protocol status. Never truncate a path after its file was published.
    static constexpr std::size_t maximumPathCodeUnits = 32768;
    static constexpr std::size_t maximumReceiveTerminalBytes = 5 * 1024 * 1024;
    static constexpr std::size_t maximumSmallJsonBytes = 4096;
    static constexpr std::size_t maximumFiles = 64;
    static constexpr std::size_t maximumNativeOperations = 3;
    static constexpr std::size_t maximumReceiveProgress = 16;
    static constexpr std::size_t maximumSnapshotBytes = maximumControlJsonBytes +
        (maximumNativeOperations + maximumReceiveProgress) * maximumSmallJsonBytes;
    static constexpr std::uint32_t nativePollMilliseconds = 20;
    static constexpr int notificationMilliseconds = 100;
    // Reopening a view restores application-owned facts, not a second task
    // controller. Completed activity is deliberately finite; media history is
    // the durable record. No private key, PIN or media lease is retained here.
    static constexpr std::size_t maximumViewSourceBytes = 128 * 1024;
    static constexpr std::size_t maximumRecentTasks = 16;
    static constexpr std::size_t maximumRecentTaskBytes = 8 * 1024 * 1024;
    static constexpr std::size_t maximumRecentDiscoveries = 16;
    static constexpr std::size_t maximumRecentDiscoveryBytes = 256 * 1024;
    static constexpr std::int64_t recentTaskLifetimeMilliseconds = 30 * 60 * 1000;
};

class PreparedMedia;
struct PreparedMediaResult {
    int status{HUHU_INVALID_STATE};
    std::shared_ptr<const PreparedMedia> media;
};

// A stable native handle plus the application history lease. Prepare on a
// worker after acquiring the selected archive entry's read lease. It compares
// the already-open object's full integer identity and size, so a replaced path
// cannot silently send another entry. Retain immutability while leased; Linux
// does not prohibit unrelated processes from writing an already-open inode.
class PreparedMedia final {
public:
    static PreparedMediaResult open(const QString& path, const Platform::FileSnapshot& expected,
        std::uint32_t expectedMediaKind, std::shared_ptr<const void> historyLease);
    ~PreparedMedia();
    PreparedMedia(const PreparedMedia&) = delete;
    PreparedMedia& operator=(const PreparedMedia&) = delete;
    const HuhuMediaLeaseInfo& info() const noexcept { return info_; }
    const HuhuMediaLease* nativeHandle() const noexcept { return native_; }
    const QString& path() const noexcept { return path_; }
    const Platform::FileSnapshot& snapshot() const noexcept { return snapshot_; }
private:
    PreparedMedia(HuhuMediaLease* native, HuhuMediaLeaseInfo info,
        std::shared_ptr<const void> historyLease, QString path, Platform::FileSnapshot snapshot);
    HuhuMediaLease* native_{};
    HuhuMediaLeaseInfo info_{};
    std::shared_ptr<const void> historyLease_;
    QString path_;
    Platform::FileSnapshot snapshot_;
};

using RequestId = std::uint64_t;
enum class ServiceState { Disabled, Starting, Enabled, Stopping, Closed };
enum class EventKind : std::uint32_t {
    ReceiveOffer = HUHU_RECEIVE_OFFER,
    ReceiveFinished = HUHU_RECEIVE_END,
    SendProgress = HUHU_SEND_PROGRESS,
    SendFinished = HUHU_SEND_END,
    DevicesChanged = HUHU_DISCOVERY_CHANGED,
    DiscoveryFinished = HUHU_DISCOVERY_END,
    ListenerFailed = HUHU_LISTENER_ERROR,
    Enabled = 100,
    Disabled = 101,
    CommandFinished = 102,
    ReceiveProgress = 103,
    // Only facade admission, never remote acceptance or completed transfer.
    SendQueued = 104,
    ReceiveDecisionQueued = 105,
};

struct SendFile {
    QString path;
    QString name;
    Platform::FileSnapshot snapshot;
    std::uint32_t mediaKind{};
};

struct Event {
    EventKind kind{EventKind::CommandFinished};
    int status{HUHU_INVALID_STATE};
    RequestId request{}; // Facade admission ID, never a lossy JSON number.
    std::uint64_t generation{};
    // Native immutable JSON fields are documented in docs/rust-bridge.md.
    // Receive terminals may add localStatus when local budget/sink admission
    // failed. CommandFinished describes only command acceptance, never a save.
    QJsonObject data;
    std::vector<ReceiveResult> receivedFiles;
    std::optional<ReceiveProgress> receiveProgress;
    // Present only on SendQueued. These immutable values allow a retry to ask
    // the host for a NEW validated lease; they do not retain an open source.
    std::vector<SendFile> sendFiles;
    // Monotonic order assigned by this facade when it publishes/cache-delivers
    // an event. It orders a manual probe against a later discovery snapshot.
    std::uint64_t sequence{};
};

struct ViewSnapshot {
    ServiceState state{ServiceState::Disabled};
    std::optional<Event> service;
    std::optional<Event> devices;
    // Each task is replayed in origin/decision/progress/terminal order. Device
    // snapshots and progress are coalesced; the same Event contract is used by
    // live subscribers. Expired completed tasks are absent, active tasks stay.
    std::vector<Event> events;
    std::size_t retainedBytes{};
};

struct Submission {
    int status{HUHU_INVALID_STATE};
    RequestId request{};
    explicit operator bool() const noexcept { return status == HUHU_OK; }
};

struct Statistics {
    std::size_t queuedCommands{};
    std::size_t queuedNotifications{};
    std::size_t notificationBytes{};
    std::size_t reservedNotifications{};
    std::size_t reservedBytes{};
    bool nativeRunning{};
};

// UI-thread facade. The actor owns every transfer control/poll call; only the
// ReceiveSink workers use their individual receives. The GUI timer drains value
// snapshots at 10 Hz, avoiding an unbounded queue of cross-thread Qt callbacks.
// Closing a view only disconnects its subscription; it does not call disable.
class TransferService final : public QObject {
    Q_OBJECT
public:
    explicit TransferService(QObject* parent = nullptr);
    ~TransferService() override;
    TransferService(const TransferService&) = delete;
    TransferService& operator=(const TransferService&) = delete;

    // Configuration is the native JSON contract, copied into the actor. Saved
    // identity/private key handling belongs to the configuration service; no
    // identity is generated or silently replaced here. A cold service creates
    // no Rust runtime/listener/discovery until this explicit call succeeds.
    Submission enable(QByteArray configuration, std::unique_ptr<ReceiveSink> sink);
    Submission send(QByteArray verifiedPeer, std::vector<std::shared_ptr<const PreparedMedia>> media,
        QString displayAlias = {});
    Submission discover(QString address, std::uint16_t port);
    Submission accept(QString sessionId, QStringList fileIds);
    Submission cancelSend(RequestId request);
    Submission cancelReceive(QString sessionId);

    // Asynchronous, priority stop. It is admitted even when command/notification
    // budgets are full. Disabled is delivered after sink join, Rust stop/drain,
    // and all operation terminals. Re-enable requires a fresh idle sink.
    int disable();
    ServiceState state() const;
    Statistics statistics() const;
    // GUI-thread only. Returns nullopt on a foreign thread. Snapshot reads do
    // not start networking and never wait for the actor or a file operation.
    std::optional<ViewSnapshot> viewSnapshot() const;

    // Final lifetime barrier, also used by the destructor. Normal UI exit first
    // calls disable and waits for Disabled; only the final teardown joins here.
    // It never relies on the GUI processing an event to release a worker.
    void shutdownAndJoin() noexcept;

signals:
    void eventReady(const HuhuCapture::Transfer::Event& event);

private:
    struct Impl;
    void drainNotifications();
    void remember(Event& event);
    std::unique_ptr<Impl> impl_;
    QTimer notifications_;
};

} // namespace HuhuCapture::Transfer

Q_DECLARE_METATYPE(HuhuCapture::Transfer::Event)
