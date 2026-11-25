#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>
#include <optional>

namespace HuhuCapture::Updates {

enum class PackageKind { WindowsInstaller, WindowsPortableZip, LinuxDeb, LinuxAppImage };
enum class Stage { Idle, Checking, Available, Downloading, Downloaded, Failed, Cancelled };
enum class Error { None, Network, Metadata, Folder, Integrity, Save, DurabilityUnknown };

struct Snapshot {
    Stage stage{Stage::Idle};
    Error error{Error::None};
    QString version, tag, assetName, file;
    quint64 received{}, total{};
    PackageKind packageKind{PackageKind::WindowsInstaller};
    bool stopped{};
};

struct Options {
    // The host supplies the actual QStandardPaths Downloads location and the
    // installed package marker. Never infer either from a peer, URL or cwd.
    QString downloadsDirectory;
    QString currentVersion;
    PackageKind packageKind{PackageKind::WindowsInstaller};
    // Only an explicitly constructed integration fixture may redirect canonical
    // GitHub paths to a literal loopback HTTP origin. Production uses no override;
    // neither configuration nor the public UI accepts an arbitrary update URL.
    std::optional<QUrl> loopbackTestOrigin;
};

// One application-owned task. Windows/pages subscribe to immutable snapshots;
// closing one never cancels the download. Network, file IO, hashing and commit
// all run on the one lazily started worker, outside the UI thread.
class UpdateService final : public QObject {
    Q_OBJECT
public:
    explicit UpdateService(Options options, QObject* parent = nullptr);
    ~UpdateService() override;
    Snapshot snapshot() const;
    bool busy() const;
    bool check();
    bool download();
    // Cancels unfinished work, cleans only its own staging identity, closes the
    // real network objects and joins the worker before shutdownFinished.
    void shutdown();

Q_SIGNALS:
    void changed(const HuhuCapture::Updates::Snapshot& snapshot);
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
    bool begin(bool download);
};

QString packageKindName(PackageKind kind);

} // namespace HuhuCapture::Updates

Q_DECLARE_METATYPE(HuhuCapture::Updates::Snapshot)
