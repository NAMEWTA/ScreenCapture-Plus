#pragma once

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

struct PortalFailure {
    enum class Code {
        None, Cancelled, ParentDestroyed, Denied, Unavailable,
        Unsupported, Busy, InvalidState, Failed
    };
    Code code{Code::None};
    QString domain;
    int nativeCode{};
    QString remoteError;
    QString message;
    explicit operator bool() const noexcept { return code != Code::None; }
};

struct PortalFeature {
    quint32 version{};
    PortalFailure failure;
    bool available() const noexcept { return version != 0 && !failure; }
};

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
    quint64 requestScreenshot(QWindow* parent, bool interactive = true);
    quint64 requestScreenCast(QWindow* parent, const ScreenCastOptions& options = {});
    void cancelRequest(quint64 requestId);
    void closeScreenCast();
    bool hasActiveScreenCast() const;

    // Transfer once to the PipeWire consumer. The caller must close it or pass
    // ownership to PipeWire. Session ownership remains here until closeScreenCast.
    int takePipeWireFd();

Q_SIGNALS:
    void initialized(const PortalCapabilities& capabilities, const PortalFailure& failure);
    void screenshotFinished(quint64 requestId, const QUrl& uri, const PortalFailure& failure);
    void screenCastFinished(quint64 requestId, const QVector<PortalStream>& streams,
                            const QString& restoreToken, const PortalFailure& failure);
    void screenCastClosed(const PortalFailure& failure);

private:
    struct Private;
    std::shared_ptr<Private> d;
};

} // namespace HuhuCapture::Platform::Linux

Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalFailure)
Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalCapabilities)
Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalStream)
