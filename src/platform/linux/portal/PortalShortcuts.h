#pragma once

#include "platform/linux/portal/PortalTypes.h"

#include <QObject>
#include <QVector>
#include <functional>
#include <memory>

class QWindow;

namespace HuhuCapture::Platform::Linux {

struct PortalShortcutRequest {
    QString id;
    QString description;
    // XDG shortcut specification syntax, not QKeySequence's display text.
    QString preferredTrigger;
};

struct PortalShortcutBinding {
    QString id;
    QString description;
    QString triggerDescription;
};

struct PortalShortcutCapabilities {
    PortalFeature globalShortcuts;
    QString qtPlatform;
    QString dispatcher;
};

// Owned by an application service and used only on the QApplication thread.
// A failed replacement leaves the previous active session owned and connected.
class PortalShortcuts final : public QObject {
    Q_OBJECT
public:
    using Commit = std::function<bool()>;
    explicit PortalShortcuts(QObject* parent = nullptr);
    ~PortalShortcuts() override;

    void initialize();
    void cancelInitialization();
    // Commit runs on the GUI thread after the candidate is completely granted
    // and before the old session is closed. Returning false retains the old
    // session. It must not spin an event loop or claim the candidate is active.
    quint64 bindShortcuts(QWindow* parent, const QVector<PortalShortcutRequest>& shortcuts, Commit commit = {});
    // An explicit background restoration request. The protocol permits an
    // empty parent string; the desktop may still display authorization. This
    // is never a fabricated Wayland handle or proof of a remembered grant.
    quint64 restoreShortcuts(const QVector<PortalShortcutRequest>& shortcuts, Commit commit = {});
    void cancelRequest(quint64 requestId);
    void closeBindings();
    bool hasActiveBindings() const;
    bool available() const;
    QVector<PortalShortcutBinding> bindings() const;

Q_SIGNALS:
    void initialized(const PortalShortcutCapabilities& capabilities, const PortalFailure& failure);
    void bindingFinished(quint64 requestId, const QVector<PortalShortcutBinding>& assigned,
                         const PortalFailure& failure);
    void bindingsChanged(const QVector<PortalShortcutBinding>& assigned);
    void bindingsRefreshFailed(const PortalFailure& failure);
    void bindingsClosed(const PortalFailure& failure);
    // Timestamp has millisecond granularity and an undefined origin. The token
    // may activate a window in response to this user action; do not persist it.
    void activated(const QString& id, quint64 timestamp, const QString& activationToken);
    void deactivated(const QString& id, quint64 timestamp, const QString& activationToken);

private:
    quint64 beginBinding(QWindow* parent, const QVector<PortalShortcutRequest>& shortcuts, Commit commit, bool unparented);
    struct Private;
    std::shared_ptr<Private> d;
};

} // namespace HuhuCapture::Platform::Linux

Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalShortcutCapabilities)
Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalShortcutBinding)
