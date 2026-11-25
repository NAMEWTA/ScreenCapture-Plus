#include "ui/qt/TransferWindow.h"

#include "ui/qt/DesktopTheme.h"
#include "ui/qt/TransferPresentation.h"
#include <QCloseEvent>
#include <QHeaderView>
#include <QFileInfo>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace HuhuCapture::UI::Qt {
namespace {
namespace Theme = DesktopTheme;
namespace Presentation = TransferPresentation;

struct Task {
    std::optional<Transfer::Event> origin, decision, progress, terminal;
    std::map<QString, Transfer::ReceiveProgress> received;
    bool incoming{}, recovery{};
};

QString label(const QJsonObject& peer)
{
    const auto alias = peer.value(QStringLiteral("alias")).toString();
    return alias.isEmpty() ? Presentation::endpoint(peer) : alias;
}

std::uint64_t jsonBytes(const QJsonObject& data, const QString& name)
{
    const auto value = data.value(name).toInteger(-1);
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
}

QString taskStatus(const Task& task, const DesktopTranslator& translator)
{
    if (task.terminal) {
        const auto status = task.terminal->data.value(QStringLiteral("localStatus")).toInt(task.terminal->status);
        return Presentation::status(translator, status, task.terminal->data.value(QStringLiteral("httpStatus")).toInt());
    }
    if (task.incoming) {
        if (!task.received.empty()) {
            const auto allWritten = std::all_of(task.received.begin(), task.received.end(), [](const auto& value) {
                return value.second.bytes == value.second.total;
            });
            return Presentation::text(translator, allWritten ? u"lan.validating" : u"lan.receiving");
        }
        if (task.decision) {
            if (task.decision->kind == Transfer::EventKind::CommandFinished && task.decision->status != HUHU_OK)
                return Presentation::status(translator, task.decision->status);
            return Presentation::text(translator, task.decision->data.value(QStringLiteral("fileIds")).toArray().isEmpty() ?
                u"lan.declining" : task.decision->kind == Transfer::EventKind::ReceiveDecisionQueued ? u"lan.checkingStorage" : u"lan.waitingData");
        }
        return Presentation::text(translator, u"lan.awaitingYourDecision");
    }
    if (task.progress) {
        const auto& data = task.progress->data;
        const auto complete = jsonBytes(data, QStringLiteral("bytes")) >= jsonBytes(data, QStringLiteral("total"));
        if (data.value(QStringLiteral("phase")).toString() == QStringLiteral("hashing"))
            return Presentation::text(translator, complete ? u"lan.waitingReceiver" : u"lan.hashing");
        return Presentation::text(translator, complete ? u"lan.waitingCommit" : u"lan.sending");
    }
    return Presentation::text(translator, u"lan.queued");
}

QString taskProgress(const Task& task)
{
    if (task.terminal) return {};
    std::uint64_t current{}, total{};
    if (task.progress) {
        current = jsonBytes(task.progress->data, QStringLiteral("bytes"));
        total = jsonBytes(task.progress->data, QStringLiteral("total"));
    } else for (const auto& [id, progress] : task.received) {
        (void)id;
        if (progress.total > std::numeric_limits<std::uint64_t>::max() - total ||
            progress.bytes > std::numeric_limits<std::uint64_t>::max() - current) return {};
        current += progress.bytes; total += progress.total;
    }
    if (!total) return {};
    return Presentation::bytes(current) + QStringLiteral(" / ") + Presentation::bytes(total);
}

bool isTask(const Transfer::Event& event)
{
    switch (event.kind) {
    case Transfer::EventKind::SendQueued: case Transfer::EventKind::SendProgress: case Transfer::EventKind::SendFinished:
    case Transfer::EventKind::ReceiveOffer: case Transfer::EventKind::ReceiveDecisionQueued:
    case Transfer::EventKind::ReceiveProgress: case Transfer::EventKind::ReceiveFinished: return true;
    case Transfer::EventKind::CommandFinished:
        return event.data.value(QStringLiteral("command")).toString() == QStringLiteral("accept");
    default: return false;
    }
}

Media::Kind mediaKind(std::uint32_t kind)
{
    switch (kind) { case HUHU_GIF: return Media::Kind::Gif; case HUHU_MP4: return Media::Kind::Mp4; default: return Media::Kind::Png; }
}
}

struct TransferWindow::State {
    QPointer<Transfer::TransferService> service;
    DesktopTranslator translator;
    QMetaObject::Connection events, destroyed;
    QTimer refresh;
    QPointer<QMessageBox> confirmation;
    QVector<Media::Entry> selection;
    QMap<QString, QJsonObject> peers;
    QMap<QString, Task> tasks;
    QJsonObject selectedPeer;
    QString directory, expectedFingerprint, selectedPeerKey, preferredPeerKey;
    quint64 nextToken{1}, reviewToken{}, preparationToken{};
    Transfer::RequestId discoveryRequest{}, announcementRequest{};
    bool reviewed{}, reviewFailed{}, rebuilding{}, subscribed{}, retryAfterReview{}, closed{}, multicastAvailable{};
    QLabel *heading{}, *live{}, *selectionLabel{}, *peerIdentity{}, *trust{}, *tasksLabel{}, *status{}, *activityHint{};
    QTreeWidget *devices{}, *activity{};
    QListWidget* details{};
    QLineEdit *address{}, *pin{};
    QSpinBox* port{};
    QPushButton *probe{}, *verify{}, *send{}, *settings{}, *close{}, *cancel{}, *retry{}, *answer{}, *reveal{}, *refreshDevices{};
};

TransferWindow::TransferWindow(Transfer::TransferService& service, QWidget* parent)
    : QWidget(parent), state_(std::make_unique<State>())
{
    state_->service = &service;
    setObjectName(QStringLiteral("transferWindow")); Theme::apply(this); Theme::initialSize(this, {840, 700});
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(20, 18, 20, 12); outer->setSpacing(10);
    auto* heading = new QHBoxLayout;
    state_->heading = Theme::label(this, 22); state_->heading->setFont(Theme::font(22, QFont::DemiBold));
    state_->settings = Theme::button(this, "transferSettings"); state_->close = Theme::button(this, "transferClose");
    state_->refreshDevices = Theme::button(this, "transferRefresh"); state_->refreshDevices->setFixedSize(32, 32);
    state_->refreshDevices->setIcon(style()->standardIcon(QStyle::SP_BrowserReload)); state_->refreshDevices->setIconSize({18, 18});
    heading->addWidget(state_->heading, 1); heading->addWidget(state_->refreshDevices);
    heading->addWidget(state_->settings); heading->addWidget(state_->close); outer->addLayout(heading);
    state_->live = Theme::label(this, 12, true); outer->addWidget(state_->live);
    state_->selectionLabel = new Theme::ElidingLabel(this); state_->selectionLabel->setFont(Theme::font());
    state_->selectionLabel->setObjectName(QStringLiteral("transferSelection")); outer->addWidget(state_->selectionLabel);
    auto* splitter = new QSplitter(::Qt::Vertical, this); splitter->setChildrenCollapsible(false);
    auto* upper = new QWidget(splitter); auto* upperLayout = new QVBoxLayout(upper);
    upperLayout->setContentsMargins(0, 0, 0, 0); upperLayout->setSpacing(8);
    state_->devices = new QTreeWidget(upper); state_->devices->setObjectName(QStringLiteral("transferDevices"));
    state_->devices->setColumnCount(3); state_->devices->setRootIsDecorated(false); state_->devices->setUniformRowHeights(true);
    state_->devices->setSelectionMode(QAbstractItemView::SingleSelection); state_->devices->setMinimumHeight(100);
    state_->devices->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    state_->devices->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    state_->devices->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents); upperLayout->addWidget(state_->devices, 1);
    auto* manual = new QHBoxLayout;
    state_->address = new QLineEdit(upper); state_->address->setObjectName(QStringLiteral("transferAddress"));
    state_->address->setMaxLength(64); state_->address->setMinimumHeight(32);
    state_->port = new QSpinBox(upper); state_->port->setObjectName(QStringLiteral("transferPort"));
    state_->port->setRange(1, 65535); state_->port->setValue(53317); state_->port->setMinimumHeight(32);
    state_->probe = Theme::button(upper, "transferProbe"); state_->verify = Theme::button(upper, "transferVerify");
    manual->addWidget(state_->address, 1); manual->addWidget(state_->port); manual->addWidget(state_->probe); manual->addWidget(state_->verify);
    upperLayout->addLayout(manual);
    state_->peerIdentity = Theme::label(upper, 12, true); state_->peerIdentity->setWordWrap(true);
    state_->peerIdentity->setTextInteractionFlags(::Qt::TextSelectableByMouse | ::Qt::TextSelectableByKeyboard);
    state_->peerIdentity->setObjectName(QStringLiteral("transferPeerFingerprint")); upperLayout->addWidget(state_->peerIdentity);
    state_->trust = Theme::label(upper, 12, true); state_->trust->setWordWrap(true); state_->trust->setObjectName(QStringLiteral("transferTrust"));
    upperLayout->addWidget(state_->trust);
    auto* send = new QHBoxLayout;
    state_->pin = new QLineEdit(upper); state_->pin->setObjectName(QStringLiteral("transferPin")); state_->pin->setEchoMode(QLineEdit::Password);
    state_->pin->setMaxLength(128); state_->pin->setMinimumHeight(32);
    state_->send = Theme::button(upper, "transferSend", true);
    send->addWidget(state_->pin, 1); send->addWidget(state_->send); upperLayout->addLayout(send);
    auto* lower = new QWidget(splitter); auto* lowerLayout = new QVBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0); lowerLayout->setSpacing(8);
    state_->tasksLabel = Theme::label(lower); lowerLayout->addWidget(state_->tasksLabel);
    state_->activity = new QTreeWidget(lower); state_->activity->setObjectName(QStringLiteral("transferActivity"));
    state_->activity->setColumnCount(3); state_->activity->setRootIsDecorated(false); state_->activity->setUniformRowHeights(true);
    state_->activity->setSelectionMode(QAbstractItemView::SingleSelection); state_->activity->setMinimumHeight(100);
    state_->activity->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    state_->activity->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    state_->activity->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents); lowerLayout->addWidget(state_->activity, 1);
    state_->details = new QListWidget(lower); state_->details->setObjectName(QStringLiteral("transferDetails"));
    state_->details->setMinimumHeight(48); state_->details->setMaximumHeight(96); lowerLayout->addWidget(state_->details);
    auto* taskActions = new QHBoxLayout;
    state_->answer = Theme::button(lower, "transferAnswer"); state_->cancel = Theme::button(lower, "transferCancel");
    state_->retry = Theme::button(lower, "transferRetry"); state_->reveal = Theme::button(lower, "transferReveal");
    taskActions->addWidget(state_->answer); taskActions->addWidget(state_->cancel); taskActions->addWidget(state_->retry);
    taskActions->addStretch(); taskActions->addWidget(state_->reveal); lowerLayout->addLayout(taskActions);
    splitter->addWidget(upper); splitter->addWidget(lower); splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);
    state_->activityHint = Theme::label(this, 12, true); state_->activityHint->setWordWrap(true); outer->addWidget(state_->activityHint);
    state_->status = Theme::label(this, 12); state_->status->setObjectName(QStringLiteral("transferStatus"));
    state_->status->setWordWrap(true); outer->addWidget(state_->status);
    state_->refresh.setSingleShot(true); state_->refresh.setInterval(0);
    connect(&state_->refresh, &QTimer::timeout, this, &TransferWindow::refreshSnapshot);
    connect(state_->close, &QPushButton::clicked, this, &QWidget::close);
    connect(state_->settings, &QPushButton::clicked, this, &TransferWindow::settingsRequested);
    connect(state_->refreshDevices, &QPushButton::clicked, this, [this] {
        if (!state_->service || state_->announcementRequest) return;
        const auto result = state_->service->refreshDiscovery();
        if (result) state_->announcementRequest = result.request;
        showStatus(result ? translated(u"lan.refreshingDevices") : Presentation::status(state_->translator, result.status), !result);
        updateActions();
    });
    connect(state_->devices, &QTreeWidget::itemSelectionChanged, this, &TransferWindow::selectedPeerChanged);
    connect(state_->activity, &QTreeWidget::itemSelectionChanged, this, &TransferWindow::taskSelected);
    connect(state_->details, &QListWidget::itemSelectionChanged, this, [this] { updateActions(); });
    connect(state_->send, &QPushButton::clicked, this, &TransferWindow::confirmSend);
    connect(state_->probe, &QPushButton::clicked, this, [this] {
        (void)discover(state_->address->text().trimmed(), static_cast<quint16>(state_->port->value()));
    });
    connect(state_->address, &QLineEdit::returnPressed, state_->probe, &QPushButton::click);
    connect(state_->verify, &QPushButton::clicked, this, [this] {
        (void)discover(state_->selectedPeer.value(QStringLiteral("address")).toString(),
            static_cast<quint16>(state_->selectedPeer.value(QStringLiteral("port")).toInt()));
    });
    connect(state_->cancel, &QPushButton::clicked, this, &TransferWindow::cancelTask);
    connect(state_->retry, &QPushButton::clicked, this, &TransferWindow::retryTask);
    connect(state_->answer, &QPushButton::clicked, this, [this] {
        const auto* row = state_->activity->currentItem(); if (!row) return;
        const auto task = state_->tasks.value(row->data(0, ::Qt::UserRole).toString());
        if (task.incoming && task.origin && !task.decision && !task.terminal) emit receiveConfirmationRequested(*task.origin);
    });
    connect(state_->reveal, &QPushButton::clicked, this, [this] {
        const auto* item = state_->details->currentItem(); if (!item) return;
        const auto path = item->data(::Qt::UserRole).toString(); if (!path.isEmpty()) emit receivedFileRequested(path);
    });
    subscribe(); refreshLanguage(); refreshSnapshot();
}

TransferWindow::~TransferWindow()
{
    state_->refresh.stop(); disconnect(state_->events); disconnect(state_->destroyed);
    for (auto* child : findChildren<QObject*>()) disconnect(child, nullptr, this, nullptr);
}

void TransferWindow::subscribe()
{
    if (state_->subscribed || !state_->service) return;
    state_->subscribed = true;
    state_->events = connect(state_->service, &Transfer::TransferService::eventReady, this, [this](const Transfer::Event& event) {
        if (event.kind == Transfer::EventKind::DiscoveryRefreshed && event.request == state_->announcementRequest) {
            state_->announcementRequest = 0;
            showStatus(event.status == HUHU_OK ? translated(u"lan.devicesRefreshed") :
                event.status == HUHU_IO ? translated(u"lan.discoveryUnavailable") : Presentation::status(state_->translator, event.status),
                event.status != HUHU_OK);
        }
        if (event.kind == Transfer::EventKind::Disabled) state_->announcementRequest = 0;
        if (event.kind == Transfer::EventKind::DiscoveryFinished && event.request == state_->discoveryRequest) {
            state_->discoveryRequest = 0;
            showStatus(event.status == HUHU_OK ? translated(u"lan.deviceVerified") :
                Presentation::status(state_->translator, event.status, event.data.value(QStringLiteral("httpStatus")).toInt()), event.status != HUHU_OK);
            if (event.status != HUHU_OK) { state_->preferredPeerKey.clear(); state_->retryAfterReview = false; }
        }
        if (!state_->refresh.isActive()) state_->refresh.start();
    });
    state_->destroyed = connect(state_->service, &QObject::destroyed, this, [this] {
        state_->service.clear(); state_->subscribed = false; state_->reviewed = false;
        state_->live->setText(translated(u"lan.closed")); updateActions();
    });
}

void TransferWindow::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); refreshLanguage(); }
QString TransferWindow::translated(QStringView key) const { return Presentation::text(state_->translator, key); }
bool TransferWindow::preparationPending() const noexcept { return state_->preparationToken != 0; }
qsizetype TransferWindow::deviceCount() const noexcept { return state_->devices->topLevelItemCount(); }
qsizetype TransferWindow::taskCount() const noexcept { return state_->tasks.size(); }
QString TransferWindow::selectedEndpoint() const { return state_->selectedPeerKey; }
void TransferWindow::setArchiveDirectory(const QString& directory) { state_->directory = directory; taskSelected(); }

bool TransferWindow::setMediaSelection(const QVector<Media::Entry>& entries)
{
    if (preparationPending() || entries.size() > static_cast<qsizetype>(Transfer::TransferLimits::maximumFiles)) return false;
    if (std::any_of(entries.cbegin(), entries.cend(), [](const auto& entry) { return entry.path.isEmpty() || !entry.snapshot.bytes; })) return false;
    state_->selection = entries;
    QStringList names; for (const auto& entry : entries) names.append(entry.name);
    state_->selectionLabel->setText(entries.isEmpty() ? translated(u"lan.selectMediaHint") : translated(u"lan.selectedMedia").arg(entries.size()).arg(names.join(QStringLiteral(", "))));
    state_->selectionLabel->setToolTip(names.join(u'\n')); updateActions(); return true;
}

void TransferWindow::refreshLanguage()
{
    setWindowTitle(translated(u"lan.title")); state_->heading->setText(windowTitle());
    const std::pair<QPushButton*, QStringView> buttons[] = {
        {state_->settings, u"lan.settings"}, {state_->close, u"lan.close"}, {state_->probe, u"lan.findDevice"},
        {state_->verify, u"lan.verifyDevice"}, {state_->send, u"lan.send"}, {state_->answer, u"lan.reviewOffer"},
        {state_->cancel, u"lan.cancel"}, {state_->retry, u"lan.retry"}, {state_->reveal, u"lan.showFile"}};
    for (const auto& [button, key] : buttons) { button->setText(translated(key)); button->setAccessibleName(button->text()); }
    state_->refreshDevices->setToolTip(translated(u"lan.refreshDevices"));
    state_->refreshDevices->setAccessibleName(translated(u"lan.refreshDevices"));
    state_->address->setPlaceholderText(translated(u"lan.manualAddress")); state_->address->setAccessibleName(translated(u"lan.manualAddress"));
    state_->port->setAccessibleName(translated(u"lan.port"));
    state_->pin->setPlaceholderText(translated(u"lan.optionalPin")); state_->pin->setAccessibleName(translated(u"lan.optionalPin"));
    state_->devices->setHeaderLabels({translated(u"lan.device"), translated(u"lan.address"), translated(u"lan.identity")});
    state_->activity->setHeaderLabels({translated(u"lan.task"), translated(u"lan.status"), translated(u"lan.progress")});
    state_->tasksLabel->setText(translated(u"lan.tasks")); state_->activityHint->setText(translated(u"lan.activityHint"));
    QStringList names; for (const auto& entry : state_->selection) names.append(entry.name);
    state_->selectionLabel->setText(names.isEmpty() ? translated(u"lan.selectMediaHint") :
        translated(u"lan.selectedMedia").arg(names.size()).arg(names.join(QStringLiteral(", "))));
    refreshSnapshot();
}

Transfer::Submission TransferWindow::discover(const QString& address, quint16 port)
{
    if (!state_->service || state_->discoveryRequest) return {HUHU_BUSY, 0};
    const QHostAddress host(address);
    if (host.protocol() != QAbstractSocket::IPv4Protocol || host.isNull() || host.isMulticast() ||
        host == QHostAddress::AnyIPv4 || host == QHostAddress::Broadcast || port == 0) {
        showStatus(translated(u"lan.invalidAddress"), true); return {HUHU_INVALID_ARGUMENT, 0};
    }
    const auto result = state_->service->discover(host.toString(), port);
    if (result) {
        state_->discoveryRequest = result.request;
        state_->preferredPeerKey = host.toString() + u':' + QString::number(port);
        if (state_->preferredPeerKey == state_->selectedPeerKey) {
            state_->reviewed = false; state_->reviewFailed = false; state_->reviewToken = 0;
        }
        showStatus(translated(u"lan.discovering"), false);
    } else showStatus(Presentation::status(state_->translator, result.status), true);
    updateActions(); return result;
}

bool TransferWindow::selectEndpoint(const QString& address, quint16 port)
{
    const auto key = address + u':' + QString::number(port);
    if (!state_->peers.contains(key)) return false;
    state_->preferredPeerKey = key; refreshSnapshot();
    return state_->selectedPeerKey == key;
}

void TransferWindow::refreshSnapshot()
{
    if (!state_->service) { updateActions(); return; }
    const auto snapshot = state_->service->viewSnapshot(); if (!snapshot) return;
    state_->multicastAvailable = snapshot->service && snapshot->service->data.value(QStringLiteral("multicastAvailable")).toBool();
    state_->live->setText(Presentation::serviceState(state_->translator, snapshot->state));
    if (snapshot->service && snapshot->service->status != HUHU_OK)
        state_->live->setText(state_->live->text() + QStringLiteral(" · ") + Presentation::status(state_->translator, snapshot->service->status));
    QMap<QString, QJsonObject> peers;
    const auto generation = snapshot->service ? snapshot->service->generation : std::uint64_t{};
    if (snapshot->state == Transfer::ServiceState::Enabled && snapshot->devices) {
        for (const auto& value : snapshot->devices->data.value(QStringLiteral("devices")).toArray()) {
            const auto device = value.toObject();
            for (const auto& channelValue : device.value(QStringLiteral("channels")).toArray()) {
                const auto channel = channelValue.toObject();
                if (channel.value(QStringLiteral("protocol")).toString() != QStringLiteral("https")) continue;
                const auto address = channel.value(QStringLiteral("address")).toString();
                if (QHostAddress(address).protocol() != QAbstractSocket::IPv4Protocol) continue;
                QJsonObject peer = device;
                peer.remove(QStringLiteral("channels")); peer.insert(QStringLiteral("address"), address);
                peer.insert(QStringLiteral("port"), channel.value(QStringLiteral("port")));
                const auto endpoint = Presentation::endpoint(peer);
                const auto previous = peers.value(endpoint);
                if (!previous.isEmpty() && previous.value(QStringLiteral("fingerprint")) != peer.value(QStringLiteral("fingerprint"))) {
                    // An old certificate may remain in discovery's finite
                    // cache. Conflicting claims for one endpoint require a
                    // fresh actual TLS probe, never arbitrary map order.
                    peer.insert(QStringLiteral("verified"), false);
                    peer.insert(QStringLiteral("fingerprint"), QString());
                }
                peers.insert(endpoint, peer);
            }
        }
    }
    QMap<QString, Task> tasks;
    for (const auto& event : snapshot->events) {
        if (event.kind == Transfer::EventKind::DiscoveryFinished && event.status == HUHU_OK && event.generation == generation &&
            snapshot->state == Transfer::ServiceState::Enabled) {
            auto peer = event.data; peer.insert(QStringLiteral("verified"), true); peer.insert(QStringLiteral("confirmed"), true);
            // A later discovery state supersedes old probes, including a
            // removed/offline endpoint or a changed TLS identity. Conversely,
            // a newly finished TLS probe can supersede an older native list.
            if (!snapshot->devices || event.sequence > snapshot->devices->sequence)
                peers.insert(Presentation::endpoint(peer), peer);
            continue;
        }
        if (!isTask(event)) continue;
        auto& task = tasks[Presentation::taskKey(event)];
        switch (event.kind) {
        case Transfer::EventKind::SendQueued: task.origin = event; break;
        case Transfer::EventKind::ReceiveOffer: task.origin = event; task.incoming = true; break;
        case Transfer::EventKind::SendProgress: task.progress = event; break;
        case Transfer::EventKind::ReceiveDecisionQueued: case Transfer::EventKind::CommandFinished: task.decision = event; task.incoming = true; break;
        case Transfer::EventKind::ReceiveProgress:
            if (event.receiveProgress) task.received.insert_or_assign(event.receiveProgress->fileId, *event.receiveProgress);
            task.incoming = true; break;
        case Transfer::EventKind::ReceiveFinished: task.incoming = true; [[fallthrough]];
        case Transfer::EventKind::SendFinished: task.terminal = event; break;
        default: break;
        }
    }
    if (snapshot->service && !snapshot->service->receivedFiles.empty()) {
        Task recovery; recovery.terminal = *snapshot->service; recovery.incoming = true; recovery.recovery = true;
        tasks.insert(QStringLiteral("recovery/") + QString::number(static_cast<qulonglong>(snapshot->service->generation)), std::move(recovery));
    }
    state_->peers = std::move(peers); state_->tasks = std::move(tasks);
    state_->rebuilding = true;
    {
        const QSignalBlocker blocked(state_->devices);
        QMap<QString, QStringList> groups;
        for (auto it = state_->peers.cbegin(); it != state_->peers.cend(); ++it) {
            const auto fingerprint = it.value().value(QStringLiteral("fingerprint")).toString().toUpper();
            groups[Presentation::fingerprint(fingerprint) ? fingerprint : QStringLiteral("endpoint/") + it.key()].append(it.key());
        }
        QMap<QString, QTreeWidgetItem*> rows;
        for (int index = 0; index < state_->devices->topLevelItemCount(); ++index) {
            auto* item = state_->devices->topLevelItem(index); rows.insert(item->data(0, ::Qt::UserRole).toString(), item);
        }
        for (auto it = rows.begin(); it != rows.end(); ++it) if (!groups.contains(it.key())) delete it.value();
        const auto desired = state_->preferredPeerKey.isEmpty() ? state_->selectedPeerKey : state_->preferredPeerKey;
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
            auto* item = rows.value(it.key(), nullptr); if (!item) item = new QTreeWidgetItem(state_->devices);
            auto endpoint = item->data(0, ::Qt::UserRole + 1).toString();
            if (it.value().contains(desired)) endpoint = desired;
            else if (!it.value().contains(endpoint)) endpoint = it.value().front();
            const auto peer = state_->peers.value(endpoint);
            item->setData(0, ::Qt::UserRole, it.key()); item->setData(0, ::Qt::UserRole + 1, endpoint);
            item->setText(0, label(peer)); item->setToolTip(0, item->text(0));
            item->setText(1, endpoint + (it.value().size() > 1 ? QStringLiteral(" (+%1)").arg(it.value().size() - 1) : QString()));
            item->setToolTip(1, it.value().join(u'\n'));
            item->setText(2, translated(peer.value(QStringLiteral("verified")).toBool() ?
                u"lan.tlsVerified" : u"lan.needsVerification"));
            item->setToolTip(2, peer.value(QStringLiteral("fingerprint")).toString());
        }
        for (int index = 0; index < state_->devices->topLevelItemCount(); ++index) {
            auto* item = state_->devices->topLevelItem(index);
            if (item->data(0, ::Qt::UserRole + 1).toString() == desired) { state_->devices->setCurrentItem(item); break; }
        }
        if (!state_->discoveryRequest) state_->preferredPeerKey.clear();
    }
    {
        const QSignalBlocker blocked(state_->activity);
        QMap<QString, QTreeWidgetItem*> rows;
        for (int index = 0; index < state_->activity->topLevelItemCount(); ++index) {
            auto* item = state_->activity->topLevelItem(index); rows.insert(item->data(0, ::Qt::UserRole).toString(), item);
        }
        for (auto it = rows.begin(); it != rows.end(); ++it) if (!state_->tasks.contains(it.key())) delete it.value();
        for (auto it = state_->tasks.cbegin(); it != state_->tasks.cend(); ++it) {
            auto* item = rows.value(it.key(), nullptr); if (!item) item = new QTreeWidgetItem(state_->activity);
            item->setData(0, ::Qt::UserRole, it.key());
            QString peer;
            if (it.value().origin) peer = it.value().incoming ? it.value().origin->data.value(QStringLiteral("alias")).toString() :
                label(it.value().origin->data.value(QStringLiteral("peer")).toObject());
            item->setText(0, it.value().recovery ? translated(u"lan.recoveryTask") :
                translated(it.value().incoming ? u"lan.receiveTask" : u"lan.sendTask").arg(peer));
            item->setText(1, taskStatus(it.value(), state_->translator)); item->setText(2, taskProgress(it.value()));
            item->setToolTip(0, item->text(0)); item->setToolTip(1, item->text(1));
        }
    }
    state_->rebuilding = false;
    const QPointer<TransferWindow> lifetime(this);
    selectedPeerChanged(); if (!lifetime) return;
    taskSelected(); updateActions();
}

void TransferWindow::selectedPeerChanged()
{
    if (state_->rebuilding) return;
    const auto* row = state_->devices->currentItem();
    const auto key = row ? row->data(0, ::Qt::UserRole + 1).toString() : QString();
    const auto peer = state_->peers.value(key);
    const auto previousFingerprint = state_->selectedPeer.value(QStringLiteral("fingerprint")).toString();
    const auto currentFingerprint = peer.value(QStringLiteral("fingerprint")).toString();
    const bool changed = key != state_->selectedPeerKey || previousFingerprint != currentFingerprint;
    state_->selectedPeerKey = key; state_->selectedPeer = peer;
    state_->peerIdentity->setText(currentFingerprint.isEmpty() ? QString() :
        translated(u"lan.fingerprint") + QStringLiteral(": ") + currentFingerprint);
    if (changed) {
        state_->reviewed = false; state_->reviewFailed = false; state_->reviewToken = 0; state_->expectedFingerprint.clear();
        state_->pin->clear();
        if (state_->confirmation) state_->confirmation->close();
    }
    if (peer.isEmpty()) { state_->trust->setText(translated(u"lan.selectDevice")); updateActions(); return; }
    if (!peer.value(QStringLiteral("verified")).toBool() || !Presentation::fingerprint(currentFingerprint)) {
        state_->trust->setText(translated(u"lan.needsVerification")); updateActions(); return;
    }
    if (!state_->reviewed && !state_->reviewFailed && !state_->reviewToken && !state_->discoveryRequest &&
        state_->nextToken != std::numeric_limits<quint64>::max()) {
        state_->reviewToken = state_->nextToken++;
        state_->trust->setText(translated(u"lan.checkingIdentity")); updateActions();
        emit peerReviewRequested(state_->reviewToken, peer); return;
    }
    if (state_->reviewed) state_->trust->setText(translated(state_->expectedFingerprint.isEmpty() ? u"lan.newIdentity" :
        state_->expectedFingerprint.compare(currentFingerprint, ::Qt::CaseInsensitive) == 0 ? u"lan.knownIdentity" : u"lan.changedIdentity"));
    updateActions();
}

void TransferWindow::completePeerReview(quint64 token, DesktopResult result,
    const QString& expectedFingerprint, const QString& message)
{
    if (!token || state_->reviewToken != token) return;
    state_->reviewToken = 0;
    if (result != DesktopResult::Succeeded || (!expectedFingerprint.isEmpty() && !Presentation::fingerprint(expectedFingerprint))) {
        state_->reviewed = false; state_->reviewFailed = true; state_->retryAfterReview = false;
        showStatus(message.isEmpty() ? translated(u"lan.identityUnavailable") : message, true); updateActions(); return;
    }
    state_->expectedFingerprint = expectedFingerprint; state_->reviewed = true;
    const bool changed = !expectedFingerprint.isEmpty() && expectedFingerprint.compare(
        state_->selectedPeer.value(QStringLiteral("fingerprint")).toString(), ::Qt::CaseInsensitive) != 0;
    state_->trust->setText(translated(changed ? u"lan.changedIdentity" : expectedFingerprint.isEmpty() ? u"lan.newIdentity" : u"lan.knownIdentity"));
    state_->trust->setProperty("error", changed); Theme::repolish(state_->trust); updateActions();
    if (state_->retryAfterReview) { state_->retryAfterReview = false; confirmSend(); }
}

void TransferWindow::confirmSend()
{
    if (!state_->send->isEnabled() || state_->confirmation || state_->nextToken == std::numeric_limits<quint64>::max()) return;
    const auto fingerprint = state_->selectedPeer.value(QStringLiteral("fingerprint")).toString();
    const bool changed = !state_->expectedFingerprint.isEmpty() && state_->expectedFingerprint.compare(fingerprint, ::Qt::CaseInsensitive) != 0;
    QStringList names; for (const auto& entry : state_->selection) names.append(entry.name);
    const auto description = state_->selection.size() == 1 ? state_->selection.front().name :
        translated(u"lan.selectedCount").arg(state_->selection.size());
    auto message = translated(u"lan.confirmSend").arg(description, label(state_->selectedPeer),
        Presentation::endpoint(state_->selectedPeer), fingerprint);
    if (changed) message = translated(u"lan.confirmChangedIdentity").arg(state_->expectedFingerprint, fingerprint) + QStringLiteral("\n\n") + message;
    else if (state_->expectedFingerprint.isEmpty()) message = translated(u"lan.confirmNewIdentity") + QStringLiteral("\n\n") + message;
    auto* box = new QMessageBox(changed ? QMessageBox::Warning : QMessageBox::Question, translated(u"lan.send"),
        message, QMessageBox::Yes | QMessageBox::No, this);
    box->setObjectName(QStringLiteral("transferSendConfirmation")); box->setTextFormat(::Qt::PlainText);
    box->setDefaultButton(QMessageBox::No); box->setEscapeButton(QMessageBox::No);
    box->setDetailedText(names.join(u'\n'));
    box->setWindowModality(::Qt::WindowModal); box->setAttribute(::Qt::WA_DeleteOnClose); box->setFont(Theme::font());
    state_->confirmation = box;
    const auto reviewedPeer = state_->selectedPeer;
    connect(box, &QMessageBox::finished, this, [this, reviewedPeer](int result) {
        state_->confirmation.clear();
        if (result == QMessageBox::Yes && reviewedPeer == state_->selectedPeer && state_->reviewed) requestPreparation();
    });
    box->open();
}

void TransferWindow::requestPreparation()
{
    if (preparationPending() || !state_->service || state_->service->state() != Transfer::ServiceState::Enabled ||
        state_->selection.isEmpty() || !state_->reviewed || state_->nextToken == std::numeric_limits<quint64>::max()) return;
    QJsonObject peer{{QStringLiteral("address"), state_->selectedPeer.value(QStringLiteral("address"))},
        {QStringLiteral("port"), state_->selectedPeer.value(QStringLiteral("port"))},
        {QStringLiteral("fingerprint"), state_->selectedPeer.value(QStringLiteral("fingerprint"))}};
    // Exactly the pinned native peer schema. The PIN lives only in the
    // confirmed intent, never in the service's long-lived view cache.
    if (!state_->pin->text().isEmpty()) peer.insert(QStringLiteral("pin"), state_->pin->text());
    state_->pin->clear(); state_->preparationToken = state_->nextToken++;
    const auto token = state_->preparationToken;
    const auto alias = state_->selectedPeer.value(QStringLiteral("alias")).toString();
    const auto entries = state_->selection;
    showStatus(translated(u"lan.preparing"), false); updateActions();
    emit prepareSendRequested(token, peer, alias, entries);
}

void TransferWindow::completePreparation(quint64 token, int status, quint64 request)
{
    if (!token || token != state_->preparationToken) return;
    state_->preparationToken = 0;
    if (status == HUHU_OK && request == 0) status = HUHU_INVALID_STATE;
    showStatus(status == HUHU_OK ? translated(u"lan.queued") : Presentation::status(state_->translator, status), status != HUHU_OK);
    if (state_->subscribed) refreshSnapshot(); else updateActions();
}

void TransferWindow::taskSelected()
{
    if (state_->rebuilding) return;
    const auto selectedPath = state_->details->currentItem() ? state_->details->currentItem()->data(::Qt::UserRole).toString() : QString();
    const auto* row = state_->activity->currentItem();
    const auto task = row ? state_->tasks.value(row->data(0, ::Qt::UserRole).toString()) : Task{};
    const QSignalBlocker blocked(state_->details); state_->details->clear();
    if (task.origin) {
        if (task.incoming) {
            for (const auto& value : task.origin->data.value(QStringLiteral("files")).toArray()) {
                const auto file = value.toObject();
                auto text = file.value(QStringLiteral("fileName")).toString() + QStringLiteral(" · ") + Presentation::kind(file) +
                    QStringLiteral(" · ") + Presentation::bytes(jsonBytes(file, QStringLiteral("size")));
                auto* item = new QListWidgetItem(text, state_->details); item->setToolTip(text);
                if (task.terminal) for (const auto& result : task.terminal->receivedFiles)
                    if (result.fileId == file.value(QStringLiteral("id")).toString()) {
                        text += QStringLiteral(" · ") + Presentation::status(state_->translator, result.status);
                        item->setText(text);
                        if (result.published && !result.path.isEmpty()) {
                            item->setData(::Qt::UserRole, result.path); item->setToolTip(text + u'\n' + result.path);
                            if (selectedPath == result.path) state_->details->setCurrentItem(item);
                        }
                    }
            }
        } else for (const auto& file : task.origin->sendFiles) {
            auto text = file.name + QStringLiteral(" · ") + Presentation::kind(file.mediaKind) + QStringLiteral(" · ") +
                Presentation::bytes(file.snapshot.bytes);
            auto* item = new QListWidgetItem(text, state_->details); item->setToolTip(file.path);
            if (task.terminal) {
                const auto outcomes = task.terminal->data.value(QStringLiteral("files")).toArray();
                const auto index = static_cast<qsizetype>(&file - task.origin->sendFiles.data());
                for (const auto& outcomeValue : outcomes) {
                    const auto outcome = outcomeValue.toObject();
                    if (outcome.value(QStringLiteral("id")).toString() == QString::number(static_cast<qlonglong>(index))) {
                        text += QStringLiteral(" · ") + Presentation::status(state_->translator,
                            outcome.value(QStringLiteral("status")).toInt(HUHU_PROTOCOL)); item->setText(text); break;
                    }
                }
            }
        }
    }
    if (!task.origin && task.terminal) for (const auto& result : task.terminal->receivedFiles) {
        const auto text = QFileInfo(result.path).fileName() + QStringLiteral(" · ") + Presentation::status(state_->translator, result.status);
        auto* item = new QListWidgetItem(text, state_->details); item->setToolTip(result.path);
        if (result.published && !result.path.isEmpty()) {
            item->setData(::Qt::UserRole, result.path);
            if (selectedPath == result.path) state_->details->setCurrentItem(item);
        }
    }
    if (state_->details->currentRow() < 0 && state_->details->count()) state_->details->setCurrentRow(0);
    updateActions();
}

void TransferWindow::cancelTask()
{
    const auto* row = state_->activity->currentItem(); if (!row || !state_->service) return;
    const auto task = state_->tasks.value(row->data(0, ::Qt::UserRole).toString());
    if (!task.origin || task.terminal) return;
    const auto result = task.incoming ? state_->service->cancelReceive(task.origin->data.value(QStringLiteral("sessionId")).toString()) :
        state_->service->cancelSend(task.origin->request);
    showStatus(result ? translated(u"lan.cancelling") : Presentation::status(state_->translator, result.status), !result);
}

void TransferWindow::retryTask()
{
    const auto* row = state_->activity->currentItem(); if (!row || preparationPending()) return;
    const auto task = state_->tasks.value(row->data(0, ::Qt::UserRole).toString());
    if (task.incoming || !task.origin || !task.terminal || task.terminal->status == HUHU_OK) return;
    const auto outcomes = task.terminal->data.value(QStringLiteral("files")).toArray();
    QVector<Media::Entry> entries;
    for (std::size_t index = 0; index < task.origin->sendFiles.size(); ++index) {
        const auto completed = std::any_of(outcomes.begin(), outcomes.end(), [index](const QJsonValue& value) {
            const auto item = value.toObject();
            return item.value(QStringLiteral("id")).toString() == QString::number(static_cast<qulonglong>(index)) &&
                item.value(QStringLiteral("accepted")).toBool() && item.value(QStringLiteral("status")).toInt(HUHU_PROTOCOL) == HUHU_OK;
        });
        if (completed) continue;
        const auto& file = task.origin->sendFiles[index];
        entries.append({file.path, file.name, mediaKind(file.mediaKind), file.snapshot});
    }
    if (entries.isEmpty() || !setMediaSelection(entries)) return;
    const auto peer = task.origin->data.value(QStringLiteral("peer")).toObject();
    state_->retryAfterReview = true;
    if (!discover(peer.value(QStringLiteral("address")).toString(), static_cast<quint16>(peer.value(QStringLiteral("port")).toInt())))
        state_->retryAfterReview = false;
}

void TransferWindow::updateActions()
{
    const bool enabled = state_->service && state_->service->state() == Transfer::ServiceState::Enabled;
    state_->refreshDevices->setEnabled(enabled && state_->multicastAvailable && !state_->announcementRequest);
    state_->probe->setEnabled(enabled && !state_->discoveryRequest);
    state_->verify->setEnabled(enabled && !state_->discoveryRequest && !state_->selectedPeer.isEmpty());
    state_->send->setEnabled(enabled && state_->reviewed && !state_->selection.isEmpty() && !preparationPending() && !state_->discoveryRequest);
    state_->pin->setEnabled(enabled && !preparationPending());
    const auto* row = state_->activity->currentItem();
    const auto task = row ? state_->tasks.value(row->data(0, ::Qt::UserRole).toString()) : Task{};
    state_->cancel->setEnabled(enabled && task.origin && !task.terminal);
    state_->retry->setEnabled(enabled && task.origin && task.terminal && !task.incoming && task.terminal->status != HUHU_OK && !preparationPending());
    state_->answer->setEnabled(enabled && task.incoming && task.origin && !task.decision && !task.terminal);
    const auto* file = state_->details->currentItem(); state_->reveal->setEnabled(file && !file->data(::Qt::UserRole).toString().isEmpty());
}

void TransferWindow::showStatus(const QString& message, bool error)
{ state_->status->setText(message); state_->status->setProperty("error", error); Theme::repolish(state_->status); }

void TransferWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event); state_->closed = false; subscribe(); refreshSnapshot();
}

void TransferWindow::closeEvent(QCloseEvent* event)
{
    if (state_->confirmation) state_->confirmation->close();
    state_->pin->clear(); state_->refresh.stop(); disconnect(state_->events); disconnect(state_->destroyed);
    state_->subscribed = false; event->accept();
    const bool notify = !state_->closed; state_->closed = true;
    // Confirmed preparation belongs to the host and admitted sends to the
    // service. Neither is cancelled by closing or deleting this view.
    if (notify) emit viewClosed();
}

void TransferWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) refreshLanguage();
}

} // namespace HuhuCapture::UI::Qt
