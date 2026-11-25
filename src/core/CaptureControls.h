#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace HuhuCapture::Core {

// UI-owned native surfaces, published as one immutable value. A platform
// worker validates these actual surfaces against its real readback; this is
// not a requested black-out rectangle or a claim that exclusion succeeded.
enum class CaptureControlPlacement { Excluded, Hidden, Outside };
struct CaptureControlSurface {
    std::uintptr_t nativeId{};
    CaptureControlPlacement placement{CaptureControlPlacement::Hidden};
};
struct CaptureControlSnapshot {
    std::vector<CaptureControlSurface> surfaces;
};
struct CaptureControls {
    // Null closes frame admission while a native surface is being replaced.
    // Workers retain their snapshot through readback and check it again before
    // publishing pixels. No GUI callback or mutex is held across native I/O.
    std::atomic<std::shared_ptr<const CaptureControlSnapshot>> snapshot;
};

} // namespace HuhuCapture::Core
