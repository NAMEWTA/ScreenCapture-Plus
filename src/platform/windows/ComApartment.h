#pragma once

#include <objbase.h>

namespace ScreenCapture::Platform {

// Declare before COM-owning locals so their Release calls happen before teardown.
class ComApartment final {
public:
    explicit ComApartment(DWORD model) noexcept : result_(CoInitializeEx(nullptr, model)) {}
    ~ComApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

} // namespace ScreenCapture::Platform
