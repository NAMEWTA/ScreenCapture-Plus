#pragma once

#include "platform/linux/portal/PortalTypes.h"

#include <QObject>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVector>
#include <memory>
#include <optional>

class QWindow;

namespace HuhuCapture::Platform::Linux {

struct PortalCapabilities {
    PortalFeature screenshot;
    PortalFeature screenCast;
    quint32 sourceTypes{};
    quint32 cursorModes{};
    QString qtPlatform;
    QString dispatcher;
};

struct PortalStream {
    quint32 nodeId{};
    // ScreenCast v6 object.serial targets the same authorized remote. Never
    // reinterpret this as a node ID, or reconnect it on an unrelated global FD.
    std::optional<quint64> pipeWireSerial;
    quint32 sourceType{};
    QString id;
    // These are compositor coordinates, not negotiated PipeWire pixel geometry.
    std::optional<QPoint> position;
    std::optional<QSize> size;
};

struct ScreenCastOptions {
    enum class Cursor { Hidden, Embedded, Metadata };
    enum class Persistence { None, Application, Persistent };
    bool monitors{true};
    bool windows{false};
    bool multiple{false};
    Cursor cursor{Cursor::Embedded};
    Persistence persistence{Persistence::None};
    QString restoreToken;
};

// Linux Portal control plane, called and destroyed on the QApplication thread.
// It uses the Qt GLib dispatcher/default context; it never runs another GMainLoop.
// Successful ScreenCast setup yields selected nodes and a remote FD, not frames.
class PortalCapture final : public QObject {
    Q_OBJECT
public:
    explicit PortalCapture(QObject* parent = nullptr);
    ~PortalCapture() override;

    void initialize();
    // Screenshot permits a genuine null parent (tray/shortcut entry); a nonnull
    // parent must still be a visible, live native window. ScreenCast requires it.
    // Both request methods return zero after shutdown closes admission.
    quint64 requestScreenshot(QWindow* parent, bool interactive = true);
    quint64 requestScreenCast(QWindow* parent, const ScreenCastOptions& options = {});
    // Open another restricted connection to the already-authorized session.
    // The previous FD must have been taken and its consumer fully stopped.
    // This performs no new selection/permission request and transfers no frame.
    quint64 openPipeWireRemote();
    void cancelRequest(quint64 requestId);
    // Cancel work and release native requests on the shared GUI/GLib dispatcher.
    // Keep that dispatcher alive until shutdownFinished; destruction alone is
    // not a positive proof that asynchronous callbacks have drained.
    void shutdown();
    void closeScreenCast();
    bool hasActiveScreenCast() const;

    // Transfer once to the PipeWire consumer. The caller must close it or pass
    // ownership to PipeWire. Session ownership remains here until closeScreenCast.
    int takePipeWireFd();

Q_SIGNALS:
    void initialized(const PortalCapabilities& capabilities, const PortalFailure& failure);
    void capabilitiesChanged(const PortalCapabilities& capabilities);
    void screenshotFinished(quint64 requestId, const QUrl& uri, const PortalFailure& failure);
    void screenCastFinished(quint64 requestId, const QVector<PortalStream>& streams,
                            const QString& restoreToken, const PortalFailure& failure);
    void screenCastClosed(const PortalFailure& failure);
    void pipeWireRemoteOpened(quint64 requestId, const PortalFailure& failure);
    void shutdownFinished();

private:
    struct Private;
    std::shared_ptr<Private> d;
};

} // namespace HuhuCapture::Platform::Linux

Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalCapabilities)
Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalStream)
