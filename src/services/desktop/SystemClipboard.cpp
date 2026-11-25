#include "services/desktop/SystemClipboard.h"
#include "core/PixelBuffer.h"

#include <QClipboard>
#include <QFile>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImageWriter>
#include <QMimeData>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#include "platform/windows/DesktopClipboard.h"
#else
#include "platform/linux/DesktopClipboard.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#endif

namespace HuhuCapture::Desktop {
namespace {
Failure cancelled() { return {Error::Cancelled, 0, QStringLiteral("Clipboard preparation was cancelled")}; }
Failure changed() { return {Error::ChangedOnDisk, 0, QStringLiteral("The confirmed media changed on disk; refresh history before copying")}; }
Failure allocation() { return {Error::Allocation, 0, QStringLiteral("Insufficient memory to prepare the clipboard")}; }

#ifdef Q_OS_WIN
Failure fromNative(const Platform::Windows::ClipboardResult& result)
{
    using Native = Platform::Windows::ClipboardError;
    Error error = Error::ClipboardRejected;
    switch (result.error) {
    case Native::None: return {};
    case Native::InvalidInput: error = Error::InvalidArgument; break;
    case Native::Allocation: error = Error::Allocation; break;
    case Native::Encoding: error = Error::Encoding; break;
    case Native::Busy: error = Error::Busy; break;
    case Native::Partial: error = Error::PartialPublication; break;
    case Native::Publication: break;
    }
    return {error, result.nativeCode, QStringLiteral("Windows could not publish every required clipboard format")};
}
#else
// QImageWriter uses the real PNG plugin. Limit compressed output as well as
// pixels, and make cancellation observable at its existing write boundary.
class PngBuffer final : public QIODevice {
public:
    PngBuffer(quint64 maximum, std::stop_token stop) : maximum_(maximum), stop_(stop) { open(QIODevice::WriteOnly); }
    QByteArray bytes;
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 length) override
    {
        if (stop_.stop_requested()) { setErrorString(QStringLiteral("PNG encoding cancelled")); return -1; }
        if (length < 0 || static_cast<quint64>(length) > maximum_ - static_cast<quint64>(bytes.size())) {
            setErrorString(QStringLiteral("PNG exceeds the clipboard memory budget")); return -1;
        }
        bytes.append(data, static_cast<qsizetype>(length)); return length;
    }
private:
    quint64 maximum_;
    std::stop_token stop_;
};
#endif
}

struct PreparedClipboard::State {
    std::atomic_bool consumed{};
    std::shared_ptr<const Media::MediaLease> lease;
    QStringList formats;
#ifdef Q_OS_WIN
    Platform::Windows::ClipboardPayload native;
#else
    QImage image;
    QByteArray png;
    QUrl file;
    std::optional<QString> text;
    QByteArray utf8;
#endif
    void release() noexcept
    {
        lease.reset(); formats.clear();
#ifdef Q_OS_WIN
        native = {};
#else
        image = {}; png.clear(); file = QUrl{};
        text.reset(); utf8.clear();
#endif
    }
};
PreparedClipboard::PreparedClipboard() : state_(std::make_unique<State>()) {}
PreparedClipboard::~PreparedClipboard() = default;

ClipboardPreparation prepareImage(const QImage& image, ClipboardLimits limits, std::stop_token stop)
{
    try {
        if (stop.stop_requested()) return {cancelled(), {}};
        const auto layout = Core::pixelLayout(image.width(), image.height(), 4);
        if (image.isNull() || !layout || !limits.maximumImageBytes || layout->bytes > limits.maximumImageBytes ||
            static_cast<quint64>(image.sizeInBytes()) > limits.maximumImageBytes ||
            limits.maximumImageBytes > static_cast<quint64>((std::numeric_limits<qsizetype>::max)() / 2))
            return {{Error::InvalidArgument, 0, QStringLiteral("The image exceeds the clipboard pixel budget")}, {}};
        // detach() also owns externally backed QImages, without copying a
        // freshly converted allocation twice. A prepared item cannot retain a
        // capture buffer that its producer is about to reuse.
        QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
        pixels.detach();
        if (pixels.isNull()) return {allocation(), {}};
        pixels.setDevicePixelRatio(1.0);
        if (stop.stop_requested()) return {cancelled(), {}};
        auto content = std::shared_ptr<PreparedClipboard>(new PreparedClipboard);
#ifdef Q_OS_WIN
        auto native = Platform::Windows::prepareClipboardImage(pixels.width(), pixels.height(),
            static_cast<std::size_t>(pixels.bytesPerLine()),
            {pixels.constBits(), static_cast<std::size_t>(pixels.sizeInBytes())});
        if (native.result.error != Platform::Windows::ClipboardError::None) return {fromNative(native.result), {}};
        content->state_->native = std::move(native.payload);
        content->state_->formats = {QStringLiteral("PNG"), QStringLiteral("CF_DIBV5"), QStringLiteral("CF_DIB")};
#else
        PngBuffer buffer(limits.maximumImageBytes + limits.maximumImageBytes / 8 + 1024 * 1024, stop);
        QImageWriter writer(&buffer, "png");
        if (!writer.write(pixels))
            return {stop.stop_requested() ? cancelled() : Failure{Error::Encoding, writer.error(), writer.errorString()}, {}};
        content->state_->image = std::move(pixels); content->state_->png = std::move(buffer.bytes);
        content->state_->formats = {QStringLiteral("image/png"), QStringLiteral("application/x-qt-image")};
#endif
        if (stop.stop_requested()) return {cancelled(), {}};
        return {{}, std::move(content)};
    } catch (const std::bad_alloc&) { return {allocation(), {}}; }
}

ClipboardPreparation prepareText(const QString& text, ClipboardLimits limits, std::stop_token stop)
{
    try {
        if (stop.stop_requested()) return {cancelled(), {}};
        if (!limits.maximumTextBytes ||
            limits.maximumTextBytes > static_cast<quint64>((std::numeric_limits<qsizetype>::max)() / 4) ||
            static_cast<quint64>(text.size()) > limits.maximumTextBytes / sizeof(char16_t))
            return {{Error::InvalidArgument, 0, QStringLiteral("The text exceeds the clipboard memory budget")}, {}};
        if (!text.isValidUtf16() || text.contains(QChar(u'\0')))
            return {{Error::Encoding, 0, QStringLiteral("The complete value cannot be represented as Unicode plain text")}, {}};
        auto content = std::shared_ptr<PreparedClipboard>(new PreparedClipboard);
#ifdef Q_OS_WIN
        auto native = Platform::Windows::prepareClipboardText(text.toStdWString(),
            static_cast<std::size_t>(limits.maximumTextBytes));
        if (native.result.error != Platform::Windows::ClipboardError::None) return {fromNative(native.result), {}};
        content->state_->native = std::move(native.payload);
        content->state_->formats = {QStringLiteral("CF_UNICODETEXT")};
#else
        QString owned = text;
        owned.detach(); // Also own QString::fromRawData inputs before their producer resumes.
        auto utf8 = owned.toUtf8();
        if (static_cast<quint64>(utf8.size()) > limits.maximumTextBytes)
            return {{Error::InvalidArgument, 0, QStringLiteral("The UTF-8 text exceeds the clipboard memory budget")}, {}};
        content->state_->text = std::move(owned);
        content->state_->utf8 = std::move(utf8);
        content->state_->formats = {QStringLiteral("text/plain"), QStringLiteral("text/plain;charset=utf-8")};
#endif
        if (stop.stop_requested()) return {cancelled(), {}};
        return {{}, std::move(content)};
    } catch (const std::bad_alloc&) { return {allocation(), {}}; }
}

ClipboardPreparation prepareFile(std::shared_ptr<const Media::MediaLease> lease, ClipboardLimits limits, std::stop_token stop)
{
    try {
        if (stop.stop_requested()) return {cancelled(), {}};
        if (!lease || lease->removal()) return {{Error::InvalidArgument, 0, QStringLiteral("A history read lease is required for copying")}, {}};
        if (!lease->unchanged()) return {changed(), {}};
        const auto& entry = lease->entry();
        ClipboardPreparation output;
        if (entry.kind == Media::Kind::Png) {
            Media::Limits inspectionLimits; inspectionLimits.maximumImageBytes = limits.maximumImageBytes;
            QImage image;
            const auto inspection = Media::inspectPng(entry.path, inspectionLimits, stop, &image);
            if (inspection.failure) return {{inspection.failure.code == Media::Error::Cancelled ? Error::Cancelled : Error::Read,
                0, inspection.failure.detail}, {}};
            if (!lease->unchanged()) return {changed(), {}};
            output = prepareImage(image, limits, stop);
        } else {
            auto content = std::shared_ptr<PreparedClipboard>(new PreparedClipboard);
#ifdef Q_OS_WIN
            auto native = Platform::Windows::prepareClipboardFile(entry.path.toStdWString());
            if (native.result.error != Platform::Windows::ClipboardError::None) return {fromNative(native.result), {}};
            content->state_->native = std::move(native.payload); content->state_->formats = {QStringLiteral("CF_HDROP")};
#else
            content->state_->file = QUrl::fromLocalFile(entry.path);
            content->state_->formats = {QStringLiteral("text/uri-list")};
#endif
            output.content = std::move(content);
        }
        if (output.failure) return output;
        if (stop.stop_requested()) return {cancelled(), {}};
        if (!lease->unchanged()) return {changed(), {}};
        output.content->state_->lease = std::move(lease);
        return output;
    } catch (const std::bad_alloc&) { return {allocation(), {}}; }
}

struct SystemClipboard::State {
    // Admission is the only cross-thread state. A call accepted before shutdown
    // owns a receipt until all finished() listeners return, even when its GUI
    // dispatch has not run yet. Calls after that boundary return zero.
    std::mutex admission;
    quint64 sequence{};
    std::size_t receipts{};
    bool accepting{true};
    quint64 active{};
    unsigned publications{};
    bool stopping{}, shutdownEmitted{}, busyReported{};
    std::shared_ptr<PreparedClipboard> content;
    QPointer<QWindow> origin;
    QPointer<QFutureWatcher<Failure>> validation;
#ifndef Q_OS_WIN
    std::unique_ptr<Platform::Linux::DesktopClipboard> observer;
    QPointer<QDBusPendingCallWatcher> filePortal;
#endif
};

SystemClipboard::SystemClipboard(QObject* parent) : QObject(parent), state_(std::make_unique<State>())
{
#ifndef Q_OS_WIN
    state_->observer = std::make_unique<Platform::Linux::DesktopClipboard>();
    connect(state_->observer.get(), &Platform::Linux::DesktopClipboard::verified, this,
        [this](quint64 token, const Failure& failure) {
            if (state_->active != token) return;
            finish(token, {failure, failure ? QStringList{} : state_->content->state_->formats});
        });
#endif
}
SystemClipboard::~SystemClipboard()
{
    if (state_->content) state_->content->state_->release();
}
bool SystemClipboard::busy() const noexcept
{
    return state_->active || state_->validation || state_->publications
#ifndef Q_OS_WIN
        || state_->filePortal
#endif
        ;
}

quint64 SystemClipboard::publish(std::shared_ptr<PreparedClipboard> content, QWindow* origin)
{
    quint64 token{};
    {
        const std::lock_guard lock(state_->admission);
        if (state_->accepting && state_->sequence != (std::numeric_limits<quint64>::max)()) {
            token = ++state_->sequence; ++state_->receipts;
        }
    }
    if (!token) {
        if (content && !content->state_->consumed.exchange(true)) content->state_->release();
        return 0;
    }
    if (QThread::currentThread() != thread()) {
        QTimer::singleShot(0, this, [this, token, content = std::move(content)] {
            if (content && !content->state_->consumed.exchange(true)) content->state_->release();
            finish(token, {{Error::WrongThread, 0, QStringLiteral("Clipboard publication requires the GUI thread")}, {}});
        });
        return token;
    }
    if (!content || content->state_->consumed.exchange(true)) {
        finish(token, {{Error::InvalidArgument, 0, QStringLiteral("Clipboard preparation is missing or was already used")}, {}}); return token;
    }
    if (busy()) {
        content->state_->release();
        finish(token, {{Error::Busy, 0, QStringLiteral("Another clipboard publication is still pending")}, {}}); return token;
    }
    if (!qGuiApp || thread() != qGuiApp->thread()) {
        content->state_->release();
        finish(token, {{Error::Unavailable, 0, QStringLiteral("There is no application-owned system clipboard")}, {}}); return token;
    }
    try {
        state_->active = token; state_->content = std::move(content); state_->origin = origin;
        updateBusy();
#ifndef Q_OS_WIN
        if (!state_->content->state_->file.isEmpty() &&
            QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
            // Fixed Qt prefers the Documents transfer MIME for local files.
            // Query the actual version asynchronously before replacing any
            // clipboard data. A compositor receipt alone cannot establish that
            // a missing document service can later resolve those file URLs.
            auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Documents"),
                QStringLiteral("/org/freedesktop/portal/documents"), QStringLiteral("org.freedesktop.DBus.Properties"),
                QStringLiteral("Get"));
            message.setArguments({QStringLiteral("org.freedesktop.portal.FileTransfer"), QStringLiteral("version")});
            auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 3000), this);
            state_->filePortal = watcher;
            const auto completed = [this, token, watcher] {
                // A disconnected connection returns an already finished call
                // without Qt's private watcher helper. No finished signal is
                // emitted in that case (Qt 6.12.1 qdbuspendingcall.cpp). Both
                // completion paths must consume the same owner identity once.
                if (state_->filePortal != watcher) return;
                const QDBusPendingReply<QDBusVariant> reply = *watcher;
                state_->filePortal.clear();
                watcher->deleteLater();
                if (state_->active != token) { updateBusy(); return; }
                bool numeric = false;
                const auto version = reply.isValid() ? reply.value().variant().toUInt(&numeric) : 0u;
                if (!numeric || version < 1) {
                    const auto detail = reply.isError() ? reply.error().message() : QStringLiteral("FileTransfer version 1 is required");
                    finish(token, {{Error::Unavailable, reply.isError() ? static_cast<qint64>(reply.error().type()) : 0,
                        QStringLiteral("The desktop document portal is unavailable for file copying: ") + detail}, {}});
                    return;
                }
                validatePrepared(token);
            };
            connect(watcher, &QDBusPendingCallWatcher::finished, this, completed);
            if (watcher->isFinished()) QTimer::singleShot(0, watcher, completed);
            return token;
        }
#endif
        validatePrepared(token);
    } catch (const std::bad_alloc&) { finish(token, {allocation(), {}}); }
    return token;
}

void SystemClipboard::validatePrepared(quint64 token)
{
    if (!token || state_->active != token) return;
    QPointer<QFutureWatcher<Failure>> createdWatcher;
    bool workerStarted{};
    try {
        if (auto lease = state_->content->state_->lease) {
            // This last identity check remains off the GUI thread. The application
            // read reservation bridges preparation and actual native publication.
            auto* watcher = new QFutureWatcher<Failure>(this); state_->validation = watcher; createdWatcher = watcher;
            connect(watcher, &QFutureWatcher<Failure>::finished, this, [this, token, watcher] {
                Failure failure;
                try { failure = watcher->result(); }
                catch (const std::bad_alloc&) { failure = allocation(); }
                catch (...) { failure = {Error::Read, 0, QStringLiteral("The media identity check failed")}; }
                if (state_->validation == watcher) state_->validation.clear();
                watcher->deleteLater();
                if (state_->active != token) { updateBusy(); return; }
                if (failure) finish(token, {failure, {}}); else publishPrepared(token);
            });
            auto future = QtConcurrent::run([lease]() mutable {
                // Empty the stored task capture before QFuture reports its
                // result; no completed future may retain a hidden media lease.
                auto reservation = std::move(lease);
                try { return reservation->unchanged() ? Failure{} : changed(); }
                catch (const std::bad_alloc&) { return allocation(); }
                catch (const std::exception& error) { return Failure{Error::Read, 0, QString::fromUtf8(error.what())}; }
                catch (...) { return Failure{Error::Read, 0, QStringLiteral("The media identity check failed")}; }
            });
            workerStarted = true;
            watcher->setFuture(future);
        } else {
            QTimer::singleShot(0, this, [this, token] { if (state_->active == token) publishPrepared(token); });
        }
    } catch (const std::bad_alloc&) {
        // QtConcurrent could fail before a future was attached. Such an empty
        // watcher has no worker to drain and will never emit finished().
        if (createdWatcher && !workerStarted) {
            delete createdWatcher;
            state_->validation.clear();
        }
        finish(token, {allocation(), {}});
    }
}

void SystemClipboard::publishPrepared(quint64 token)
{
    if (!token || state_->active != token) return;
    const QPointer<SystemClipboard> lifetime(this);
    ++state_->publications;
    const auto publication = qScopeGuard([lifetime] {
        if (!lifetime) return;
        --lifetime->state_->publications;
        lifetime->updateBusy();
    });
    const auto content = state_->content;
    // System clipboard notifications may synchronously destroy their caller.
    // Keep the original reservation through the actual native handoff anyway.
    [[maybe_unused]] const auto publicationLease = content->state_->lease;
    try {
#ifdef Q_OS_WIN
        if (!state_->origin) {
            finish(token, {{Error::RequiresInteraction, 0, QStringLiteral("Copy from a live application window")}, {}}); return;
        }
        auto native = std::move(content->state_->native);
        const auto owner = reinterpret_cast<HWND>(state_->origin->winId());
        if (!lifetime || state_->active != token) return;
        const auto result = Platform::Windows::publishClipboard(native, owner);
        if (!lifetime || state_->active != token) return;
        QStringList formats;
        if (result.publishedFormats & Platform::Windows::ClipboardPng) formats.push_back(QStringLiteral("PNG"));
        if (result.publishedFormats & Platform::Windows::ClipboardDibV5) formats.push_back(QStringLiteral("CF_DIBV5"));
        if (result.publishedFormats & Platform::Windows::ClipboardDib) formats.push_back(QStringLiteral("CF_DIB"));
        if (result.publishedFormats & Platform::Windows::ClipboardFiles) formats.push_back(QStringLiteral("CF_HDROP"));
        if (result.publishedFormats & Platform::Windows::ClipboardText) formats.push_back(QStringLiteral("CF_UNICODETEXT"));
        finish(token, {fromNative(result), std::move(formats)});
#else
        auto mime = std::make_unique<QMimeData>();
        const auto& prepared = *content->state_;
        if (prepared.text) {
            mime->setText(*prepared.text);
            mime->setData(QStringLiteral("text/plain;charset=utf-8"), prepared.utf8);
        } else if (!prepared.file.isEmpty()) mime->setUrls({prepared.file});
        else { mime->setImageData(prepared.image); mime->setData(QStringLiteral("image/png"), prepared.png); }
        const auto marker = QByteArrayLiteral("application/x-huhucapture-clipboard-") + QUuid::createUuid().toByteArray(QUuid::Id128);
        mime->setData(QString::fromLatin1(marker), QByteArrayLiteral("1"));
        const auto failure = state_->observer->begin(token, state_->origin, mime.get(), marker);
        if (failure) { finish(token, {failure, {}}); return; }
        QGuiApplication::clipboard()->setMimeData(mime.release());
        if (!lifetime || state_->active != token) return;
        state_->observer->submitted();
#endif
    } catch (const std::bad_alloc&) { if (lifetime && state_->active == token) finish(token, {allocation(), {}}); }
}

void SystemClipboard::cancel(quint64 token)
{
    if (QThread::currentThread() != thread()) {
        QTimer::singleShot(0, this, [this, token] { cancel(token); }); return;
    }
    if (!token || state_->active != token) return;
    finish(token, {cancelled(), {}});
}

void SystemClipboard::finish(quint64 token, ClipboardResult result)
{
    if (state_->active == token) {
#ifndef Q_OS_WIN
        state_->observer->cancel();
#endif
        // Keep actual future/Portal watchers alive until their completion.
        // A cancelled request cannot turn unfinished I/O into an idle service
        // or enqueue unbounded replacement workers.
        // Neither the QMimeData nor an externally retained preparation keeps a
        // media lease after completion. Linux's clipboard owner remains Qt.
        if (state_->content) state_->content->state_->release();
        state_->content.reset(); state_->origin.clear(); state_->active = 0;
    }
    updateBusy();
    QTimer::singleShot(0, this, [this, token, result = std::move(result)] {
        const QPointer<SystemClipboard> lifetime(this);
        Q_EMIT finished(token, result);
        if (!lifetime) return;
        { const std::lock_guard lock(state_->admission); --state_->receipts; }
        maybeFinishShutdown();
    });
}

void SystemClipboard::updateBusy()
{
    const bool current = busy();
    if (current != state_->busyReported) {
        state_->busyReported = current;
        QTimer::singleShot(0, this, [this, current] { Q_EMIT busyChanged(current); });
    }
    maybeFinishShutdown();
}

void SystemClipboard::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_->stopping) return;
    { const std::lock_guard lock(state_->admission); state_->accepting = false; }
    state_->stopping = true;
    if (state_->active) finish(state_->active, {cancelled(), {}});
    maybeFinishShutdown();
}

void SystemClipboard::maybeFinishShutdown()
{
    if (!state_->stopping || state_->shutdownEmitted || busy()) return;
    { const std::lock_guard lock(state_->admission); if (state_->receipts) return; }
    state_->shutdownEmitted = true;
    QTimer::singleShot(0, this, [this] { Q_EMIT shutdownFinished(); });
}

} // namespace HuhuCapture::Desktop
