#ifndef HUHUCAPTURE_NATIVE_H
#define HUHUCAPTURE_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Project-owned ABI version 1. All text is UTF-8 with an explicit byte length;
 * no input pointer is retained. Calls must not run on the GUI thread when they
 * may wait for I/O. Never let a C++ exception cross an upstream gifski callback.
 * Rust-owned objects are released only by the matching functions below.
 * Handles must be live, of the correct type, and freed exactly once. Destruction
 * and stop require the caller to have joined its own API-calling workers.
 * Other calls on a transfer are thread-safe. A receive has one consumer.
 */
typedef struct HuhuTransfer HuhuTransfer;
typedef struct HuhuMediaLease HuhuMediaLease;
typedef struct HuhuReceive HuhuReceive;
typedef struct HuhuBuffer HuhuBuffer;
typedef struct HuhuEvent HuhuEvent;

enum HuhuStatus {
    HUHU_OK = 0,
    HUHU_TIMEOUT = 1,
    HUHU_END = 2,
    HUHU_PARTIAL = 3,
    HUHU_INVALID_ARGUMENT = -1,
    HUHU_INVALID_STATE = -2,
    HUHU_IO = -3,
    HUHU_CANCELLED = -4,
    HUHU_BUSY = -5,
    HUHU_PROTOCOL = -6,
    HUHU_CHECKSUM_MISMATCH = -7,
    HUHU_LENGTH_MISMATCH = -8,
    HUHU_INVALID_MEDIA = -9,
    HUHU_DURABILITY_UNKNOWN = -10,
    HUHU_CLOSED = -11,
    HUHU_PANIC = -12
};

enum HuhuEventKind {
    HUHU_RECEIVE_OFFER = 1,
    HUHU_RECEIVE_FILE = 2,
    HUHU_RECEIVE_END = 3,
    HUHU_SEND_PROGRESS = 4,
    HUHU_SEND_END = 5,
    HUHU_DISCOVERY_CHANGED = 6,
    HUHU_DISCOVERY_END = 7,
    HUHU_LISTENER_ERROR = 8,
    HUHU_STOPPED = 9
};

enum HuhuMediaKind {
    HUHU_PNG = 1,
    HUHU_GIF = 2,
    HUHU_MP4 = 3
};

typedef struct HuhuSlice {
    const uint8_t* data;
    size_t length;
} HuhuSlice;

typedef struct HuhuEventView {
    uint32_t kind;
    int32_t status;
    uint64_t operation;
    HuhuSlice json;
} HuhuEventView;

typedef struct HuhuVerification {
    uint64_t received_bytes;
    uint8_t sha256[32];
    uint32_t sender_checksum_matched;
    uint32_t media_kind;
} HuhuVerification;

uint32_t huhu_native_abi_version(void);

/* Generates an official LocalSend RSA identity. The JSON contains certPem,
 * privateKeyPem and fingerprint. Persist it with the user's configuration;
 * do not log its private key or silently regenerate a damaged saved identity.
 */
int32_t huhu_identity_generate(HuhuBuffer** result);
int32_t huhu_buffer_view(const HuhuBuffer* buffer, HuhuSlice* result);
void huhu_buffer_free(HuhuBuffer* buffer);

/* config JSON:
 * {"alias":"name","addresses":["127.0.0.1"],"port":0,
 *  "certPem":"...","privateKeyPem":"...","discovery":false}
 * Optional: pin (string), maxFileBytes (1..32 GiB), decisionTimeoutMs
 * (1..300000), idleTimeoutMs (1..120000), commitTimeoutMs (1..300000).
 * Concrete selected IPv4 addresses only; never wildcard binding. port=0 is
 * useful for probes; production normally uses the protocol port 53317.
 * discovery=true starts the official bounded IPv4 multicast discovery on
 * those interfaces. Its multicast failure is reported; manual TLS discovery
 * remains usable. TLS and checksum verification cannot be disabled.
 */
int32_t huhu_transfer_create(const uint8_t* config, size_t length, HuhuTransfer** result);
/* Returns {port,fingerprint,addresses}. No private key is returned. */
int32_t huhu_transfer_info(const HuhuTransfer* transfer, HuhuBuffer** result);

/* Poll the bounded event queue, waiting at most 1000 ms. TIMEOUT has no event;
 * CLOSED means stop completed and the queue is empty. Terminal events reserve
 * capacity before admitting their operation; discovery/progress updates may
 * coalesce. Every successful send/discover admission has exactly one END.
 * Event JSON contracts are documented in docs/rust-bridge.md.
 */
int32_t huhu_transfer_poll(HuhuTransfer* transfer, uint32_t timeout_ms, HuhuEvent** result);
int32_t huhu_event_view(const HuhuEvent* event, HuhuEventView* result);
/* Only RECEIVE_FILE carries a receive. Ownership moves out once; freeing an
 * untaken RECEIVE_FILE rejects its body instead of acknowledging it.
 */
int32_t huhu_event_take_receive(HuhuEvent* event, HuhuReceive** result);
void huhu_event_free(HuhuEvent* event);

/* accepted_ids is a JSON array of offered file IDs, up to 64. An empty array
 * explicitly declines. An expired/cancelled offer returns CLOSED.
 */
int32_t huhu_transfer_accept(HuhuTransfer* transfer, const uint8_t* session,
    size_t session_length, const uint8_t* accepted_ids, size_t ids_length);
int32_t huhu_transfer_cancel_receive(HuhuTransfer* transfer,
    const uint8_t* session, size_t session_length);

/* Opens the selected ordinary file ONCE, rejects symlinks/reparse points,
 * records its size and PNG/GIF/MP4 name, and retains its OS file identity.
 * Never reopen the pathname in an async send. User media must remain immutable
 * while leased; the application also keeps its history deletion lease alive.
 * A lease can participate in one send at a time. Freeing the C handle during
 * a send is allowed: the admitted operation retains its own reference.
 */
int32_t huhu_media_lease_open(const uint8_t* path, size_t length, HuhuMediaLease** result);
void huhu_media_lease_free(HuhuMediaLease* lease);

/* peer JSON: {"address":"127.0.0.1","port":53317,
 *             "fingerprint":"64 hex digits","pin":"optional"}.
 * Uses the verified peer fingerprint, never just the discovery alias.
 * Borrows 1..64 lease handles. On OK, returns an operation ID. END reports
 * success only after every accepted file receives the peer's HTTP success;
 * rejected files and partial acceptance are reported explicitly in its JSON.
 */
int32_t huhu_transfer_send(HuhuTransfer* transfer, const uint8_t* peer,
    size_t peer_length, const HuhuMediaLease* const* leases, size_t lease_count,
    uint64_t* operation);
int32_t huhu_transfer_discover(HuhuTransfer* transfer, const uint8_t* address,
    size_t address_length, uint16_t port, uint64_t* operation);
int32_t huhu_transfer_devices(const HuhuTransfer* transfer, HuhuBuffer** result);
int32_t huhu_transfer_cancel(HuhuTransfer* transfer, uint64_t operation);

/* read copies into caller memory; capacity must be 1..512 KiB. OK always
 * produces at least one byte. END only means stream EOF, not validation or
 * persistence. Call verify after END, even when the file was short. Timeout
 * is bounded to 1000 ms and does not consume the future's eventual result.
 */
int32_t huhu_receive_read(HuhuReceive* receive, uint8_t* destination,
    size_t capacity, size_t* written, uint32_t timeout_ms);
int32_t huhu_receive_verify(HuhuReceive* receive, uint32_t timeout_ms,
    HuhuVerification* result);
/* Returns 1 if cancelled/stopped; 0 otherwise; negative status on bad input. */
int32_t huhu_receive_is_cancelled(const HuhuReceive* receive);
/* After verify returns OK: decode/probe the COMPLETE staging file with the
 * application codec, recheck is_cancelled, then perform the shared durable
 * FileCommitter no-replace commit. Only its actual durable success permits
 * status=OK with the matching media_kind. IO, CANCELLED, INVALID_MEDIA and
 * DURABILITY_UNKNOWN may be sent on failure. A published destination with
 * uncertain durability remains the caller's real file and must be preserved.
 * Each receive is completed exactly once; early success is rejected.
 */
int32_t huhu_receive_complete(HuhuReceive* receive, int32_t status, uint32_t media_kind);
void huhu_receive_free(HuhuReceive* receive);

/* Cancel and JOIN all listeners, discovery and transfer tasks; never a timed
 * detach. No callback is made into C/C++. The queue remains drainable, ending
 * with STOPPED after per-operation terminal events. This may wait for an OS
 * file operation already in progress. Idempotent. Free calls stop if needed.
 */
int32_t huhu_transfer_stop(HuhuTransfer* transfer);
void huhu_transfer_free(HuhuTransfer* transfer);

#ifdef __cplusplus
}
#endif
#endif
