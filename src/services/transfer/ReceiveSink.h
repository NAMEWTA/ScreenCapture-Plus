#pragma once

#include "platform/rust/include/huhucapture_native.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

namespace HuhuCapture::Transfer {

struct ReceiveDeleter {
    void operator()(HuhuReceive* value) const noexcept { huhu_receive_free(value); }
};
using ReceiveHandle = std::unique_ptr<HuhuReceive, ReceiveDeleter>;

struct ReceiveJob {
    QString sessionId;
    QString fileId;
    std::uint64_t attempt{};
    QJsonObject file;
    ReceiveHandle receive;
};

struct ReceiveResult {
    QString sessionId;
    QString fileId;
    std::uint64_t attempt{};
    QString path;
    int status{HUHU_INVALID_STATE};
    bool published{};
};

struct ReceiveProgress {
    QString sessionId;
    QString fileId;
    std::uint64_t attempt{};
    std::uint64_t bytes{};
    std::uint64_t total{};
};

// Implemented by the shared media service, never by a window. Every method is
// called on the TransferService actor. A sink owns a bounded independent worker
// queue; workers consume each ReceiveHandle exclusively and never wait for GUI
// callbacks. Create a fresh, idle sink for each enable generation.
class ReceiveSink {
public:
    virtual ~ReceiveSink() = default;

    // Before the protocol accept: validate the selected metadata and reserve
    // its aggregate disk/concurrency budget. Failure must roll back its partial
    // reservations. Return OK, LIMIT_EXCEEDED (policy/concurrency budget), IO
    // (storage unavailable), or CANCELLED. No body bytes are accepted yet; these
    // errors remain distinct standard 413/507/403 responses to the sender.
    virtual int reserve(const QString& sessionId, const QJsonArray& acceptedFiles) = 0;

    // Takes the only body consumer. Return OK only after an actual bounded
    // worker owns the job; every other result must discard/complete the job
    // without success. The actor then cancels that receive session. A successful
    // enqueue is not an archive result. The worker must read EOF, verify, decode
    // the COMPLETE media, recheck cancellation, durably commit without replacing
    // an existing file, then call complete(OK). Preserve every published path on
    // late cancellation/durability failure. No bytes or handles enter the UI.
    virtual int enqueue(ReceiveJob job) = 0;

    // enqueue must reserve a result slot before accepting ownership. Every OK
    // enqueue produces exactly one outcome after releasing its receive; failed
    // admissions produce none. Publication into that reserved slot cannot wait
    // for the actor to drain it (cancelAndJoin would otherwise deadlock). The
    // actor drains these bounded values and delays ReceiveFinished until every
    // handed-off attempt has an outcome, retaining published recovery paths.
    virtual std::vector<ReceiveResult> takeResults() = 0;

    // Optional-in-time snapshots of actual bytes already written, coalesced in
    // the existing bounded outcome slots. Return only changed values. Counts
    // reaching total never authorize success; decoding/commit still follow.
    virtual std::vector<ReceiveProgress> takeProgress() = 0;

    // Cancel work and release unstarted reservations. These must not require
    // a running Qt event loop; finish releases the unused session reservation
    // after the protocol terminal without revoking already committed media.
    virtual void cancel(const QString& sessionId) noexcept = 0;
    virtual void finish(const QString& sessionId) noexcept = 0;

    // Request all worker stops and JOIN all API callers before returning.
    // TransferService calls this before stop/free on the Rust engine. No timed
    // detach and no receive left for a GUI callback to release afterward.
    virtual void cancelAndJoin() noexcept = 0;
};

} // namespace HuhuCapture::Transfer
