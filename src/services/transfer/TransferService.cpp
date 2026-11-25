#include "services/transfer/TransferService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace HuhuCapture::Transfer {
namespace {

const std::uint8_t* bytes(const QByteArray& value) noexcept
{
    return reinterpret_cast<const std::uint8_t*>(value.constData());
}

using Buffer = std::unique_ptr<HuhuBuffer, decltype(&huhu_buffer_free)>;
using NativeEvent = std::unique_ptr<HuhuEvent, decltype(&huhu_event_free)>;
using NativeLease = std::unique_ptr<HuhuMediaLease, decltype(&huhu_media_lease_free)>;

bool boundedText(const QString& value, std::size_t maximum)
{
    const auto utf8 = value.toUtf8();
    return !utf8.isEmpty() && !utf8.contains('\0') &&
        static_cast<std::size_t>(utf8.size()) <= maximum;
}

struct Parsed {
    int status{HUHU_PROTOCOL};
    QJsonObject data;
};

Parsed parse(HuhuSlice view, bool array = false)
{
    if (!view.data || view.length == 0 || view.length > TransferLimits::maximumControlJsonBytes)
        return {};
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(QByteArray::fromRawData(
        reinterpret_cast<const char*>(view.data), static_cast<qsizetype>(view.length)), &error);
    if (error.error != QJsonParseError::NoError || (array ? !json.isArray() : !json.isObject()))
        return {};
    return {HUHU_OK, array ? QJsonObject{{QStringLiteral("devices"), json.array()}} : json.object()};
}

Parsed takeBuffer(HuhuBuffer* raw, bool array = false)
{
    Buffer owner(raw, &huhu_buffer_free);
    HuhuSlice view{};
    const auto result = huhu_buffer_view(owner.get(), &view);
    return result == HUHU_OK ? parse(view, array) : Parsed{result, {}};
}

Event makeEvent(EventKind kind, int status, RequestId request, std::uint64_t generation,
    QJsonObject data = {})
{
    return {kind, status, request, generation, std::move(data), {}};
}

QJsonObject errorData(const char* message)
{
    return {{QStringLiteral("error"), QString::fromUtf8(message)}};
}

std::size_t eventBytes(const Event& event)
{
    auto count = static_cast<std::size_t>(QJsonDocument(event.data).toJson(QJsonDocument::Compact).size());
    count += sizeof(Event);
    for (const auto& result : event.receivedFiles) {
        count += sizeof(ReceiveResult) + sizeof(QChar) * static_cast<std::size_t>(
            result.sessionId.size() + result.fileId.size() + result.path.size());
    }
    return count;
}

} // namespace

PreparedMedia::PreparedMedia(HuhuMediaLease* native, HuhuMediaLeaseInfo info,
    std::shared_ptr<const void> historyLease)
    : native_(native), info_(info), historyLease_(std::move(historyLease))
{
}

PreparedMedia::~PreparedMedia() { huhu_media_lease_free(native_); }

PreparedMediaResult PreparedMedia::open(const QString& path, const Platform::FileSnapshot& expected,
    std::uint32_t expectedMediaKind, std::shared_ptr<const void> historyLease)
{
    if (!historyLease || expected.bytes == 0 || expectedMediaKind < HUHU_PNG || expectedMediaKind > HUHU_MP4)
        return {HUHU_INVALID_ARGUMENT, {}};
    const auto encoded = path.toUtf8();
    HuhuMediaLease* raw{};
    auto result = huhu_media_lease_open(bytes(encoded), static_cast<std::size_t>(encoded.size()), &raw);
    NativeLease owner(raw, &huhu_media_lease_free);
    if (result != HUHU_OK) return {result, {}};
    HuhuMediaLeaseInfo info{};
    result = huhu_media_lease_info(owner.get(), &info);
    if (result != HUHU_OK) return {result, {}};
    if (info.volume != expected.identity.volume || info.file != expected.identity.file)
        return {HUHU_INVALID_STATE, {}};
    if (info.bytes != expected.bytes) return {HUHU_LENGTH_MISMATCH, {}};
    if (info.media_kind != expectedMediaKind) return {HUHU_INVALID_MEDIA, {}};
    return {HUHU_OK, std::shared_ptr<const PreparedMedia>(
        new PreparedMedia(owner.release(), info, std::move(historyLease)))};
}

struct TransferService::Impl {
    enum class CommandKind { Enable, Send, Discover, Accept, CancelSend, CancelReceive };
    struct Command {
        CommandKind kind{};
        RequestId request{};
        std::uint64_t generation{};
        std::size_t terminalBudget{TransferLimits::maximumSmallJsonBytes};
        QByteArray json;
        QString text;
        QStringList fileIds;
        std::uint16_t port{};
        RequestId target{};
        std::vector<std::shared_ptr<const PreparedMedia>> media;
        std::unique_ptr<ReceiveSink> sink;
    };
    struct Notice { Event event; std::size_t bytes{}; };
    struct Operation {
        RequestId request{};
        bool sending{};
        std::size_t terminalBudget{};
        std::vector<std::shared_ptr<const PreparedMedia>> media;
    };
    struct Incoming {
        QJsonArray offered;
        std::map<std::uint64_t, QString> pending;
        std::map<QString, ReceiveResult> results;
        std::optional<Event> terminal;
        std::optional<int> localStatus;
        bool answered{};
    };

    // GUI/actor shared data. Never hold this mutex across a native/sink call.
    mutable std::mutex mutex;
    std::mutex joinMutex;
    std::condition_variable wake;
    std::deque<Command> commands;
    std::deque<Notice> notifications;
    std::map<RequestId, Notice> progress;
    std::optional<Notice> devices;
    std::size_t notificationBytes{};
    std::size_t reservedNotifications{};
    std::size_t reservedBytes{};
    ServiceState serviceState{ServiceState::Disabled};
    RequestId nextRequest{1};
    std::uint64_t nextGeneration{};
    bool closing{};
    bool stopRequested{};
    bool nativeRunning{};
    std::jthread worker;

    // Actor-only resources. These remain alive until explicit checked shutdown.
    HuhuTransfer* native{};
    std::unique_ptr<ReceiveSink> sink;
    std::map<std::uint64_t, Operation> operations;
    std::map<QString, Incoming> incoming;
    std::set<QString> declined;
    std::vector<ReceiveResult> recovery;
    std::uint64_t generation{};
    std::uint64_t nextAttempt{1};
    RequestId enablingRequest{};
    int firstFailure{HUHU_OK};
    bool disabledReserved{};
    bool listenerReserved{};
    bool sawNativeStopped{};

    static constexpr auto smallBudget = TransferLimits::maximumSmallJsonBytes;
    static constexpr auto terminalBudget = TransferLimits::maximumTerminalJsonBytes;
    static constexpr auto receiveBudget = TransferLimits::maximumReceiveTerminalBytes;

    bool roomLocked(std::size_t count, std::size_t amount) const
    {
        return count <= TransferLimits::maximumNotifications - notifications.size() - reservedNotifications &&
            amount <= TransferLimits::maximumNotificationBytes - TransferLimits::maximumSnapshotBytes -
                notificationBytes - reservedBytes;
    }

    void reserveLocked(std::size_t count, std::size_t amount)
    {
        reservedNotifications += count;
        reservedBytes += amount;
    }

    void release(std::size_t count, std::size_t amount)
    {
        std::lock_guard lock(mutex);
        if (reservedNotifications < count || reservedBytes < amount)
            throw std::logic_error("Transfer notification reservation was lost");
        reservedNotifications -= count;
        reservedBytes -= amount;
    }

    void publish(Event event, std::size_t budget)
    {
        const auto amount = eventBytes(event);
        if (amount > budget)
            throw std::length_error("Transfer terminal exceeded its reserved notification bytes");
        std::lock_guard lock(mutex);
        if (reservedNotifications == 0 || reservedBytes < budget)
            throw std::logic_error("Transfer terminal has no reserved notification");
        notifications.push_back({std::move(event), amount});
        --reservedNotifications;
        reservedBytes -= budget;
        notificationBytes += amount;
    }

    void eraseProgress(RequestId request)
    {
        std::lock_guard lock(mutex);
        progress.erase(request);
    }

    void snapshot(Event event, bool deviceList)
    {
        const auto amount = eventBytes(event);
        const auto budget = deviceList ? TransferLimits::maximumControlJsonBytes : smallBudget;
        if (amount > budget) { fail(HUHU_LIMIT_EXCEEDED, "Transfer snapshot exceeds its byte budget"); return; }
        std::lock_guard lock(mutex);
        if (deviceList) devices = Notice{std::move(event), amount};
        else if (progress.contains(event.request) || progress.size() < TransferLimits::maximumNativeOperations) {
            const auto request = event.request;
            progress.insert_or_assign(request, Notice{std::move(event), amount});
        }
    }

    void startWorkerLocked()
    {
        if (worker.joinable()) return;
        worker = std::jthread([this] {
            try { run(); }
            catch (...) {
                // Sink exceptions are caught at their call sites. This final
                // boundary still joins native callers on an internal failure.
                try {
                    fail(HUHU_PANIC, "Transfer actor failed");
                    cancelQueued();
                    stopNative();
                    std::lock_guard lock(mutex);
                    serviceState = ServiceState::Closed;
                    closing = true;
                } catch (...) { std::terminate(); }
            }
        });
    }

    Submission submit(Command command, bool enabling = false)
    {
        std::lock_guard lock(mutex);
        if (closing) return {HUHU_CLOSED, 0};
        if (enabling ? serviceState != ServiceState::Disabled : serviceState != ServiceState::Enabled)
            return {HUHU_INVALID_STATE, 0};
        const auto count = enabling ? std::size_t{3} : std::size_t{1};
        const auto amount = enabling ? smallBudget + receiveBudget + terminalBudget : command.terminalBudget;
        if (commands.size() >= TransferLimits::maximumQueuedCommands || !roomLocked(count, amount))
            return {HUHU_BUSY, 0};
        if (nextRequest == std::numeric_limits<RequestId>::max() ||
            (enabling && nextGeneration == std::numeric_limits<std::uint64_t>::max()))
            return {HUHU_INVALID_STATE, 0};
        startWorkerLocked();
        command.request = nextRequest;
        command.generation = enabling ? nextGeneration + 1 : nextGeneration;
        commands.push_back(std::move(command));
        reserveLocked(count, amount);
        if (enabling) {
            ++nextGeneration;
            stopRequested = false;
            serviceState = ServiceState::Starting;
        }
        wake.notify_one();
        return {HUHU_OK, nextRequest++};
    }

    void fail(int status, const char* detail)
    {
        if (firstFailure == HUHU_OK) firstFailure = status;
        if (listenerReserved) {
            publish(makeEvent(EventKind::ListenerFailed, status, 0, generation, errorData(detail)), terminalBudget);
            listenerReserved = false;
        }
        std::lock_guard lock(mutex);
        stopRequested = true;
        if (serviceState != ServiceState::Closed) serviceState = ServiceState::Stopping;
        wake.notify_one();
    }

    void commandEnd(const Command& command, int status)
    {
        EventKind kind = EventKind::CommandFinished;
        if (command.kind == CommandKind::Send) kind = EventKind::SendFinished;
        if (command.kind == CommandKind::Discover) kind = EventKind::DiscoveryFinished;
        QJsonObject data;
        switch (command.kind) {
        case CommandKind::Accept: data.insert(QStringLiteral("command"), QStringLiteral("accept")); break;
        case CommandKind::CancelSend: data.insert(QStringLiteral("command"), QStringLiteral("cancelSend")); break;
        case CommandKind::CancelReceive: data.insert(QStringLiteral("command"), QStringLiteral("cancelReceive")); break;
        default: break;
        }
        if (!command.text.isEmpty()) data.insert(QStringLiteral("sessionId"), command.text);
        publish(makeEvent(kind, status, command.request, command.generation, std::move(data)), command.terminalBudget);
    }

    void cancelQueued()
    {
        std::deque<Command> cancelled;
        {
            std::lock_guard lock(mutex);
            cancelled.swap(commands);
        }
        for (auto& command : cancelled) {
            if (command.kind == CommandKind::Enable) {
                command.sink->cancelAndJoin();
                publish(makeEvent(EventKind::Enabled, HUHU_CANCELLED, command.request, command.generation), smallBudget);
                publish(makeEvent(EventKind::Disabled, HUHU_CANCELLED, command.request, command.generation), receiveBudget);
                release(1, terminalBudget);
            } else commandEnd(command, HUHU_CANCELLED);
        }
    }

    void refreshDevices(QJsonObject hint = {}, int status = HUHU_OK)
    {
        HuhuBuffer* raw{};
        const auto result = huhu_transfer_devices(native, &raw);
        if (result != HUHU_OK) { fail(result, "Could not read discovery state"); return; }
        auto parsed = takeBuffer(raw, true);
        if (parsed.status != HUHU_OK) { fail(parsed.status, "Invalid discovery state"); return; }
        for (auto it = hint.begin(); it != hint.end(); ++it)
            if (it.key() != QStringLiteral("devices")) parsed.data.insert(it.key(), it.value());
        snapshot(makeEvent(EventKind::DevicesChanged, status, 0, generation, std::move(parsed.data)), true);
    }

    void enableNative(Command command)
    {
        generation = command.generation;
        enablingRequest = command.request;
        nextAttempt = 1;
        firstFailure = HUHU_OK;
        sawNativeStopped = false;
        disabledReserved = true;
        listenerReserved = true;
        sink = std::move(command.sink);
        int result = huhu_transfer_create(bytes(command.json), static_cast<std::size_t>(command.json.size()), &native);
        // Do not retain another copy of the private identity in the actor queue.
        command.json.clear();
        QJsonObject info;
        if (result == HUHU_OK) {
            HuhuBuffer* raw{};
            result = huhu_transfer_info(native, &raw);
            if (result == HUHU_OK) {
                auto parsed = takeBuffer(raw);
                result = parsed.status;
                info = std::move(parsed.data);
            }
        }
        {
            std::lock_guard lock(mutex);
            nativeRunning = native != nullptr;
            serviceState = result == HUHU_OK && !stopRequested && !closing ?
                ServiceState::Enabled : ServiceState::Stopping;
        }
        publish(makeEvent(EventKind::Enabled, result, command.request, generation, std::move(info)), smallBudget);
        if (result != HUHU_OK) {
            firstFailure = result;
            stopNative();
        } else refreshDevices();
    }

    void execute(Command command)
    {
        if (command.kind == CommandKind::Enable) { enableNative(std::move(command)); return; }
        if (!native || command.generation != generation) { commandEnd(command, HUHU_CLOSED); return; }
        int result = HUHU_INVALID_STATE;
        std::uint64_t operation{};
        switch (command.kind) {
        case CommandKind::Send: {
            std::array<const HuhuMediaLease*, TransferLimits::maximumFiles> handles{};
            for (std::size_t i = 0; i < command.media.size(); ++i) handles[i] = command.media[i]->nativeHandle();
            result = huhu_transfer_send(native, bytes(command.json), static_cast<std::size_t>(command.json.size()),
                handles.data(), command.media.size(), &operation);
            break;
        }
        case CommandKind::Discover: {
            const auto address = command.text.toUtf8();
            result = huhu_transfer_discover(native, bytes(address), static_cast<std::size_t>(address.size()),
                command.port, &operation);
            break;
        }
        case CommandKind::Accept: {
            auto session = incoming.find(command.text);
            if (session == incoming.end() || session->second.terminal || session->second.answered) {
                result = HUHU_CLOSED; break;
            }
            QJsonArray accepted;
            QJsonArray ids;
            bool unknown{};
            for (const auto& id : command.fileIds) {
                const auto file = std::find_if(session->second.offered.cbegin(), session->second.offered.cend(),
                    [&id](const QJsonValue& item) { return item.toObject().value(QStringLiteral("id")).toString() == id; });
                if (file == session->second.offered.cend()) { unknown = true; break; }
                accepted.append(*file);
                ids.append(id);
            }
            if (unknown) { result = HUHU_INVALID_ARGUMENT; break; }
            if (!accepted.isEmpty()) {
                try { result = sink->reserve(command.text, accepted); }
                catch (...) { result = HUHU_PANIC; }
                if (result != HUHU_OK) {
                    session->second.localStatus = result;
                    sink->cancel(command.text);
                    // A local refusal is explicit and distinct from a user
                    // decline in the eventual ReceiveFinished.localStatus.
                    const auto sessionBytes = command.text.toUtf8();
                    (void)huhu_transfer_accept(native, bytes(sessionBytes), static_cast<std::size_t>(sessionBytes.size()),
                        reinterpret_cast<const std::uint8_t*>("[]"), 2);
                    session->second.answered = true;
                    break;
                }
            }
            const auto sessionBytes = command.text.toUtf8();
            const auto selected = QJsonDocument(ids).toJson(QJsonDocument::Compact);
            result = huhu_transfer_accept(native, bytes(sessionBytes), static_cast<std::size_t>(sessionBytes.size()),
                bytes(selected), static_cast<std::size_t>(selected.size()));
            if (result == HUHU_OK) session->second.answered = true;
            else { sink->cancel(command.text); sink->finish(command.text); }
            break;
        }
        case CommandKind::CancelSend: {
            const auto target = std::find_if(operations.begin(), operations.end(), [&command](const auto& item) {
                return item.second.request == command.target;
            });
            result = target == operations.end() ? HUHU_CLOSED : huhu_transfer_cancel(native, target->first);
            break;
        }
        case CommandKind::CancelReceive: {
            const auto session = command.text.toUtf8();
            result = huhu_transfer_cancel_receive(native, bytes(session), static_cast<std::size_t>(session.size()));
            sink->cancel(command.text);
            break;
        }
        case CommandKind::Enable: break;
        }
        if ((command.kind == CommandKind::Send || command.kind == CommandKind::Discover) && result == HUHU_OK) {
            operations.emplace(operation, Operation{command.request, command.kind == CommandKind::Send,
                command.terminalBudget, std::move(command.media)});
        } else commandEnd(command, result);
    }

    void collectResults()
    {
        if (!sink) return;
        std::vector<ReceiveResult> results;
        try { results = sink->takeResults(); }
        catch (...) { fail(HUHU_PANIC, "Receive result collection failed"); return; }
        for (auto& result : results) {
            if (result.path.size() > static_cast<qsizetype>(TransferLimits::maximumPathCodeUnits))
                fail(HUHU_LIMIT_EXCEEDED, "Receive sink returned a path beyond the native filesystem budget");
            auto session = incoming.find(result.sessionId);
            if (session == incoming.end() || !session->second.pending.contains(result.attempt) ||
                session->second.pending.at(result.attempt) != result.fileId) {
                if (result.published) recovery.push_back(std::move(result));
                fail(HUHU_PROTOCOL, "Receive sink returned an unknown or duplicate attempt");
                continue;
            }
            if (result.status > HUHU_OK || (result.status == HUHU_OK && !result.published)) {
                result.status = HUHU_PROTOCOL;
                session->second.localStatus = HUHU_PROTOCOL;
                fail(HUHU_PROTOCOL, "Receive sink reported success without a published media file");
            }
            session->second.pending.erase(result.attempt);
            auto previous = session->second.results.find(result.fileId);
            if (previous == session->second.results.end() || previous->second.attempt < result.attempt) {
                if (previous != session->second.results.end() && previous->second.published)
                    recovery.push_back(std::move(previous->second));
                session->second.results.insert_or_assign(result.fileId, std::move(result));
            } else if (result.published) recovery.push_back(std::move(result));
        }
        finishReadySessions();
    }

    void finishReadySessions()
    {
        for (auto it = incoming.begin(); it != incoming.end();) {
            auto& session = it->second;
            if (!session.terminal || !session.pending.empty()) { ++it; continue; }
            auto event = std::move(*session.terminal);
            if (event.status == HUHU_OK && std::any_of(session.results.begin(), session.results.end(),
                [](const auto& item) { return item.second.status != HUHU_OK; })) {
                event.status = HUHU_PROTOCOL;
                session.localStatus = HUHU_PROTOCOL;
                if (firstFailure == HUHU_OK) firstFailure = HUHU_PROTOCOL;
            }
            if (session.localStatus) event.data.insert(QStringLiteral("localStatus"), *session.localStatus);
            event.receivedFiles.reserve(session.results.size());
            for (auto& [id, result] : session.results) {
                (void)id;
                event.receivedFiles.push_back(std::move(result));
            }
            publish(std::move(event), receiveBudget);
            it = incoming.erase(it);
        }
    }

    void offer(QJsonObject data, bool stopping)
    {
        const auto id = data.value(QStringLiteral("sessionId")).toString();
        const auto files = data.value(QStringLiteral("files")).toArray();
        if (!boundedText(id, 128) || files.isEmpty() || files.size() > static_cast<qsizetype>(TransferLimits::maximumFiles)) {
            fail(HUHU_PROTOCOL, "Invalid receive offer from native core"); return;
        }
        if (incoming.contains(id) || declined.contains(id)) {
            fail(HUHU_PROTOCOL, "Native core repeated a live receive session"); return;
        }
        auto event = makeEvent(EventKind::ReceiveOffer, HUHU_OK, 0, generation, std::move(data));
        const auto amount = eventBytes(event);
        bool admitted{};
        {
            std::lock_guard lock(mutex);
            admitted = !stopping && !stopRequested && !closing && roomLocked(2, amount + receiveBudget);
            if (admitted) reserveLocked(2, amount + receiveBudget);
        }
        if (!admitted) {
            if (declined.size() >= TransferLimits::maximumNotifications) {
                fail(HUHU_BUSY, "Pending automatic declines exceeded their budget"); return;
            }
            declined.insert(id);
            const auto session = id.toUtf8();
            (void)huhu_transfer_accept(native, bytes(session), static_cast<std::size_t>(session.size()),
                reinterpret_cast<const std::uint8_t*>("[]"), 2);
            return;
        }
        incoming.emplace(id, Incoming{files, {}, {}, {}, {}, false});
        publish(std::move(event), amount);
    }

    void receiveFile(HuhuEvent* raw, const QJsonObject& data, bool stopping)
    {
        const auto sessionId = data.value(QStringLiteral("sessionId")).toString();
        const auto fileId = data.value(QStringLiteral("fileId")).toString();
        auto session = incoming.find(sessionId);
        if (stopping || session == incoming.end() || session->second.terminal) return;
        if (nextAttempt == std::numeric_limits<std::uint64_t>::max()) {
            fail(HUHU_INVALID_STATE, "Receive attempt identifier space exhausted"); return;
        }
        HuhuReceive* receive{};
        auto status = huhu_event_take_receive(raw, &receive);
        if (status != HUHU_OK) { fail(status, "Could not take native receive ownership"); return; }
        const auto attempt = nextAttempt++;
        ReceiveJob job{sessionId, fileId, attempt, data.value(QStringLiteral("file")).toObject(), ReceiveHandle(receive)};
        // Record before enqueue: a fast worker may publish its result before
        // enqueue returns, but only this actor can collect that result.
        session->second.pending.emplace(attempt, fileId);
        try { status = sink->enqueue(std::move(job)); }
        catch (...) { status = HUHU_PANIC; }
        if (status != HUHU_OK) {
            session->second.pending.erase(attempt);
            session->second.results.insert_or_assign(fileId, ReceiveResult{sessionId, fileId, attempt, {}, status, false});
            session->second.localStatus = status;
            const auto text = sessionId.toUtf8();
            (void)huhu_transfer_cancel_receive(native, bytes(text), static_cast<std::size_t>(text.size()));
            sink->cancel(sessionId);
        }
    }

    void handleEvent(HuhuEvent* raw, bool stopping)
    {
        HuhuEventView view{};
        const auto viewed = huhu_event_view(raw, &view);
        if (viewed != HUHU_OK) { fail(viewed, "Could not inspect native event"); return; }
        auto parsed = parse(view.json);
        if (parsed.status != HUHU_OK) { fail(parsed.status, "Invalid native event JSON"); return; }
        switch (view.kind) {
        case HUHU_RECEIVE_OFFER: offer(std::move(parsed.data), stopping); break;
        case HUHU_RECEIVE_FILE: receiveFile(raw, parsed.data, stopping); break;
        case HUHU_RECEIVE_END: {
            const auto id = parsed.data.value(QStringLiteral("sessionId")).toString();
            if (declined.erase(id)) break;
            const auto session = incoming.find(id);
            if (session == incoming.end()) {
                if (!stopping) fail(HUHU_PROTOCOL, "Receive terminal has no admitted session");
                break;
            }
            if (view.status != HUHU_OK) sink->cancel(id);
            sink->finish(id);
            session->second.terminal = makeEvent(EventKind::ReceiveFinished, view.status, 0, generation, std::move(parsed.data));
            collectResults();
            finishReadySessions();
            break;
        }
        case HUHU_SEND_PROGRESS: {
            const auto operation = operations.find(view.operation);
            if (operation != operations.end() && !stopping)
                snapshot(makeEvent(EventKind::SendProgress, view.status, operation->second.request, generation,
                    std::move(parsed.data)), false);
            break;
        }
        case HUHU_SEND_END:
        case HUHU_DISCOVERY_END: {
            const auto operation = operations.find(view.operation);
            if (operation == operations.end()) { fail(HUHU_PROTOCOL, "Operation terminal has no admission"); break; }
            const auto expected = operation->second.sending ? HUHU_SEND_END : HUHU_DISCOVERY_END;
            if (view.kind != static_cast<std::uint32_t>(expected)) {
                fail(HUHU_PROTOCOL, "Operation terminal changed its kind"); break;
            }
            eraseProgress(operation->second.request);
            publish(makeEvent(operation->second.sending ? EventKind::SendFinished : EventKind::DiscoveryFinished,
                view.status, operation->second.request, generation, std::move(parsed.data)), operation->second.terminalBudget);
            // Both native and history leases remain owned through the actual
            // native END, regardless of any view's lifetime or subscriptions.
            operations.erase(operation);
            break;
        }
        case HUHU_DISCOVERY_CHANGED: if (!stopping) refreshDevices(std::move(parsed.data), view.status); break;
        case HUHU_LISTENER_ERROR:
            if (firstFailure == HUHU_OK) firstFailure = view.status;
            if (listenerReserved) {
                publish(makeEvent(EventKind::ListenerFailed, view.status, 0, generation, std::move(parsed.data)), terminalBudget);
                listenerReserved = false;
            }
            fail(view.status, "LocalSend listener failed");
            break;
        case HUHU_STOPPED:
            sawNativeStopped = true;
            if (view.status != HUHU_OK && firstFailure == HUHU_OK) firstFailure = view.status;
            break;
        default: fail(HUHU_PROTOCOL, "Unknown native event kind"); break;
        }
    }

    void stopNative()
    {
        if (!disabledReserved) return;
        for (auto& [id, session] : incoming) {
            (void)session;
            const auto text = id.toUtf8();
            if (native) (void)huhu_transfer_cancel_receive(native, bytes(text), static_cast<std::size_t>(text.size()));
            sink->cancel(id);
        }
        if (native) for (const auto& [id, operation] : operations) {
            (void)operation;
            (void)huhu_transfer_cancel(native, id);
        }
        sink->cancelAndJoin();
        collectResults();
        int stopped = HUHU_OK;
        if (native) {
            stopped = huhu_transfer_stop(native);
            for (;;) {
                HuhuEvent* raw{};
                const auto polled = huhu_transfer_poll(native, 0, &raw);
                NativeEvent event(raw, &huhu_event_free);
                if (polled == HUHU_CLOSED) break;
                if (polled != HUHU_OK) { fail(polled, "Stopped native queue did not drain"); break; }
                handleEvent(event.get(), true);
            }
            if (!sawNativeStopped) fail(HUHU_PROTOCOL, "Native stop omitted its terminal marker");
            huhu_transfer_free(std::exchange(native, nullptr));
        }
        collectResults();
        // Any missing terminal/outcome is an explicit contract failure after
        // the owning runtime/workers have actually joined, never fake success.
        for (const auto& [id, operation] : operations) {
            (void)id;
            publish(makeEvent(operation.sending ? EventKind::SendFinished : EventKind::DiscoveryFinished,
                HUHU_PROTOCOL, operation.request, generation, errorData("Native stop omitted an operation terminal")),
                operation.terminalBudget);
            if (firstFailure == HUHU_OK) firstFailure = HUHU_PROTOCOL;
        }
        operations.clear();
        for (auto& [id, session] : incoming) {
            if (!session.terminal) session.terminal = makeEvent(EventKind::ReceiveFinished, HUHU_PROTOCOL, 0,
                generation, {{QStringLiteral("sessionId"), id}, {QStringLiteral("error"), QStringLiteral("Native stop omitted a receive terminal")}});
            if (!session.pending.empty()) {
                session.terminal->status = HUHU_PROTOCOL;
                session.terminal->data.insert(QStringLiteral("error"), QStringLiteral("Receive sink joined without reporting an admitted attempt"));
                session.pending.clear();
            }
            if (firstFailure == HUHU_OK) firstFailure = HUHU_PROTOCOL;
        }
        finishReadySessions();
        declined.clear();
        sink.reset();
        if (listenerReserved) { release(1, terminalBudget); listenerReserved = false; }
        if (stopped != HUHU_OK && firstFailure == HUHU_OK) firstFailure = stopped;
        auto disabled = makeEvent(EventKind::Disabled, firstFailure, enablingRequest, generation);
        disabled.receivedFiles = std::move(recovery);
        publish(std::move(disabled), receiveBudget);
        disabledReserved = false;
        {
            std::lock_guard lock(mutex);
            nativeRunning = false;
            progress.clear();
            devices.reset();
            stopRequested = false;
            serviceState = closing ? ServiceState::Closed : ServiceState::Disabled;
        }
    }

    void run()
    {
        for (;;) {
            std::optional<Command> command;
            bool stop{};
            bool close{};
            {
                std::unique_lock lock(mutex);
                if (!native && commands.empty() && !stopRequested && !closing)
                    wake.wait(lock, [this] { return !commands.empty() || stopRequested || closing; });
                stop = stopRequested || closing;
                close = closing;
                if (!stop && !commands.empty()) {
                    command.emplace(std::move(commands.front()));
                    commands.pop_front();
                }
            }
            if (stop) {
                cancelQueued();
                stopNative();
                {
                    std::lock_guard lock(mutex);
                    stopRequested = false;
                    if (!native) serviceState = closing ? ServiceState::Closed : ServiceState::Disabled;
                }
                if (close) break;
                continue;
            }
            if (command) execute(std::move(*command));
            if (!native) continue;
            collectResults();
            // Drain bursts without delaying every file chunk; cap each burst
            // so a queued cancellation/stop gets another scheduling opportunity.
            for (unsigned i = 0; i < 16; ++i) {
                HuhuEvent* raw{};
                const auto result = huhu_transfer_poll(native, i == 0 ? TransferLimits::nativePollMilliseconds : 0, &raw);
                NativeEvent event(raw, &huhu_event_free);
                if (result == HUHU_TIMEOUT) break;
                if (result != HUHU_OK) { fail(result, "Native event polling failed"); break; }
                handleEvent(event.get(), false);
                std::lock_guard lock(mutex);
                if (stopRequested || closing) break;
            }
        }
    }

    void shutdown() noexcept
    {
        std::lock_guard joined(joinMutex);
        {
            std::lock_guard lock(mutex);
            closing = true;
            stopRequested = true;
            serviceState = ServiceState::Stopping;
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        serviceState = ServiceState::Closed;
    }
};

TransferService::TransferService(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>())
{
    qRegisterMetaType<Event>();
    notifications_.setInterval(TransferLimits::notificationMilliseconds);
    connect(&notifications_, &QTimer::timeout, this, &TransferService::drainNotifications);
}

TransferService::~TransferService()
{
    notifications_.stop();
    impl_->shutdown();
}

Submission TransferService::enable(QByteArray configuration, std::unique_ptr<ReceiveSink> sink)
{
    if (QThread::currentThread() != thread() || !sink || configuration.isEmpty() ||
        static_cast<std::size_t>(configuration.size()) > TransferLimits::maximumControlJsonBytes)
        return {HUHU_INVALID_ARGUMENT, 0};
    Impl::Command command;
    command.kind = Impl::CommandKind::Enable;
    command.json = std::move(configuration);
    command.sink = std::move(sink);
    const auto result = impl_->submit(std::move(command), true);
    if (result) notifications_.start();
    return result;
}

Submission TransferService::send(QByteArray verifiedPeer, std::vector<std::shared_ptr<const PreparedMedia>> media)
{
    if (QThread::currentThread() != thread() || verifiedPeer.isEmpty() ||
        static_cast<std::size_t>(verifiedPeer.size()) > TransferLimits::maximumSmallJsonBytes ||
        media.empty() || media.size() > TransferLimits::maximumFiles ||
        std::any_of(media.begin(), media.end(), [](const auto& item) { return !item; }))
        return {HUHU_INVALID_ARGUMENT, 0};
    Impl::Command command;
    command.kind = Impl::CommandKind::Send;
    command.terminalBudget = Impl::terminalBudget;
    command.json = std::move(verifiedPeer);
    command.media = std::move(media);
    return impl_->submit(std::move(command));
}

Submission TransferService::discover(QString address, std::uint16_t port)
{
    if (QThread::currentThread() != thread() || port == 0 || !boundedText(address, 64))
        return {HUHU_INVALID_ARGUMENT, 0};
    Impl::Command command;
    command.kind = Impl::CommandKind::Discover;
    command.text = std::move(address);
    command.port = port;
    return impl_->submit(std::move(command));
}

Submission TransferService::accept(QString sessionId, QStringList fileIds)
{
    if (QThread::currentThread() != thread() || !boundedText(sessionId, 128) ||
        fileIds.size() > static_cast<qsizetype>(TransferLimits::maximumFiles) ||
        std::any_of(fileIds.cbegin(), fileIds.cend(), [](const auto& id) { return !boundedText(id, 128); }))
        return {HUHU_INVALID_ARGUMENT, 0};
    if (std::set<QString>(fileIds.cbegin(), fileIds.cend()).size() != static_cast<std::size_t>(fileIds.size()))
        return {HUHU_INVALID_ARGUMENT, 0};
    Impl::Command command;
    command.kind = Impl::CommandKind::Accept;
    command.text = std::move(sessionId);
    command.fileIds = std::move(fileIds);
    return impl_->submit(std::move(command));
}

Submission TransferService::cancelSend(RequestId request)
{
    if (QThread::currentThread() != thread() || request == 0) return {HUHU_INVALID_ARGUMENT, 0};
    Impl::Command command;
    command.kind = Impl::CommandKind::CancelSend;
    command.target = request;
    return impl_->submit(std::move(command));
}

Submission TransferService::cancelReceive(QString sessionId)
{
    if (QThread::currentThread() != thread() || !boundedText(sessionId, 128))
        return {HUHU_INVALID_ARGUMENT, 0};
    Impl::Command command;
    command.kind = Impl::CommandKind::CancelReceive;
    command.text = std::move(sessionId);
    return impl_->submit(std::move(command));
}

int TransferService::disable()
{
    if (QThread::currentThread() != thread()) return HUHU_INVALID_ARGUMENT;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closing) return HUHU_CLOSED;
        if (impl_->serviceState == ServiceState::Disabled) return HUHU_OK;
        impl_->serviceState = ServiceState::Stopping;
        impl_->stopRequested = true;
    }
    impl_->wake.notify_one();
    return HUHU_OK;
}

ServiceState TransferService::state() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->serviceState;
}

Statistics TransferService::statistics() const
{
    std::lock_guard lock(impl_->mutex);
    auto bytes = impl_->notificationBytes;
    for (const auto& [request, notice] : impl_->progress) { (void)request; bytes += notice.bytes; }
    if (impl_->devices) bytes += impl_->devices->bytes;
    return {impl_->commands.size(), impl_->notifications.size(), bytes,
        impl_->reservedNotifications, impl_->reservedBytes, impl_->nativeRunning};
}

void TransferService::shutdownAndJoin() noexcept
{
    // This method is a final barrier, not an ordinary UI stop button. It may
    // also be used by the application's shutdown coordinator after disable.
    impl_->shutdown();
}

void TransferService::drainNotifications()
{
    std::vector<Event> pending;
    {
        std::lock_guard lock(impl_->mutex);
        pending.reserve(impl_->notifications.size() + impl_->progress.size() + (impl_->devices ? 1 : 0));
        for (auto& notice : impl_->notifications) pending.push_back(std::move(notice.event));
        impl_->notifications.clear();
        impl_->notificationBytes = 0;
        for (auto& [request, notice] : impl_->progress) { (void)request; pending.push_back(std::move(notice.event)); }
        impl_->progress.clear();
        if (impl_->devices) { pending.push_back(std::move(impl_->devices->event)); impl_->devices.reset(); }
        if ((impl_->serviceState == ServiceState::Disabled || impl_->serviceState == ServiceState::Closed) &&
            impl_->commands.empty()) notifications_.stop();
    }
    // Direct same-GUI-thread subscribers can delete their owner/service. No
    // actor ever dereferences QObject; stop this delivery if the facade dies.
    QPointer<TransferService> alive(this);
    for (const auto& event : pending) {
        emit eventReady(event);
        if (!alive) return;
    }
}

} // namespace HuhuCapture::Transfer
