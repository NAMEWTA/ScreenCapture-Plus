#pragma once

#include "ui/qt/DesktopTypes.h"
#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

class AboutPanel final : public QWidget {
    Q_OBJECT
public:
    explicit AboutPanel(QWidget* parent = nullptr);
    ~AboutPanel() override;
    void setTranslator(DesktopTranslator translator);
    void refreshLanguage();
    void setApplicationVersion(const QString& version);
    void setUpdateSnapshot(const AboutUpdateSnapshot& snapshot);
    void setLinksEnabled(bool enabled);

Q_SIGNALS:
    void downloadRequested();
    void openUpdateFolderRequested(const QString& file);
    void openLinkRequested(const QUrl& url);

private:
    struct State;
    std::unique_ptr<State> state_;
    QString translated(QStringView key) const;
    void refreshUpdate();
};

} // namespace HuhuCapture::UI::Qt
