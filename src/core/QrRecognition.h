#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace HuhuCapture::Core {

// A borrowed, immutable, top-down straight BGRA image. Alpha is deliberately
// ignored, as in the existing Windows QR command. Row padding is permitted.
struct QrImageView {
    int width{}, height{};
    std::size_t stride{};
    std::span<const std::uint8_t> bgra;
};

struct QrLimits {
    // quirc's context, grayscale image, 16-bit region image and flood-fill
    // stack. This excludes the caller's image and the bounded decoded output.
    std::size_t maximumWorkingBytes{256U * 1024U * 1024U};
};

enum class QrError { None, InvalidInput, BudgetExceeded, Allocation, Cancelled, Decoder };

struct QrCorner { int x{}, y{}; };
struct QrCode {
    // Keep bytes even when their Windows ANSI code page cannot be inferred on
    // Linux. dataType and eci retain quirc's highest mode / last ECI metadata.
    std::vector<std::uint8_t> payload;
    std::array<QrCorner, 4> corners{};
    int version{}, dataType{}, eccLevel{}, mask{};
    std::uint32_t eci{};
    bool mirrored{};
};

struct QrDetection {
    // quirc discovery order is part of the existing multi-code copy behavior.
    std::vector<QrCode> codes;
    std::size_t candidates{}, undecodableCandidates{};
};

// The exact heap request for a fresh recognizer, including the existing
// QUIRC_MAX_REGIONS=65534 configuration. Reject dimensions outside the pinned
// detector's signed-int indexing range before any allocation or input read.
std::optional<std::size_t> qrWorkingBytes(int width, int height) noexcept;

// Success, including a valid image with no QR codes, atomically replaces output.
// Any failure or cancellation leaves it unchanged. The monolithic upstream
// identification pass is cancellable at its boundaries, not inside quirc_end.
QrError recognizeQr(const QrImageView& image, QrDetection& output,
    const QrLimits& limits = {}, std::stop_token stop = {});

} // namespace HuhuCapture::Core
