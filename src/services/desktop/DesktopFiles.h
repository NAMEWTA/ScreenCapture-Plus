#pragma once

#include "services/desktop/DesktopResult.h"
#include "services/media/MediaArchiveService.h"

#include <memory>
#include <atomic>
#include <stop_token>
#include <utility>

namespace HuhuCapture::Desktop {

struct RemovalResult;

// Constructed only after a real failed recycle operation. Keeping this permit
// holds the exact, previously confirmed removal reservation while the separate
// permanent-delete question is displayed. Dropping it keeps the file.
class PermanentRemovalPermit final {
public:
    const Media::Entry& entry() const noexcept { return lease_->entry(); }
private:
    explicit PermanentRemovalPermit(std::shared_ptr<const Media::MediaLease> lease) : lease_(std::move(lease)) {}
    std::shared_ptr<const Media::MediaLease> lease_;
    mutable std::atomic_bool consumed_{};
    friend RemovalResult recycle(const Media::MediaArchiveService&, const Media::Entry&, std::stop_token);
    friend RemovalResult permanentlyRemove(const std::shared_ptr<const PermanentRemovalPermit>&, std::stop_token);
};

enum class RemovalEffect { None, Recycled, PermanentlyRemoved };
struct RemovalResult {
    Failure failure;
    RemovalEffect effect{RemovalEffect::None};
    Media::Entry entry;
    QString recycledPath;
    std::shared_ptr<const PermanentRemovalPermit> permanentPermit;
    bool completed() const noexcept { return !failure && effect != RemovalEffect::None; }
};

// Synchronous worker APIs. The controller calls recycle only for the UI's
// confirmed snapshot. Busy/changed/cancelled entries are retained. There is no
// automatic fallback to permanent deletion and no implicit directory scan.
RemovalResult recycle(const Media::MediaArchiveService& archive, const Media::Entry& entry,
    std::stop_token stop = {});
// Call only after the second explicit confirmation for this permit's entry.
RemovalResult permanentlyRemove(const std::shared_ptr<const PermanentRemovalPermit>& permit,
    std::stop_token stop = {});

} // namespace HuhuCapture::Desktop

Q_DECLARE_METATYPE(HuhuCapture::Desktop::RemovalResult)
