#pragma once

#include "services/transfer/TransferService.h"
#include "ui/qt/DesktopTypes.h"
#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

// One presentation of the application-owned service. It owns no network or
// archive workers. Every send/retry asks the host for new validated media.
class TransferWindow final : public QWidget {
    Q_OBJECT
public:
    explicit TransferWindow(Transfer::TransferService& service, QWidget* parent = nullptr);
    ~TransferWindow() override;
    void setTranslator(DesktopTranslator translator);
    void refreshLanguage();
    bool setMediaSelection(const QVector<Media::Entry>& entries);
    void setArchiveDirectory(const QString& directory);
    bool preparationPending() const noexcept;
    qsizetype deviceCount() const noexcept;
    qsizetype taskCount() const noexcept;
    QString selectedEndpoint() const;
    bool selectEndpoint(const QString& address, quint16 port);
    Transfer::Submission discover(const QString& address, quint16 port);

public Q_SLOTS:
    // Empty expectedFingerprint means the host has no saved trust for this
    // endpoint. A mismatched value is shown explicitly before confirmation.
    void completePeerReview(quint64 token, DesktopResult result,
        const QString& expectedFingerprint, const QString& message = {});
    // The host owns the confirmed intent independently of this view: prepare
    // on its worker, enqueue into the application service even after this view
    // closes, then report the actual Submission here if the view still exists.
    // No source lease is ever retained by the view.
    void completePreparation(quint64 token, int status, quint64 request);

Q_SIGNALS:
    void peerReviewRequested(quint64 token, const QJsonObject& peer);
    void prepareSendRequested(quint64 token, const QJsonObject& confirmedPeer,
        const QString& displayAlias, const QVector<HuhuCapture::Media::Entry>& entries);
    void receivedFileRequested(const QString& path);
    void receiveConfirmationRequested(const HuhuCapture::Transfer::Event& offer);
    void settingsRequested();
    void viewClosed();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    QString translated(QStringView key) const;
    void subscribe();
    void refreshSnapshot();
    void selectedPeerChanged();
    void updateActions();
    void showStatus(const QString& message, bool error);
    void confirmSend();
    void requestPreparation();
    void taskSelected();
    void cancelTask();
    void retryTask();
};

} // namespace HuhuCapture::UI::Qt
