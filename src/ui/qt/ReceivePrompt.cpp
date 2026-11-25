#include "ui/qt/ReceivePrompt.h"

#include "ui/qt/DesktopTheme.h"
#include "ui/qt/TransferPresentation.h"
#include <QCloseEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QPointer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace Theme = DesktopTheme;
namespace Presentation = TransferPresentation;
}

struct ReceivePrompt::State {
    QPointer<Transfer::TransferService> service;
    DesktopTranslator translator;
    Transfer::Event offer;
    QString session, directory;
    Transfer::RequestId decision{};
    bool answered{}, pending{}, ended{}, constructing{true}, closed{};
    QLabel *heading{}, *sender{}, *identity{}, *destination{}, *hint{}, *status{};
    QTreeWidget* files{};
    QPushButton *accept{}, *reject{};
};

ReceivePrompt::ReceivePrompt(Transfer::TransferService& service, const Transfer::Event& offer,
    const QString& archiveDirectory, QWidget* parent)
    : QWidget(parent, ::Qt::Window), state_(std::make_unique<State>())
{
    state_->service = &service; state_->offer = offer; state_->directory = archiveDirectory;
    state_->session = offer.data.value(QStringLiteral("sessionId")).toString();
    setObjectName(QStringLiteral("receivePrompt")); Theme::apply(this); Theme::initialSize(this, {660, 480});
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(20, 18, 20, 16); outer->setSpacing(12);
    state_->heading = Theme::label(this, 22); state_->heading->setFont(Theme::font(22, QFont::DemiBold));
    state_->sender = Theme::label(this); state_->sender->setWordWrap(true);
    state_->identity = Theme::label(this, 12, true); state_->identity->setWordWrap(true);
    state_->identity->setTextInteractionFlags(::Qt::TextSelectableByMouse | ::Qt::TextSelectableByKeyboard);
    state_->identity->setObjectName(QStringLiteral("receiveFingerprint"));
    state_->destination = Theme::label(this, 12, true); state_->destination->setWordWrap(true);
    state_->destination->setTextInteractionFlags(::Qt::TextSelectableByMouse);
    outer->addWidget(state_->heading); outer->addWidget(state_->sender); outer->addWidget(state_->identity);
    state_->files = new QTreeWidget(this); state_->files->setObjectName(QStringLiteral("receiveFiles"));
    state_->files->setRootIsDecorated(false); state_->files->setColumnCount(3);
    state_->files->setSelectionMode(QAbstractItemView::SingleSelection);
    state_->files->setUniformRowHeights(true); state_->files->setFont(Theme::font());
    state_->files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    state_->files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    state_->files->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    const auto files = offer.data.value(QStringLiteral("files")).toArray();
    state_->ended = offer.kind != Transfer::EventKind::ReceiveOffer || state_->session.isEmpty() ||
        files.isEmpty() || files.size() > static_cast<qsizetype>(Transfer::TransferLimits::maximumFiles);
    if (!state_->ended) for (const auto& value : files) {
        const auto file = value.toObject();
        auto* item = new QTreeWidgetItem(state_->files);
        item->setText(0, file.value(QStringLiteral("fileName")).toString()); item->setToolTip(0, item->text(0));
        item->setData(0, ::Qt::UserRole, file.value(QStringLiteral("id")).toString());
        const auto type = Presentation::kind(file);
        item->setText(1, type.isEmpty() ? file.value(QStringLiteral("fileType")).toString() : type);
        const auto size = file.value(QStringLiteral("size")).toInteger(-1);
        item->setText(2, size >= 0 ? Presentation::bytes(static_cast<std::uint64_t>(size)) : QString());
        const bool supported = !type.isEmpty() && size > 0 && !item->data(0, ::Qt::UserRole).toString().isEmpty();
        item->setFlags(supported ? ::Qt::ItemIsEnabled | ::Qt::ItemIsSelectable | ::Qt::ItemIsUserCheckable : ::Qt::ItemIsSelectable);
        if (supported) item->setCheckState(0, ::Qt::Checked);
    }
    outer->addWidget(state_->files, 1); outer->addWidget(state_->destination);
    state_->hint = Theme::label(this, 12, true); state_->hint->setWordWrap(true); outer->addWidget(state_->hint);
    state_->status = Theme::label(this, 12); state_->status->setWordWrap(true);
    state_->status->setObjectName(QStringLiteral("receiveStatus")); outer->addWidget(state_->status);
    auto* actions = new QHBoxLayout; actions->addStretch();
    state_->reject = Theme::button(this, "receiveReject"); state_->accept = Theme::button(this, "receiveAccept", true);
    actions->addWidget(state_->reject); actions->addWidget(state_->accept); outer->addLayout(actions);
    connect(state_->files, &QTreeWidget::itemChanged, this, [this] { updateActions(); });
    connect(state_->accept, &QPushButton::clicked, this, [this] { (void)acceptSelected(); });
    connect(state_->reject, &QPushButton::clicked, this, [this] { (void)reject(); });
    connect(&service, &Transfer::TransferService::eventReady, this, &ReceivePrompt::consume);
    connect(&service, &QObject::destroyed, this, [this] {
        state_->service.clear(); state_->ended = true; state_->pending = false;
        state_->status->setText(Presentation::text(state_->translator, u"lan.closed")); updateActions();
    });
    if (const auto snapshot = service.viewSnapshot())
        for (const auto& event : snapshot->events) consume(event);
    state_->constructing = false;
    refreshLanguage(); updateActions();
}

ReceivePrompt::~ReceivePrompt()
{
    // Parent destruction can bypass closeEvent. A pending prompt is never
    // implicitly accepted; normal application shutdown also stops the service.
    if (state_->service) {
        disconnect(state_->service, nullptr, this, nullptr);
        if (!state_->answered && !state_->ended && !state_->session.isEmpty())
            (void)state_->service->accept(state_->session, {});
    }
    for (auto* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
}

void ReceivePrompt::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); refreshLanguage(); }

void ReceivePrompt::refreshLanguage()
{
    auto translated = [this](QStringView key) { return Presentation::text(state_->translator, key); };
    setWindowTitle(translated(u"lan.receiveTitle")); state_->heading->setText(windowTitle());
    state_->sender->setText(translated(u"lan.fromDevice").arg(state_->offer.data.value(QStringLiteral("alias")).toString(),
        state_->offer.data.value(QStringLiteral("peerIp")).toString()));
    state_->identity->setText(translated(u"lan.fingerprint") + QStringLiteral(": ") +
        state_->offer.data.value(QStringLiteral("fingerprint")).toString());
    state_->destination->setText(translated(u"lan.saveTo").arg(state_->directory));
    state_->hint->setText(translated(u"lan.receiveHint"));
    state_->files->setHeaderLabels({translated(u"lan.file"), translated(u"lan.type"), translated(u"lan.size")});
    state_->accept->setText(translated(u"lan.accept")); state_->reject->setText(translated(u"lan.decline"));
    state_->accept->setAccessibleName(state_->accept->text()); state_->reject->setAccessibleName(state_->reject->text());
    updateActions();
}

const QString& ReceivePrompt::sessionId() const noexcept { return state_->session; }
bool ReceivePrompt::decisionPending() const noexcept { return state_->pending; }
bool ReceivePrompt::answered() const noexcept { return state_->answered; }

QStringList ReceivePrompt::selectedFiles() const
{
    QStringList result;
    for (int index = 0; index < state_->files->topLevelItemCount(); ++index) {
        const auto* item = state_->files->topLevelItem(index);
        if (item->flags().testFlag(::Qt::ItemIsEnabled) && item->checkState(0) == ::Qt::Checked)
            result.append(item->data(0, ::Qt::UserRole).toString());
    }
    return result;
}

Transfer::Submission ReceivePrompt::decide(QStringList ids)
{
    if (!state_->service || state_->answered || state_->pending || state_->ended) return {HUHU_CLOSED, 0};
    const QPointer<ReceivePrompt> lifetime(this);
    const bool accepting = !ids.isEmpty();
    const auto session = state_->session;
    const auto result = state_->service->accept(session, std::move(ids));
    if (!lifetime) return result;
    if (result) {
        state_->decision = result.request; state_->pending = true; state_->answered = true;
        state_->status->setText(Presentation::text(state_->translator, accepting ? u"lan.checkingStorage" : u"lan.declining"));
        updateActions(); emit decisionSubmitted(session, accepting);
    } else {
        state_->status->setProperty("error", true);
        state_->status->setText(Presentation::status(state_->translator, result.status)); Theme::repolish(state_->status);
        updateActions();
    }
    return result;
}

Transfer::Submission ReceivePrompt::acceptSelected()
{
    auto ids = selectedFiles();
    if (ids.isEmpty()) return {HUHU_INVALID_ARGUMENT, 0};
    return decide(std::move(ids));
}

Transfer::Submission ReceivePrompt::reject()
{
    const QPointer<ReceivePrompt> lifetime(this);
    const auto result = decide({});
    if (lifetime && result) close();
    return result;
}

void ReceivePrompt::consume(const Transfer::Event& event)
{
    if (event.generation != state_->offer.generation) return;
    if (event.kind == Transfer::EventKind::Disabled) {
        state_->ended = true; state_->pending = false;
        state_->status->setText(Presentation::status(state_->translator, event.status == HUHU_OK ? HUHU_CLOSED : event.status));
        updateActions(); return;
    }
    if (event.data.value(QStringLiteral("sessionId")).toString() != state_->session) return;
    if (event.kind == Transfer::EventKind::ReceiveDecisionQueued) {
        state_->decision = event.request; state_->answered = true; state_->pending = true;
    } else if (event.kind == Transfer::EventKind::CommandFinished && event.request == state_->decision) {
        state_->pending = false; state_->answered = true;
        if (event.status == HUHU_OK) {
            if (!state_->constructing) { close(); return; }
        } else {
            state_->status->setProperty("error", true);
            state_->status->setText(Presentation::status(state_->translator, event.status)); Theme::repolish(state_->status);
        }
    } else if (event.kind == Transfer::EventKind::ReceiveFinished) {
        state_->pending = false; state_->ended = true; state_->answered = true;
        const auto status = event.data.value(QStringLiteral("localStatus")).toInt(event.status);
        state_->status->setText(Presentation::status(state_->translator, status));
        state_->status->setProperty("error", status != HUHU_OK && status != HUHU_CANCELLED); Theme::repolish(state_->status);
    }
    updateActions();
}

void ReceivePrompt::updateActions()
{
    const bool available = state_->service && state_->service->state() == Transfer::ServiceState::Enabled &&
        !state_->answered && !state_->pending && !state_->ended;
    state_->accept->setEnabled(available && !selectedFiles().isEmpty());
    state_->reject->setEnabled(available); state_->files->setEnabled(available);
}

void ReceivePrompt::closeEvent(QCloseEvent* event)
{
    if (!state_->answered && !state_->ended && state_->service) {
        const QPointer<ReceivePrompt> lifetime(this);
        const auto result = decide({});
        if (!lifetime) return;
        // A saturated command queue is not a delivered refusal. Keep the
        // actionable prompt visible so the user can retry instead of lying.
        if (!result && result.status != HUHU_CLOSED && result.status != HUHU_INVALID_STATE) { event->ignore(); return; }
    }
    if (state_->service) disconnect(state_->service, nullptr, this, nullptr);
    const bool notify = !state_->closed; state_->closed = true; event->accept();
    if (notify) emit viewClosed();
}

void ReceivePrompt::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) refreshLanguage();
}

} // namespace HuhuCapture::UI::Qt
