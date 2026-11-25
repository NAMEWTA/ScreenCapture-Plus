#pragma once

#include "services/desktop/DesktopResult.h"
#include "services/media/MediaArchiveService.h"
#include <QObject>
#include <memory>

class QWindow;

namespace HuhuCapture::Desktop {

enum class RevealEffect { None, SelectedFile, OpenedParent, OpenedDirectory };
struct RevealResult {
    Failure failure;
    RevealEffect effect{RevealEffect::None};
    QString path;
    bool completed() const noexcept { return !failure && effect != RevealEffect::None; }
};

// Application-owned GUI facade. Validation and Windows Shell work run in the
// worker pool; Linux's real DBus/GIO completion runs through Qt's GLib loop.
class FileRevealer final : public QObject {
    Q_OBJECT
public:
    explicit FileRevealer(QObject* parent = nullptr);
    ~FileRevealer() override;
    quint64 revealFile(std::shared_ptr<const Media::MediaLease> lease, QWindow* origin = nullptr);
    // The archive button sets createIfMissing to retain the original behavior.
    // Update/download locations should already exist and use the default false.
    quint64 openDirectory(const QString& path, QWindow* origin = nullptr, bool createIfMissing = false);
    void cancel(quint64 token);
    void shutdown();
    bool busy() const noexcept;
Q_SIGNALS:
    void finished(quint64 token, const HuhuCapture::Desktop::RevealResult& result);
    void shutdownFinished();
private:
    struct State;
    std::unique_ptr<State> state_;
    quint64 start(std::shared_ptr<const Media::MediaLease> lease, QString path,
        bool selectFile, bool createIfMissing, QWindow* origin);
    void finish(quint64 token, RevealResult result);
    void finishShutdown();
};

} // namespace HuhuCapture::Desktop

Q_DECLARE_METATYPE(HuhuCapture::Desktop::RevealResult)
