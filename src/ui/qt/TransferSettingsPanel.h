#pragma once

#include "services/transfer/TransferService.h"
#include "ui/qt/DesktopTypes.h"
#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

struct LanInterface {
    QString name;
    QString address;
    bool available{};
    bool selected{};
    // This is a real host classification, never guessed from interface names.
    bool virtualNetwork{};
};

struct LanSettingsSnapshot {
    bool enabled{};
    bool canConfigure{};
    bool discovery{};
    QString alias;
    quint16 port{53317};
    QVector<LanInterface> interfaces;
    QString fingerprint;
    QString archiveDirectory;
    QString unavailableReason;
};

struct LanSettingsChange {
    bool enabled{};
    bool discovery{};
    QString alias;
    quint16 port{53317};
    QStringList addresses;
};

// Settings are committed by the application host, which owns persistent TLS
// identity, interface policy and service restart. This view never generates a
// new identity or starts a listener merely because it becomes visible.
class TransferSettingsPanel final : public QWidget {
    Q_OBJECT
public:
    explicit TransferSettingsPanel(Transfer::TransferService& service, QWidget* parent = nullptr);
    ~TransferSettingsPanel() override;
    void setTranslator(DesktopTranslator translator);
    void refreshLanguage();
    void setSnapshot(const LanSettingsSnapshot& snapshot);
    const LanSettingsSnapshot& snapshot() const noexcept;
    bool changePending() const noexcept;
    bool requestApply();

public Q_SLOTS:
    void completeChange(quint64 token, DesktopResult result,
        const LanSettingsSnapshot& effective, const QString& message = {});

Q_SIGNALS:
    void changeRequested(quint64 token, const HuhuCapture::UI::Qt::LanSettingsChange& change);
    void transfersRequested();
    void viewClosed();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void updateActions();
    void updateServiceState();
    void subscribe();
    void showStatus(const QString& text, bool error);
};

} // namespace HuhuCapture::UI::Qt

Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::LanSettingsSnapshot)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::LanSettingsChange)
