#pragma once

#include "services/desktop/DesktopResult.h"
#include "services/media/MediaArchiveService.h"

#include <QObject>
#include <QStringList>
#include <memory>
#include <stop_token>

class QWindow;

namespace HuhuCapture::Desktop {

struct ClipboardLimits {
    std::uint64_t maximumImageBytes{512ull * 1024 * 1024};
    std::uint64_t maximumTextBytes{16ull * 1024 * 1024};
};
struct ClipboardPreparation;

class PreparedClipboard final {
public:
    ~PreparedClipboard();
    PreparedClipboard(const PreparedClipboard&) = delete;
    PreparedClipboard& operator=(const PreparedClipboard&) = delete;
private:
    PreparedClipboard();
    struct State;
    std::unique_ptr<State> state_;
    friend ClipboardPreparation prepareImage(const QImage&, ClipboardLimits, std::stop_token);
    friend ClipboardPreparation prepareText(const QString&, ClipboardLimits, std::stop_token);
    friend ClipboardPreparation prepareFile(std::shared_ptr<const Media::MediaLease>, ClipboardLimits, std::stop_token);
    friend class SystemClipboard;
};

struct ClipboardPreparation { Failure failure; std::shared_ptr<PreparedClipboard> content; };
struct ClipboardResult {
    Failure failure;
    QStringList publishedFormats;
    bool completed() const noexcept { return !failure; }
};

// Worker APIs: own pixels, encode PNG/Windows formats, validate archive identity.
// prepareFile preserves PNG-as-image and GIF/MP4-as-file. Its read lease lasts
// through publication, never for the lifetime of the clipboard's URI content.
ClipboardPreparation prepareImage(const QImage& image, ClipboardLimits limits = {}, std::stop_token stop = {});
ClipboardPreparation prepareFile(std::shared_ptr<const Media::MediaLease> lease,
    ClipboardLimits limits = {}, std::stop_token stop = {});
// Plain text is owned, valid Unicode. Windows publishes CF_UNICODETEXT (CRLF);
// Linux publishes explicit UTF-8 MIME data through the same receipt/owner path.
ClipboardPreparation prepareText(const QString& text, ClipboardLimits limits = {}, std::stop_token stop = {});

// QApplication-owned, GUI-thread service. A history/editor window is only a
// request origin; closing it must not destroy the Linux clipboard owner.
class SystemClipboard final : public QObject {
    Q_OBJECT
public:
    explicit SystemClipboard(QObject* parent = nullptr);
    ~SystemClipboard() override;
    // One-shot content: even a failed attempt releases the file read lease.
    // Every nonzero token gets one queued completion while this service lives.
    quint64 publish(std::shared_ptr<PreparedClipboard> content, QWindow* origin);
    void cancel(quint64 token);
    void shutdown();
    bool busy() const noexcept;
Q_SIGNALS:
    void finished(quint64 token, const HuhuCapture::Desktop::ClipboardResult& result);
    void shutdownFinished();
private:
    struct State;
    std::unique_ptr<State> state_;
    void validatePrepared(quint64 token);
    void publishPrepared(quint64 token);
    void finish(quint64 token, ClipboardResult result);
    void finishShutdown();
};

} // namespace HuhuCapture::Desktop

Q_DECLARE_METATYPE(HuhuCapture::Desktop::ClipboardResult)
