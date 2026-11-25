#pragma once

#include "services/media/ThumbnailCache.h"
#include "ui/qt/DesktopTypes.h"
#include <QObject>
#include <memory>

namespace HuhuCapture::Desktop { class SystemClipboard; class FileRevealer; }
namespace HuhuCapture::UI::Qt { class EditorWindow; class HistoryWindow; }

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
    void setSendAvailable(bool available);
    UI::Qt::EditorWindow* openEditor(const QImage& image, bool pinned = true);
    UI::Qt::HistoryWindow* openHistory();
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

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace HuhuCapture::Application
