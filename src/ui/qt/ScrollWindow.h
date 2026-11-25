#pragma once

#include "capture/ScrollCaptureSession.h"
#include "services/desktop/WindowActivation.h"
#include "ui/qt/DesktopTypes.h"
#include "ui/qt/EditorTypes.h"
#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {
// Shared long-capture presentation. The session owns actual capture/matching;
// the application routes completed full-resolution images through MediaWorkspace.
class ScrollWindow final : public QWidget {
    Q_OBJECT
public:
    explicit ScrollWindow(Capture::ScrollCaptureSession& session, QWidget* parent = nullptr);
    ~ScrollWindow() override;
    void setTranslator(DesktopTranslator translator);
    void refreshLanguage();
    Capture::Failure open();
    void activate(const QString& activationToken = {});
    void setExternalStopAvailable(bool available);
    bool canStop() const noexcept;
    bool stopCapture();
    bool hiddenForCapture() const noexcept;
    void completeOutput(quint64 token, OutputResult result, const QString& detail = {});
    Capture::ScrollCaptureSession* session() const noexcept;

Q_SIGNALS:
    void outputRequested(quint64 token, HuhuCapture::UI::Qt::OutputIntent intent, const QImage& image);
    void outputCancelRequested(quint64 token);
    void closed();
    void activationFinished(const HuhuCapture::Desktop::WindowActivationResult& result);
    void captureStateChanged();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void refresh();
    void start();
    void output(OutputIntent intent);
    void prepareNativeFrame(quint64 token, quint64 sample);
    void acknowledgeHidden();
};
} // namespace HuhuCapture::UI::Qt
