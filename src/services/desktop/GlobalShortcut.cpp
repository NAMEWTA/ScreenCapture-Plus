#include "services/desktop/GlobalShortcut.h"
#include "core/ShortcutKeys.h"
#include "platform/GlobalShortcutBackend.h"

#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <array>
#include <utility>

namespace HuhuCapture::Desktop {
namespace {
QString canonicalName(QString key)
{
    constexpr std::array names{u"Ctrl", u"Alt", u"Shift", u"Win", u"LWin", u"RWin",
        u"Up", u"Down", u"Left", u"Right", u"Enter", u"Esc", u"Tab", u"Space",
        u"Backspace", u"Delete", u"Insert", u"Home", u"End", u"PageUp", u"PageDown",
        u"PrintScreen", u"ScrollLock", u"Pause", u"NumLock", u"CapsLock"};
    for (const auto* name : names)
        if (key.compare(QString::fromUtf16(name), Qt::CaseInsensitive) == 0) return QString::fromUtf16(name);
    if (key.size() == 1 && key.front().isLetter()) return key.toUpper();
    if (key.size() >= 2 && key.front().toUpper() == QChar(u'F')) {
        bool valid{}; const auto number = key.mid(1).toInt(&valid);
        if (valid && number >= 1 && number <= 24) return QStringLiteral("F%1").arg(number);
    }
    if (key.size() == 4 && key.first(3).compare(QStringLiteral("Num"), Qt::CaseInsensitive) == 0 &&
        key.back() >= QChar(u'0') && key.back() <= QChar(u'9'))
        return QStringLiteral("Num") + key.back();
    return key;
}
}

QStringList canonicalShortcut(const QStringList& source)
{
    if (source.isEmpty() || source.size() > 7) return {};
    std::vector<std::wstring> keys;
    for (const auto& key : source) {
        if (key.isEmpty() || key.size() > 32 || key.contains(QChar(u'\0'))) return {};
        keys.push_back(canonicalName(key).toStdWString());
    }
    if (Core::validateShortcutKeys(keys, true) != Core::ShortcutValidity::Valid) return {};
    Core::normalizeShortcutKeys(keys);
    QStringList result;
    for (const auto& key : keys) result.push_back(QString::fromStdWString(key));
    return result;
}

QStringList parseShortcut(const QString& stored)
{
    if (stored.isEmpty() || stored.size() > 128) return {};
    QStringList keys;
    auto remaining = stored;
    for (;;) {
        const auto separator = remaining.indexOf(QChar(u'+'));
        if (separator <= 0) break;
        const auto prefix = canonicalName(remaining.first(separator));
        if (!Core::isShortcutModifier(prefix.toStdWString())) break;
        keys.push_back(prefix); remaining.remove(0, separator + 1);
    }
    keys.push_back(remaining);
    return canonicalShortcut(keys);
}

GlobalShortcut::GlobalShortcut(QObject* parent) : QObject(parent), backend_(Platform::createGlobalShortcutBackend(*this))
{
    connect(this, &GlobalShortcut::bindingFinished, this, [this](quint64 request) {
        if (request == pendingRequest_) { pendingRequest_ = 0; pendingCancelled_ = false; }
        checkShutdown();
    });
}
GlobalShortcut::~GlobalShortcut() { shutdown(); }
void GlobalShortcut::initialize()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (stopped_) return;
    ++deliveries_;
    const QPointer<GlobalShortcut> alive(this);
    backend_->initialize();
    if (alive) { --deliveries_; checkShutdown(); }
}
void GlobalShortcut::setActionDescription(const QString& description)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!description.isEmpty() && description.size() <= 512 && !description.contains(QChar(u'\0')))
        backend_->setActionDescription(description);
}
ShortcutSnapshot GlobalShortcut::snapshot() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return backend_->snapshot();
}
quint64 GlobalShortcut::bind(const QStringList& source, QWindow* parent, Persist persist, bool allowUnparented)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (stopped_ || !nextRequest_) return 0;
    const auto request = nextRequest_++;
    ++deliveries_;
    if (pendingRequest_) {
        QTimer::singleShot(0, this, [this, request] {
            const QPointer<GlobalShortcut> alive(this);
            Q_EMIT bindingFinished(request, {stopped_ ? ShortcutError::Cancelled : ShortcutError::Busy});
            if (alive) { --deliveries_; checkShutdown(); }
        });
        return request;
    }
    pendingRequest_ = request; pendingCancelled_ = false;
    const auto keys = canonicalShortcut(source);
    const QPointer<QWindow> parentLife(parent);
    const bool hadParent = parent != nullptr;
    QTimer::singleShot(0, this, [this, request, keys, parentLife, hadParent,
                               persist = std::move(persist), allowUnparented]() mutable {
        const QPointer<GlobalShortcut> alive(this);
        if (stopped_ || pendingCancelled_) Q_EMIT bindingFinished(request, {ShortcutError::Cancelled});
        else if (keys.isEmpty()) Q_EMIT bindingFinished(request, {ShortcutError::InvalidShortcut});
        else if (hadParent && !parentLife) Q_EMIT bindingFinished(request, {ShortcutError::Cancelled});
        else backend_->bind(request, keys, parentLife, std::move(persist), allowUnparented);
        if (alive) { --deliveries_; checkShutdown(); }
    });
    return request;
}
void GlobalShortcut::cancel(quint64 request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (request == pendingRequest_) pendingCancelled_ = true;
    backend_->cancel(request);
}
ShortcutFailure GlobalShortcut::setCaptureActive(bool active)
{
    Q_ASSERT(QThread::currentThread() == thread());
    return stopped_ ? ShortcutFailure{ShortcutError::Unavailable} : backend_->setCaptureActive(active);
}
void GlobalShortcut::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (std::exchange(stopped_, true)) return;
    backend_->shutdown([alive = QPointer<GlobalShortcut>(this)] {
        if (!alive) return;
        alive->backendStopped_ = true;
        alive->checkShutdown();
    });
}
void GlobalShortcut::checkShutdown()
{
    if (!stopped_ || !backendStopped_ || pendingRequest_ || deliveries_ || shutdownQueued_ || shutdownReported_) return;
    shutdownQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        shutdownQueued_ = false;
        if (!stopped_ || !backendStopped_ || pendingRequest_ || deliveries_ || shutdownReported_) return;
        shutdownReported_ = true;
        Q_EMIT shutdownFinished();
    });
}

} // namespace HuhuCapture::Desktop

