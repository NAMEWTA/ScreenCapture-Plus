#pragma once

#include "ui/qt/DesktopTypes.h"
#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {
class AboutPanel;

class SettingsWindow final : public QWidget {
    Q_OBJECT
public:
    enum class Page { General, Shortcut, About };
    explicit SettingsWindow(QWidget* parent = nullptr);
    ~SettingsWindow() override;
    void setTranslator(DesktopTranslator translator);
    void refreshLanguage();
    void setSnapshot(const SettingsSnapshot& snapshot);
    const SettingsSnapshot& snapshot() const noexcept;
    void setApplicationVersion(const QString& version);
    void setUpdateSnapshot(const AboutUpdateSnapshot& snapshot);
    void setPage(Page page);
    Page page() const noexcept;
    AboutPanel* aboutPanel() const noexcept;
    bool changePending() const noexcept;
    bool capturingShortcut() const noexcept;

public Q_SLOTS:
    void completeChange(quint64 token, DesktopResult result, const SettingsSnapshot& effective, const QString& message = {});

Q_SIGNALS:
    void changeRequested(quint64 token, const SettingChange& change);
    void shortcutCaptureActive(bool active);
    void openArchiveRequested();
    void lanSettingsRequested();
    void openLinkRequested(const QUrl& url);
    void downloadRequested();
    void openUpdateFolderRequested(const QString& file);
    // The host disconnects this view. Its update/download/transfer services
    // retain their own application lifetime and must not be canceled here.
    void viewClosed();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    QString translated(QStringView key) const;
    void makeGeneral();
    void makeShortcut();
    void refreshValues();
    void updateActions();
    bool requestChange(const SettingChange& change);
    void beginShortcutCapture();
    void endShortcutCapture();
    void showStatus(const QString& text, bool error);
};

} // namespace HuhuCapture::UI::Qt
