#pragma once

#include "services/media/ThumbnailCache.h"
#include "ui/qt/DesktopTypes.h"
#include "ui/qt/EditorTypes.h"
#include <QObject>
#include <memory>

namespace HuhuCapture::Desktop { class SystemClipboard; class FileRevealer; }
namespace HuhuCapture::Config { class SettingsStore; }
namespace HuhuCapture::UI::Qt { class EditorWindow; class HistoryWindow; }
class QWidget;

namespace HuhuCapture::Application {

// QApplication-owned media journeys. Views emit intentions; this one owner
// keeps the shared archive, system services and bounded work alive. LAN sending
// belongs to LanController and never to a history window or this worker pool.
class MediaWorkspace final : public QObject {
    Q_OBJECT
public:
    MediaWorkspace(std::shared_ptr<Media::MediaArchiveService> archive,
        Desktop::SystemClipboard& clipboard, Desktop::FileRevealer& revealer,
        Media::VideoCoverReader videoCover = {}, QObject* parent = nullptr);
    ~MediaWorkspace() override;
    void setTranslator(UI::Qt::DesktopTranslator translator);
    // Bind the QApplication's already-loaded store before opening any editor.
    // The store must outlive this workspace; no second configuration owner is
    // created. Rebinding to another store or after admission is rejected.
    bool setEditorSettings(Config::SettingsStore& settings);
    void setSendAvailable(bool available);
    UI::Qt::EditorWindow* openEditor(const QImage& image, bool pinned = true);
    UI::Qt::HistoryWindow* openHistory();
    using OutputCompletion = std::function<void(UI::Qt::OutputResult, const QString&)>;
    // Capture and editor surfaces share this output pipeline. origin owns the
    // request token, dialogParent is only its actual visible presentation, and
    // completion runs on this object's GUI thread while origin remains alive.
    // A rejected admission returns false without invoking completion.
    bool outputImage(QObject* origin, QWidget* dialogParent, quint64 token,
        UI::Qt::OutputIntent intent, QImage image, OutputCompletion completion);
    // Recording output already has a durable archive. Reuse the exact same
    // destination/overwrite and clipboard receipts without creating a second
    // archive or deleting the original when Save As is cancelled.
    bool outputArchived(QObject* origin, QWidget* dialogParent, quint64 token,
        UI::Qt::OutputIntent intent, const Media::Entry& archivedEntry, OutputCompletion completion);
    void cancelOutput(QObject* origin, quint64 token);
    void notifyArchiveChanged();
    void revealArchivedFile(const QString& path);
    bool busy() const noexcept;
    void shutdown();

Q_SIGNALS:
    void editorOpened(HuhuCapture::UI::Qt::EditorWindow* editor);
    void sendRequested(const QVector<HuhuCapture::Media::Entry>& entries);
    void archiveChanged();
    void busyChanged(bool busy);
    void operationFailed(const QString& message);
    // Every owned worker, dialog and desktop request has settled. Normal app
    // exit waits for this instead of destroying a service with active jobs.
    void shutdownFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application
