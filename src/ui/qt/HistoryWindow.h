#pragma once

#include "ui/qt/DesktopTypes.h"

#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

// Presentation only. All disk access, media decoding, clipboard/reveal and
// transfers belong to the host. Closing this view never cancels global sends.
class HistoryWindow final : public QWidget {
    Q_OBJECT
public:
    explicit HistoryWindow(QWidget* parent = nullptr);
    ~HistoryWindow() override;
    void setTranslator(DesktopTranslator translator);
    void refreshLanguage();
    void setCapabilities(HistoryCapabilities capabilities);
    void setArchiveDirectory(const QString& path);
    bool requestReload();
    bool requestCommand(HistoryCommand command);
    bool requestClear();
    bool loading() const noexcept;
    bool commandPending() const noexcept;
    qsizetype entryCount() const noexcept;
    qsizetype page() const noexcept;
    qsizetype selectedEntry() const noexcept;
    quint64 thumbnailGeneration() const noexcept;
    void selectEntry(qsizetype index);

public Q_SLOTS:
    void completeReload(quint64 token, const HuhuCapture::Media::ListResult& result);
    // Null image means that the real decoder could not provide a cover.
    bool completeThumbnail(quint64 generation, const HuhuCapture::Media::Entry& entry, const QImage& image);
    void completeCommand(quint64 token, DesktopResult result, const QString& message = {});
    void completeRemoval(quint64 token, DesktopResult result, const QString& message = {});
    // Optional second confirmation for platforms unable to recycle. Only a
    // subset of the exact confirmed removal snapshot may be presented here.
    bool confirmPermanentRemoval(quint64 token, const QVector<HuhuCapture::Media::Entry>& entries);

Q_SIGNALS:
    void reloadRequested(quint64 token);
    // A single bounded page batch, never an unbounded task per history item.
    void thumbnailsRequested(quint64 generation, const QVector<HuhuCapture::Media::Entry>& entries, QSize maximumSize);
    void commandRequested(quint64 token, HistoryCommand command, const HuhuCapture::Media::Entry& entry);
    void removalRequested(quint64 token, const QVector<HuhuCapture::Media::Entry>& entries);
    void permanentRemovalConfirmed(quint64 token, const QVector<HuhuCapture::Media::Entry>& entries, bool approved);
    void viewClosed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    QString translated(QStringView key) const;
    void updateActions();
    void updateStatus(const QString& text, bool error);
    void rebuildPage(qsizetype page);
    void refreshRows();
    void updateSummary();
    bool reload(bool preserveStatus);
    bool handleKey(QKeyEvent* event);
};

} // namespace HuhuCapture::UI::Qt
