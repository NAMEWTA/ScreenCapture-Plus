#include "ui/qt/CaptureWindow.h"
#include "ui/qt/CaptureCanvas.h"
#include "ui/qt/ToolbarControls.h"

#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace HuhuCapture::UI::Qt {
namespace {
using Capture::Action;
using Capture::Stage;
using ToolbarControls::ToolbarStrip;
struct ButtonSpec { QStringView key; char16_t glyph{}; std::optional<Action> action; };
constexpr std::array buttons{
    ButtonSpec{u"cap.mark", u'\ue97f', Action::Edit},
    ButtonSpec{u"cap.long", u'\ue73e', Action::Scroll},
    ButtonSpec{u"cap.video", u'\ue660', Action::Record},
    ButtonSpec{u"cap.qrcode", u'\ue71e', Action::Qr},
    ButtonSpec{u"clipboard.title", u'\0', Action::History},
    ButtonSpec{u"tool.close", u'\ue62d', {}},
    ButtonSpec{u"tool.save", u'\ue608', Action::SaveAs},
    ButtonSpec{u"tool.clipboard", u'\ue6ad', Action::Clipboard}};
Capture::Failure failure(Capture::Failure::Code code, QString detail) { return {code, 0, std::move(detail)}; }
QString fallback(QStringView key)
{
    if (key == u"cap.title") return QStringLiteral("Screen capture");
    if (key == u"cap.authorizedSource") return QStringLiteral("This image was provided by your desktop. Selection and position are relative to the image.");
    if (key == u"cap.sourcePosition") return QStringLiteral("Image position");
    if (key == u"cap.mark") return QStringLiteral("Annotate Image");
    if (key == u"cap.long") return QStringLiteral("Scrolling Screenshot");
    if (key == u"cap.video") return QStringLiteral("Record Screen");
    if (key == u"cap.qrcode") return QStringLiteral("QR Code Recognition");
    if (key == u"clipboard.title") return QStringLiteral("Capture History");
    if (key == u"tool.close") return QStringLiteral("Close");
    if (key == u"tool.save") return QStringLiteral("Save");
    if (key == u"tool.clipboard") return QStringLiteral("Clipboard");
    return key.toString();
}
}

struct CaptureWindow::State {
    CaptureWindow* owner;
    QPointer<Capture::CaptureSession> session;
    Translator translator;
    std::vector<QPointer<CaptureCanvas>> views;
    QPointer<CaptureCanvas> active;
    QPointer<ToolbarStrip> toolbar;
    QPointer<QLabel> status;
    QPointer<QLabel> sourceHint;
    std::array<QPointer<QToolButton>, buttons.size()> controls;
    Desktop::WindowActivation activation;
    quint64 activationRequest{};
    QString activationToken;
    bool presentation{true}, closed{}, refreshing{};
    explicit State(CaptureWindow* window, Capture::CaptureSession& source) : owner(window), session(&source) {}
    QString text(QStringView key) const
    {
        const auto translated = translator ? translator(key) : QString();
        return translated.isEmpty() || translated == key ? fallback(key) : translated;
    }
    CaptureCanvas* preferredView() const
    {
        if (!session || views.empty()) return nullptr;
        if (session->stage() == Stage::Selecting || session->pointerPressed()) {
            if (active) return active;
        }
        const auto& bounds = session->selection().bounds();
        const QRectF selection(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
        CaptureCanvas* best = active;
        qreal largest = -1;
        for (const auto& view : views) {
            if (!view) continue;
            const auto visible = selection.intersected(view->sourceExtent());
            const auto area = visible.width() * visible.height();
            if (area > largest) { largest = area; best = view; }
        }
        return best ? best : views.front().data();
    }
    void refresh()
    {
        if (refreshing || closed || !session || views.empty()) return;
        refreshing = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{refreshing};
        auto* view = preferredView();
        if (!view) return;
        for (const auto& candidate : views) if (candidate) candidate->setHintsVisible(candidate == view);
        if (toolbar->parentWidget() != view) { toolbar->setParent(view); status->setParent(view); }
        const auto stage = session->stage();
        for (std::size_t index = 0; index < controls.size(); ++index) {
            auto* button = controls[index].data();
            const auto& spec = buttons[index];
            const auto label = text(spec.key);
            auto tip = label;
            if (spec.action) {
                const auto capability = session->capability(*spec.action);
                button->setEnabled(stage == Stage::Adjusting && capability.canRequest());
                if (!capability.reason.isEmpty()) tip += QStringLiteral("\n") + capability.reason;
            } else button->setEnabled(stage != Stage::Closed);
            button->setText(label); button->setAccessibleName(label); button->setToolTip(tip);
        }
        toolbar->refreshTextWidths();
        auto area = view->localWorkArea();
        if (area.isEmpty()) area = view->rect();
        status->setText(session->failureText());
        status->setFixedWidth((std::max)(1, (std::min)(500, view->width() - 16)));
        status->setFixedHeight((std::max)(1, status->heightForWidth(status->width())));
        status->move((view->width() - status->width()) / 2, (std::max)(8, view->height() - status->height() - 8));
        const bool hasFailure = !session->failureText().isEmpty();
        status->setVisible(presentation && hasFailure);
        if (hasFailure) area.setBottom((std::max)(area.top() + ToolbarControls::buttonSize + 2, qreal(status->y() - 8)));
        const auto preferred = toolbar->sizeHint();
        const int width = (std::min)(qFloor(area.width()), preferred.width());
        toolbar->resize((std::max)(1, width), toolbar->heightForWidth(width));
        const auto selected = view->localSelection();
        constexpr qreal gap = 4;
        qreal left = selected.right() - toolbar->width();
        qreal top = selected.bottom() + gap;
        if (top + toolbar->height() > area.bottom()) {
            if (selected.top() - gap - toolbar->height() >= area.top()) top = selected.top() - gap - toolbar->height();
            else top = selected.bottom() - gap - toolbar->height();
        }
        left = std::clamp(left, area.left(), (std::max)(area.left(), area.right() - toolbar->width()));
        top = std::clamp(top, area.top(), (std::max)(area.top(), area.bottom() - toolbar->height()));
        toolbar->move(qRound(left), qRound(top));
        const bool ready = session->selection().hasSelection() && (stage == Stage::Adjusting || stage == Stage::Submitting);
        toolbar->setVisible(presentation && ready); toolbar->raise();
        status->raise();
        if (sourceHint) {
            sourceHint->setText(text(u"cap.authorizedSource"));
            sourceHint->setFixedWidth((std::max)(1, (std::min)(600, view->width() - 16)));
            sourceHint->setFixedHeight((std::max)(1, sourceHint->heightForWidth(sourceHint->width())));
            sourceHint->move((view->width() - sourceHint->width()) / 2, (std::max)(8, view->height() - sourceHint->height() - 8));
            sourceHint->setVisible(presentation && stage == Stage::Selecting); sourceHint->raise();
        }
        QVector<QRectF> overlays;
        for (const auto* widget : {static_cast<QWidget*>(toolbar.data()), static_cast<QWidget*>(status.data()), static_cast<QWidget*>(sourceHint.data())})
            if (widget && widget->isVisible()) overlays.push_back(widget->geometry());
        view->setOverlayAreas(std::move(overlays));
    }
};

CaptureWindow::CaptureWindow(Capture::CaptureSession& session, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, session))
{
    connect(&session, &Capture::CaptureSession::changed, this, [this] { state_->refresh(); });
    connect(&session, &Capture::CaptureSession::actionRequested, this, [this](const Capture::ActionRequest& request) {
        Q_EMIT actionRequested(request, dialogParent());
    });
    connect(&session, &Capture::CaptureSession::closed, this, &CaptureWindow::close);
    connect(&session, &QObject::destroyed, this, &CaptureWindow::close);
    connect(&state_->activation, &Desktop::WindowActivation::finished, this,
        [this](quint64 token, const Desktop::WindowActivationResult& result) {
            if (token != state_->activationRequest) return;
            state_->activationRequest = 0;
            Q_EMIT activationFinished(result);
        });
}
CaptureWindow::~CaptureWindow()
{
    state_->closed = true;
    for (const auto& view : state_->views) if (view) {
        view->removeEventFilter(this); disconnect(view, nullptr, this, nullptr);
        view->setEnabled(false); view->hide(); view->deleteLater();
    }
}
void CaptureWindow::setTranslator(Translator translator)
{ state_->translator = std::move(translator); refreshLanguage(); }
void CaptureWindow::refreshLanguage()
{
    for (const auto& view : state_->views) if (view) {
        view->setWindowTitle(state_->text(u"cap.title"));
        view->setSourcePositionLabel(state_->text(u"cap.sourcePosition"));
    }
    state_->refresh();
}
Capture::Failure CaptureWindow::open()
{
    if (!qApp || thread() != qApp->thread()) return failure(Capture::Failure::Code::Unavailable, QStringLiteral("Capture windows require the application GUI thread"));
    if (!state_->session || state_->session->stage() == Stage::Empty || state_->session->stage() == Stage::Closed)
        return failure(Capture::Failure::Code::InvalidSnapshot, QStringLiteral("There is no active captured image to select"));
    if (!state_->views.empty() && !state_->closed) return failure(Capture::Failure::Code::Busy, QStringLiteral("This capture presentation is already open"));
    if (state_->closed) {
        for (const auto& view : state_->views) if (view) {
            view->removeEventFilter(this); disconnect(view, nullptr, this, nullptr); view->deleteLater();
        }
        state_->views.clear(); state_->active.clear(); state_->toolbar.clear(); state_->status.clear(); state_->sourceHint.clear();
        for (auto& control : state_->controls) control.clear();
        state_->closed = false; state_->presentation = true;
    }
    const auto& snapshot = state_->session->snapshot();
    for (const auto& binding : snapshot.screens) {
        if (!binding.screen || QRectF(binding.screen->geometry()) != binding.logicalGeometry ||
            !qFuzzyCompare(binding.screen->devicePixelRatio(), binding.pixelsPerDipX) ||
            !qFuzzyCompare(binding.screen->devicePixelRatio(), binding.pixelsPerDipY))
            return failure(Capture::Failure::Code::SourceChanged, QStringLiteral("The display layout changed before the selection window opened"));
    }
    try {
        auto makeView = [this](std::optional<Capture::ScreenBinding> screen) {
            auto view = std::make_unique<CaptureCanvas>(*state_->session, screen);
            view->setWindowTitle(state_->text(u"cap.title"));
            view->setSourcePositionLabel(state_->text(u"cap.sourcePosition"));
            if (screen) {
                view->setWindowFlags(::Qt::Tool | ::Qt::FramelessWindowHint | ::Qt::WindowStaysOnTopHint);
                view->winId(); view->windowHandle()->setScreen(screen->screen);
                view->setGeometry(screen->logicalGeometry.toRect());
            } else {
                view->setWindowFlags(::Qt::Window);
                const auto* display = QGuiApplication::primaryScreen();
                const QSize limit = display ? display->availableGeometry().size() - QSize(40, 60) : QSize(1200, 800);
                view->resize(view->sizeHint().boundedTo(limit.expandedTo({200, 200})));
            }
            view->installEventFilter(this);
            connect(view.get(), &CaptureCanvas::interacted, this, [this](CaptureCanvas* active) { state_->active = active; });
            state_->views.push_back(view.get());
            return view.release();
        };
        if (snapshot.coordinates == Capture::CoordinateSpace::DesktopPixels) {
            for (const auto& screen : snapshot.screens) makeView(screen);
        } else makeView({});
        if (state_->views.empty()) return failure(Capture::Failure::Code::InvalidSnapshot, QStringLiteral("The source has no capture view"));
        state_->active = state_->views.front();
        for (const auto& view : state_->views) if (view->sourceExtent().contains(snapshot.cursor)) state_->active = view;
        auto* host = state_->active.data();
        state_->toolbar = new ToolbarStrip(host); state_->toolbar->setObjectName(QStringLiteral("capture.toolbar"));
        const auto family = ToolbarControls::iconFontFamily();
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            if (index == 5) state_->toolbar->add(new ToolbarControls::ToolbarDivider(state_->toolbar), 1);
            const auto& spec = buttons[index];
            auto* button = ToolbarControls::makeButton(state_->toolbar, spec.key.toString(), spec.glyph, family);
            // The old history glyph belongs to Windows' Segoe MDL2 Assets and
            // is absent from the shared icon font. Use Qt's real list icon on
            // both systems instead of emitting an unsupported private glyph.
            if (spec.action == Action::History) button->setIcon(qApp->style()->standardIcon(QStyle::SP_FileDialogListView));
            state_->controls[index] = button; state_->toolbar->add(button);
            connect(button, &QToolButton::clicked, this, [this, action = spec.action] {
                if (!state_->session) return;
                if (action) state_->session->request(*action); else close();
            });
        }
        state_->status = new QLabel(host); state_->status->setObjectName(QStringLiteral("capture.failure"));
        state_->status->setTextFormat(::Qt::PlainText); state_->status->setWordWrap(true);
        state_->status->setAttribute(::Qt::WA_TransparentForMouseEvents);
        state_->status->setStyleSheet(QStringLiteral("QLabel { background: #fff2f0; color: #a8071a; padding: 8px; border: 1px solid #ffccc7; }"));
        if (!host->desktopView()) {
            state_->sourceHint = new QLabel(host); state_->sourceHint->setObjectName(QStringLiteral("capture.sourceHint"));
            state_->sourceHint->setTextFormat(::Qt::PlainText); state_->sourceHint->setWordWrap(true);
            state_->sourceHint->setAttribute(::Qt::WA_TransparentForMouseEvents);
            state_->sourceHint->setStyleSheet(QStringLiteral("QLabel { background: #e6f4ff; color: #333333; padding: 8px; border: 1px solid #91caff; }"));
        }
        for (const auto& view : state_->views) view->show();
        state_->refresh(); activate();
        return {};
    } catch (const std::bad_alloc&) {
        for (const auto& view : state_->views) if (view) { view->hide(); view->deleteLater(); }
        state_->views.clear(); state_->active.clear(); state_->toolbar.clear(); state_->status.clear(); state_->sourceHint.clear();
        return failure(Capture::Failure::Code::Allocation, QStringLiteral("Insufficient memory to open capture selection"));
    }
}
void CaptureWindow::activate(const QString& activationToken)
{
    if (!activationToken.isEmpty()) state_->activationToken = activationToken;
    if (state_->closed || !state_->presentation) return;
    if (auto* view = state_->preferredView()) {
        view->raise(); view->setFocus(::Qt::OtherFocusReason);
        state_->activationRequest = state_->activation.request(view->windowHandle(), std::exchange(state_->activationToken, {}));
    }
}
void CaptureWindow::close()
{
    if (state_->closed) return;
    state_->closed = true;
    state_->activation.cancel(state_->activationRequest); state_->activationRequest = 0;
    state_->activationToken.clear();
    const QPointer<CaptureWindow> lifetime(this);
    const auto views = state_->views;
    for (const auto& view : views) {
        if (view) view->hide();
        if (!lifetime) return;
    }
    if (state_->session && state_->session->stage() != Stage::Closed) state_->session->cancel();
    if (lifetime) Q_EMIT closed();
}
QWidget* CaptureWindow::dialogParent() const { return state_->active; }
QWindow* CaptureWindow::windowHandle() const
{ return state_->active ? state_->active->windowHandle() : nullptr; }
void CaptureWindow::setPresentationVisible(bool visible)
{
    if (state_->closed) return;
    state_->presentation = visible;
    const QPointer<CaptureWindow> lifetime(this);
    const auto views = state_->views;
    for (const auto& view : views) {
        if (view) view->setVisible(visible);
        if (!lifetime) return;
    }
    state_->refresh(); if (visible) activate();
}
bool CaptureWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (qobject_cast<CaptureCanvas*>(watched)) {
        if (event->type() == QEvent::Close) {
            static_cast<QCloseEvent*>(event)->ignore(); close(); return true;
        }
        if (event->type() == QEvent::Resize)
            QTimer::singleShot(0, this, [this] { state_->refresh(); });
    }
    return QObject::eventFilter(watched, event);
}

} // namespace HuhuCapture::UI::Qt
