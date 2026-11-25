// Adapted from Microsoft's Windows SDK XmlLite FileStream sample (MIT).
// Copyright (c) Microsoft Corporation. See third_party/windows-sdk-stream/.
#include "platform/windows/FileHandleStream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <io.h>
#include <limits>
#include <memory>
#include <new>

namespace HuhuCapture::Platform::Windows {
namespace {

HRESULT lastError() noexcept
{
    const auto error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

struct FileState final {
    explicit FileState(HANDLE value) noexcept : file(value) {}
    ~FileState() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    HANDLE file{INVALID_HANDLE_VALUE};
    SRWLOCK mutex = SRWLOCK_INIT;
};

class ExclusiveLock final {
public:
    explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
private:
    SRWLOCK& lock_;
};

class FileStream final : public IStream {
public:
    explicit FileStream(std::shared_ptr<FileState> state, std::uint64_t position = 0) noexcept
        : state_(std::move(state)), position_(position) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IStream) && iid != __uuidof(ISequentialStream))
            return E_NOINTERFACE;
        *object = static_cast<IStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG count, ULONG* read) override
    {
        if (read) *read = 0;
        if (count && !data) return STG_E_INVALIDPOINTER;
        ExclusiveLock guard(state_->mutex);
        if (const auto error = seekNative(); FAILED(error)) return error;
        DWORD actual{};
        if (!ReadFile(state_->file, data, count, &actual, nullptr)) return lastError();
        position_ += actual;
        if (read) *read = actual;
        return actual == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG count, ULONG* written) override
    {
        if (written) *written = 0;
        if (count && !data) return STG_E_INVALIDPOINTER;
        ExclusiveLock guard(state_->mutex);
        if (count > maximumPosition - position_) return STG_E_MEDIUMFULL;
        if (const auto error = seekNative(); FAILED(error)) return error;
        DWORD actual{};
        if (!WriteFile(state_->file, data, count, &actual, nullptr)) return lastError();
        position_ += actual;
        if (written) *written = actual;
        return actual == count ? S_OK : STG_E_MEDIUMFULL;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER distance, DWORD origin, ULARGE_INTEGER* position) override
    {
        ExclusiveLock guard(state_->mutex);
        std::uint64_t base{};
        if (origin == STREAM_SEEK_CUR) base = position_;
        else if (origin == STREAM_SEEK_END) {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(state_->file, &size)) return lastError();
            base = static_cast<std::uint64_t>(size.QuadPart);
        } else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        const auto negative = distance.QuadPart < 0;
        const auto amount = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(distance.QuadPart)
                                     : static_cast<std::uint64_t>(distance.QuadPart);
        if ((negative && amount > base) || (!negative && amount > maximumPosition - base))
            return STG_E_SEEKERROR;
        position_ = negative ? base - amount : base + amount;
        if (position) position->QuadPart = position_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override
    {
        if (size.QuadPart > maximumPosition) return STG_E_INVALIDFUNCTION;
        ExclusiveLock guard(state_->mutex);
        LARGE_INTEGER end{}; end.QuadPart = static_cast<LONGLONG>(size.QuadPart);
        if (!SetFilePointerEx(state_->file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(state_->file))
            return lastError();
        // IStream::SetSize must not change this stream's logical seek position.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER count,
        ULARGE_INTEGER* read, ULARGE_INTEGER* written) override
    {
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        if (!target || target == static_cast<IStream*>(this)) return STG_E_INVALIDPOINTER;
        std::array<std::uint8_t, 64 * 1024> bytes{};
        std::uint64_t remaining = count.QuadPart;
        while (remaining) {
            ULONG obtained{};
            const auto status = Read(bytes.data(), static_cast<ULONG>((std::min)(remaining,
                static_cast<std::uint64_t>(bytes.size()))), &obtained);
            if (read) read->QuadPart += obtained;
            if (FAILED(status)) return status;
            ULONG copied{};
            if (obtained) {
                const auto output = target->Write(bytes.data(), obtained, &copied);
                if (written) written->QuadPart += copied;
                if (FAILED(output)) return output;
                if (copied != obtained) return STG_E_MEDIUMFULL;
            }
            remaining -= obtained;
            if (status == S_FALSE || !obtained) return S_FALSE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD flags) override
    {
        if (flags & ~(STGC_DEFAULT | STGC_OVERWRITE | STGC_ONLYIFCURRENT | STGC_DANGEROUSLYCOMMITMERELYTODISKCACHE))
            return STG_E_INVALIDFLAG;
        ExclusiveLock guard(state_->mutex);
        return FlushFileBuffers(state_->file) ? S_OK : lastError();
    }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
    { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
    { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* status, DWORD flags) override
    {
        if (!status) return E_POINTER;
        *status = {};
        if (flags != STATFLAG_DEFAULT && flags != STATFLAG_NONAME) return STG_E_INVALIDFLAG;
        ExclusiveLock guard(state_->mutex);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(state_->file, &info)) return lastError();
        status->type = STGTY_STREAM;
        status->cbSize.QuadPart = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
        status->mtime = info.ftLastWriteTime; status->ctime = info.ftCreationTime; status->atime = info.ftLastAccessTime;
        status->grfMode = STGM_READWRITE | STGM_SHARE_DENY_WRITE;
        // There is deliberately no reopenable filename. No region locking or
        // transacted behavior is advertised for this direct owned-file stream.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** stream) override
    {
        if (!stream) return E_POINTER;
        *stream = nullptr;
        ExclusiveLock guard(state_->mutex);
        *stream = new (std::nothrow) FileStream(state_, position_);
        return *stream ? S_OK : E_OUTOFMEMORY;
    }
private:
    HRESULT seekNative() const noexcept
    {
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(position_);
        return SetFilePointerEx(state_->file, position, nullptr, FILE_BEGIN) ? S_OK : lastError();
    }
    static constexpr std::uint64_t maximumPosition = (std::numeric_limits<LONGLONG>::max)();
    std::shared_ptr<FileState> state_;
    std::uint64_t position_{};
    LONG references_{1};
};
}

HRESULT createFileHandleStream(int descriptor, IStream** stream) noexcept
{
    if (!stream) return E_POINTER;
    *stream = nullptr;
    if (descriptor < 0) return E_INVALIDARG;
    const auto native = _get_osfhandle(descriptor);
    if (native == -1) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
    const auto original = reinterpret_cast<HANDLE>(native);
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(original) != FILE_TYPE_DISK || !GetFileInformationByHandle(original, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return E_INVALIDARG;
    HANDLE duplicate{INVALID_HANDLE_VALUE};
    if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &duplicate,
        0, FALSE, DUPLICATE_SAME_ACCESS)) return lastError();
    try {
        auto state = std::make_shared<FileState>(duplicate);
        duplicate = INVALID_HANDLE_VALUE;
        *stream = new (std::nothrow) FileStream(std::move(state));
        return *stream ? S_OK : E_OUTOFMEMORY;
    } catch (const std::bad_alloc&) {
        if (duplicate != INVALID_HANDLE_VALUE) CloseHandle(duplicate);
        return E_OUTOFMEMORY;
    } catch (...) {
        if (duplicate != INVALID_HANDLE_VALUE) CloseHandle(duplicate);
        return E_FAIL;
    }
}

} // namespace HuhuCapture::Platform::Windows
