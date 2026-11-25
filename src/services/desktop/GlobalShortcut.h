#pragma once

#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

class QWindow;

namespace HuhuCapture::Platform { class GlobalShortcutBackend; }

namespace HuhuCapture::Desktop {

enum class ShortcutEditing { Unavailable, LocalCapture, SystemDialog };
enum class ShortcutError {
    None, Cancelled, Busy, Unavailable, Unsupported, InvalidShortcut,
    Conflict, Denied, SaveFailed, RestoreFailed,
};
struct ShortcutFailure {
    ShortcutError code{ShortcutError::None};
    qint64 nativeCode{};
    explicit operator bool() const noexcept { return code != ShortcutError::None; }
};
struct ShortcutSnapshot {
    ShortcutEditing editing{ShortcutEditing::Unavailable};
    QStringList requestedKeys;
    QString actualTrigger;
    bool initialized{};
    bool registered{};
    bool suspended{};
    bool portal{};
    ShortcutFailure failure;
};

// Legacy config uses canonical names joined by '+'. The final ordinary key
// may itself be '+': Ctrl++ must not be split into an empty chord.
QStringList parseShortcut(const QString& stored);
QStringList canonicalShortcut(const QStringList& keys);

// QApplication-thread owner. A candidate is registered before persist runs;
// false leaves the previous native registration/session owned and connected.
// persist must not enter an event loop or destroy this service. No key hook,
// local QShortcut or XWayland grab is substituted for a desktop-global binding.
class GlobalShortcut final : public QObject {
    Q_OBJECT
public:
    using Persist = std::function<bool()>;
    explicit GlobalShortcut(QObject* parent = nullptr);
    ~GlobalShortcut() override;
    void initialize();
    void setActionDescription(const QString& description);
    ShortcutSnapshot snapshot() const;
    quint64 bind(const QStringList& keys, QWindow* parent, Persist persist = {},
                 bool allowUnparentedPortal = false);
    void cancel(quint64 request);
    ShortcutFailure setCaptureActive(bool active);
    // Closes admission immediately; keep the dispatcher alive until the one
    // asynchronous completion, including canceled queued binding requests.
    void shutdown();

Q_SIGNALS:
    void initialized();
    void changed();
    void bindingFinished(quint64 request, const ShortcutFailure& failure);
    void activated(const QString& activationToken);
    void shutdownFinished();

private:
    std::unique_ptr<Platform::GlobalShortcutBackend> backend_;
    quint64 nextRequest_{1};
    quint64 pendingRequest_{};
    bool pendingCancelled_{};
    bool stopped_{};
    bool backendStopped_{}, shutdownQueued_{}, shutdownReported_{};
    unsigned deliveries_{};
    void checkShutdown();
};

} // namespace HuhuCapture::Desktop

Q_DECLARE_METATYPE(HuhuCapture::Desktop::ShortcutFailure)

