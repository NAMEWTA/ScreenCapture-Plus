#include "ui/qt/TransferSettingsPanel.h"

#include "ui/qt/DesktopTheme.h"
#include "ui/qt/TransferPresentation.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QVBoxLayout>
#include <limits>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace Theme = DesktopTheme;
namespace Presentation = TransferPresentation;
}

struct TransferSettingsPanel::State {
    QPointer<Transfer::TransferService> service;
    DesktopTranslator translator;
    LanSettingsSnapshot snapshot;
    quint64 nextToken{1}, pending{};
    QMetaObject::Connection events, destroyed;
    bool subscribed{}, closed{};
    QLabel *heading{}, *live{}, *aliasLabel{}, *portLabel{}, *networkLabel{}, *identity{}, *directory{}, *hint{}, *status{};
    QCheckBox *enable{}, *discovery{};
    QLineEdit* alias{};
    QSpinBox* port{};
    QListWidget* interfaces{};
    QPushButton *apply{}, *close{}, *transfers{};
};

TransferSettingsPanel::TransferSettingsPanel(Transfer::TransferService& service, QWidget* parent)
    : QWidget(parent, ::Qt::Window), state_(std::make_unique<State>())
{
    qRegisterMetaType<LanSettingsSnapshot>(); qRegisterMetaType<LanSettingsChange>();
    state_->service = &service;
    setObjectName(QStringLiteral("transferSettingsPanel")); Theme::apply(this); Theme::initialSize(this, {680, 580});
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(20, 18, 20, 16); outer->setSpacing(12);
    state_->heading = Theme::label(this, 22); state_->heading->setFont(Theme::font(22, QFont::DemiBold));
    state_->live = Theme::label(this, 12, true); state_->live->setObjectName(QStringLiteral("lanServiceState"));
    outer->addWidget(state_->heading); outer->addWidget(state_->live);
    state_->enable = new QCheckBox(this); state_->enable->setObjectName(QStringLiteral("lanEnable")); outer->addWidget(state_->enable);
    auto* form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(16); form->setVerticalSpacing(10);
    state_->aliasLabel = Theme::label(this); state_->portLabel = Theme::label(this);
    state_->alias = new QLineEdit(this); state_->alias->setObjectName(QStringLiteral("lanAlias")); state_->alias->setMaxLength(256);
    state_->alias->setMinimumHeight(32); state_->aliasLabel->setBuddy(state_->alias);
    state_->port = new QSpinBox(this); state_->port->setObjectName(QStringLiteral("lanPort")); state_->port->setRange(1, 65535);
    state_->port->setValue(53317); state_->port->setMinimumHeight(32); state_->portLabel->setBuddy(state_->port);
    form->addRow(state_->aliasLabel, state_->alias); form->addRow(state_->portLabel, state_->port); outer->addLayout(form);
    state_->networkLabel = Theme::label(this); outer->addWidget(state_->networkLabel);
    state_->interfaces = new QListWidget(this); state_->interfaces->setObjectName(QStringLiteral("lanInterfaces"));
    state_->interfaces->setMinimumHeight(96); state_->interfaces->setMaximumHeight(150); state_->interfaces->setUniformItemSizes(true);
    state_->networkLabel->setBuddy(state_->interfaces); outer->addWidget(state_->interfaces, 1);
    state_->discovery = new QCheckBox(this); state_->discovery->setObjectName(QStringLiteral("lanDiscovery")); outer->addWidget(state_->discovery);
    state_->identity = Theme::label(this, 12, true); state_->identity->setWordWrap(true);
    state_->identity->setTextInteractionFlags(::Qt::TextSelectableByMouse | ::Qt::TextSelectableByKeyboard);
    state_->identity->setObjectName(QStringLiteral("lanLocalFingerprint")); outer->addWidget(state_->identity);
    state_->directory = Theme::label(this, 12, true); state_->directory->setWordWrap(true); outer->addWidget(state_->directory);
    state_->hint = Theme::label(this, 12, true); state_->hint->setWordWrap(true); outer->addWidget(state_->hint);
    state_->status = Theme::label(this, 12); state_->status->setWordWrap(true); state_->status->setObjectName(QStringLiteral("lanSettingsStatus"));
    outer->addWidget(state_->status);
    auto* actions = new QHBoxLayout;
    state_->transfers = Theme::button(this, "lanOpenTransfers"); state_->close = Theme::button(this, "lanSettingsClose");
    state_->apply = Theme::button(this, "lanApply", true);
    actions->addWidget(state_->transfers); actions->addStretch(); actions->addWidget(state_->close); actions->addWidget(state_->apply); outer->addLayout(actions);
    connect(state_->apply, &QPushButton::clicked, this, [this] { requestApply(); });
    connect(state_->close, &QPushButton::clicked, this, &QWidget::close);
    connect(state_->transfers, &QPushButton::clicked, this, &TransferSettingsPanel::transfersRequested);
    connect(state_->enable, &QCheckBox::toggled, this, [this] { updateActions(); });
    subscribe();
    refreshLanguage(); setSnapshot({});
}

TransferSettingsPanel::~TransferSettingsPanel()
{
    if (state_->service) disconnect(state_->service, nullptr, this, nullptr);
    for (auto* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
}

void TransferSettingsPanel::subscribe()
{
    if (!state_->service || state_->subscribed) return;
    state_->subscribed = true;
    state_->events = connect(state_->service, &Transfer::TransferService::eventReady, this,
        [this](const Transfer::Event&) { updateServiceState(); });
    state_->destroyed = connect(state_->service, &QObject::destroyed, this,
        [this] { state_->service.clear(); state_->subscribed = false; updateServiceState(); });
}

void TransferSettingsPanel::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); refreshLanguage(); }

void TransferSettingsPanel::refreshLanguage()
{
    auto text = [this](QStringView key) { return Presentation::text(state_->translator, key); };
    setWindowTitle(text(u"lan.settings")); state_->heading->setText(windowTitle());
    state_->enable->setText(text(u"lan.enable")); state_->discovery->setText(text(u"lan.discovery"));
    state_->aliasLabel->setText(text(u"lan.deviceName")); state_->portLabel->setText(text(u"lan.port"));
    state_->networkLabel->setText(text(u"lan.interfaces"));
    state_->identity->setText(text(u"lan.fingerprint") + QStringLiteral(": ") + state_->snapshot.fingerprint);
    state_->directory->setText(text(u"lan.saveTo").arg(state_->snapshot.archiveDirectory));
    state_->hint->setText(text(u"lan.settingsHint"));
    state_->apply->setText(text(u"lan.apply")); state_->close->setText(text(u"lan.close")); state_->transfers->setText(text(u"lan.title"));
    for (auto* button : {state_->apply, state_->close, state_->transfers}) button->setAccessibleName(button->text());
    for (int index = 0; index < state_->interfaces->count(); ++index) {
        auto* item = state_->interfaces->item(index);
        auto value = item->data(::Qt::UserRole + 1).toString();
        if (item->data(::Qt::UserRole + 2).toBool()) value += QStringLiteral(" · ") + text(u"lan.virtualNetwork");
        if (!item->flags().testFlag(::Qt::ItemIsEnabled)) value += QStringLiteral(" · ") + text(u"lan.unavailable");
        item->setText(value); item->setToolTip(value);
    }
    updateServiceState();
}

void TransferSettingsPanel::setSnapshot(const LanSettingsSnapshot& snapshot)
{
    state_->snapshot = snapshot;
    if (!state_->pending) {
        const QSignalBlocker enable(state_->enable), discovery(state_->discovery);
        state_->enable->setChecked(snapshot.enabled); state_->discovery->setChecked(snapshot.discovery);
        state_->alias->setText(snapshot.alias); state_->port->setValue(snapshot.port ? snapshot.port : 53317);
        state_->interfaces->clear();
        // The host enumerates real adapters and supplies eligibility. A stale
        // selected adapter stays visibly unavailable, never silently replaced
        // by a wildcard or a different VPN/physical interface.
        for (const auto& network : snapshot.interfaces) {
            auto* item = new QListWidgetItem(state_->interfaces);
            item->setData(::Qt::UserRole, network.address);
            item->setData(::Qt::UserRole + 1, network.name + QStringLiteral(" · ") + network.address);
            item->setData(::Qt::UserRole + 2, network.virtualNetwork);
            item->setFlags(network.available ? ::Qt::ItemIsEnabled | ::Qt::ItemIsSelectable | ::Qt::ItemIsUserCheckable :
                ::Qt::ItemIsSelectable | ::Qt::ItemIsUserCheckable);
            item->setCheckState(network.selected ? ::Qt::Checked : ::Qt::Unchecked);
        }
    }
    if (!snapshot.unavailableReason.isEmpty()) showStatus(snapshot.unavailableReason, true);
    refreshLanguage(); updateActions();
}

const LanSettingsSnapshot& TransferSettingsPanel::snapshot() const noexcept { return state_->snapshot; }
bool TransferSettingsPanel::changePending() const noexcept { return state_->pending != 0; }

bool TransferSettingsPanel::requestApply()
{
    if (!state_->service || !state_->snapshot.canConfigure || state_->pending ||
        state_->nextToken == std::numeric_limits<quint64>::max()) return false;
    LanSettingsChange change;
    change.enabled = state_->enable->isChecked(); change.discovery = state_->discovery->isChecked();
    change.alias = state_->alias->text().trimmed(); change.port = static_cast<quint16>(state_->port->value());
    bool unavailable{};
    for (int index = 0; index < state_->interfaces->count(); ++index) {
        const auto* item = state_->interfaces->item(index);
        if (item->checkState() == ::Qt::Checked) {
            unavailable = unavailable || !item->flags().testFlag(::Qt::ItemIsEnabled);
            change.addresses.append(item->data(::Qt::UserRole).toString());
        }
    }
    if (change.enabled && (change.alias.isEmpty() || change.alias.toUtf8().size() > 256 || change.alias.contains(QChar::Null) ||
        change.addresses.isEmpty() || change.addresses.size() > 16 || unavailable)) {
        showStatus(Presentation::text(state_->translator, u"lan.settingsInvalid"), true); return false;
    }
    state_->pending = state_->nextToken++; updateActions();
    showStatus(Presentation::text(state_->translator, u"lan.applying"), false);
    emit changeRequested(state_->pending, change); return true;
}

void TransferSettingsPanel::completeChange(quint64 token, DesktopResult result,
    const LanSettingsSnapshot& effective, const QString& message)
{
    if (!state_->pending || token != state_->pending) return;
    state_->pending = 0; setSnapshot(effective);
    showStatus(message.isEmpty() ? Presentation::text(state_->translator, result == DesktopResult::Succeeded ?
        u"lan.settingsSaved" : result == DesktopResult::Cancelled ? u"lan.cancelled" : u"lan.failed") : message,
        result == DesktopResult::Failed);
    updateActions();
}

void TransferSettingsPanel::updateServiceState()
{
    state_->live->setText(Presentation::serviceState(state_->translator,
        state_->service ? state_->service->state() : Transfer::ServiceState::Closed));
    updateActions();
}

void TransferSettingsPanel::updateActions()
{
    const bool available = state_->service && state_->service->state() != Transfer::ServiceState::Closed &&
        state_->snapshot.canConfigure && !state_->pending;
    state_->enable->setEnabled(available); state_->apply->setEnabled(available);
    const bool fields = available && state_->enable->isChecked();
    state_->alias->setEnabled(fields); state_->port->setEnabled(fields);
    state_->interfaces->setEnabled(fields); state_->discovery->setEnabled(fields);
    state_->transfers->setEnabled(state_->service != nullptr);
}

void TransferSettingsPanel::showStatus(const QString& text, bool error)
{
    state_->status->setText(text); state_->status->setProperty("error", error); Theme::repolish(state_->status);
}

void TransferSettingsPanel::closeEvent(QCloseEvent* event)
{
    disconnect(state_->events); disconnect(state_->destroyed); state_->subscribed = false;
    event->accept(); const bool notify = !state_->closed; state_->closed = true;
    if (notify) emit viewClosed();
}

void TransferSettingsPanel::showEvent(QShowEvent* event)
{ QWidget::showEvent(event); state_->closed = false; subscribe(); updateServiceState(); }

void TransferSettingsPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) refreshLanguage();
}

} // namespace HuhuCapture::UI::Qt
