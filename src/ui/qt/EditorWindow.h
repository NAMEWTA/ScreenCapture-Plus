#pragma once

#include "ui/qt/EditorCanvas.h"
#include <QWidget>
#include <memory>

namespace HuhuCapture::UI::Qt {

// Production editor/pin window. The host supplies localization, preferences and
// real output services; this class never impersonates clipboard/archive success.
class EditorWindow final : public QWidget {
    Q_OBJECT
public:
    explicit EditorWindow(QWidget* parent = nullptr);
    ~EditorWindow() override;

    EditorCanvas* canvas() const noexcept;
    EditorFailure loadImage(const QImage& image);
    // One initial placement, before editing. Fits to this monitor's work area
    // and clamps only this first position; subsequent native moves remain free.
    bool applyInitialPlacement(QScreen* screen, QPoint imageTopLeft);
    void setTranslator(EditorTranslator translator);
    void refreshLanguage();
    void setOutputEnabled(OutputIntent intent, bool enabled);
    bool outputEnabled(OutputIntent intent) const noexcept;
    bool outputPending() const noexcept;
    void setCloseOnSuccessfulOutput(bool close) noexcept;
    bool iconFontAvailable() const noexcept;
    bool requestOutput(OutputIntent intent);

public slots:
    // GUI-thread completion; a stale/duplicate token has no effect. Cancel and
    // failure keep the original document, selection and full notes available.
    void completeOutput(quint64 token, OutputResult result, const QString& message = {});

signals:
    void outputRequested(quint64 token, OutputIntent intent, const QImage& image);
    void outputCancellationRequested(quint64 token);
    void outputFinished(OutputIntent intent, OutputResult result);
    void outputPendingChanged(bool pending);
    void operationFailed(EditorFailure failure);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    QString translated(QStringView key) const;
    void makeMainToolbar();
    void rebuildStyleToolbar();
    void updateStyleToolbar();
    void updateActions();
    void syncAnnotations();
    void selectNote(quint64 number, bool focus);
    void setNotesVisible(bool visible);
    void fitToWorkArea(bool includeNotes);
    void setBusy(bool busy);
    void showFailure(EditorFailure failure, const QString& message = {});
};

} // namespace HuhuCapture::UI::Qt
