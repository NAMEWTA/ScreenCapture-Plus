#pragma once

#include "services/transfer/TransferService.h"
#include "ui/qt/DesktopTypes.h"
#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

// A decision view for one real ReceiveOffer. Closing an unanswered prompt
// submits an explicit refusal. It never owns a receive body or accepts on focus.
class ReceivePrompt final : public QWidget {
    Q_OBJECT
public:
    ReceivePrompt(Transfer::TransferService& service, const Transfer::Event& offer,
        const QString& archiveDirectory, QWidget* parent = nullptr);
    ~ReceivePrompt() override;
    void setTranslator(DesktopTranslator translator);
    void refreshLanguage();
    const QString& sessionId() const noexcept;
    QStringList selectedFiles() const;
    bool decisionPending() const noexcept;
    bool answered() const noexcept;
    Transfer::Submission acceptSelected();
    Transfer::Submission reject();

Q_SIGNALS:
    void decisionSubmitted(const QString& sessionId, bool accepted);
    void viewClosed();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void consume(const Transfer::Event& event);
    void updateActions();
    Transfer::Submission decide(QStringList ids);
};

} // namespace HuhuCapture::UI::Qt
