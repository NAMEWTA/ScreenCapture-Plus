// File-operation/reveal calls extracted from this project's MediaHistory.
// The strict PreDeleteItem guard is also used by fixed Qt 6.12.1's Windows
// QFile::moveToTrash implementation; see docs/desktop-integration.md.
#include "platform/windows/DesktopFiles.h"
#include "platform/windows/ComApartment.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <new>

namespace HuhuCapture::Platform::Windows {
namespace {
using Microsoft::WRL::ComPtr;
class DeleteSink final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileOperationProgressSink> {
public:
    RecyclePolicy policy{RecyclePolicy::RequireRecycling};
    const std::function<bool(const std::filesystem::path&)>* unchanged{};
    std::stop_token stop;
    DesktopFileResult result;
    std::size_t completed{};

    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem* item) override
    {
        if (stop.stop_requested()) {
            result.error = DesktopFileError::Cancelled;
            return result.nativeCode = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        if (policy == RecyclePolicy::RequireRecycling && !(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) {
            result.error = DesktopFileError::RecycleFailed;
            return result.nativeCode = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        PWSTR name{};
        const auto status = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
        if (FAILED(status) || !name) {
            result.error = DesktopFileError::System; result.nativeCode = FAILED(status) ? status : E_FAIL;
            if (name) CoTaskMemFree(name);
            return result.nativeCode;
        }
        bool accepted = false;
        try { accepted = !unchanged || !*unchanged || (*unchanged)(std::filesystem::path(name)); }
        catch (...) { CoTaskMemFree(name); result.error = DesktopFileError::System; return result.nativeCode = E_FAIL; }
        CoTaskMemFree(name);
        if (!accepted) { result.error = DesktopFileError::Changed; return result.nativeCode = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT status, IShellItem* created) override
    {
        if (FAILED(status)) {
            if (result.error == DesktopFileError::None) { result.error = DesktopFileError::RecycleFailed; result.nativeCode = status; }
            return S_OK;
        }
        ++completed;
        if (created) {
            PWSTR name{};
            if (SUCCEEDED(created->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                try { result.recycledPath = std::filesystem::path(name); }
                catch (...) {
                    CoTaskMemFree(name); result.error = DesktopFileError::System;
                    result.nativeCode = E_OUTOFMEMORY; return E_OUTOFMEMORY;
                }
            }
            if (name) CoTaskMemFree(name);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};

bool regularFile(const std::filesystem::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
}

DesktopFileResult recycleDesktopFiles(std::span<const std::filesystem::path> files, HWND owner,
    RecyclePolicy policy, const std::function<bool(const std::filesystem::path&)>& unchanged, std::stop_token stop)
{
    if (files.empty()) return {};
    if (stop.stop_requested()) return {DesktopFileError::Cancelled, HRESULT_FROM_WIN32(ERROR_CANCELLED), {}};
    const ComApartment com(COINIT_APARTMENTTHREADED);
    if (FAILED(com.result())) return {DesktopFileError::Initialization, com.result(), {}};
    ComPtr<IFileOperation> operation;
    auto status = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(operation.GetAddressOf()));
    if (FAILED(status)) return {DesktopFileError::Initialization, status, {}};
    if (owner && FAILED(status = operation->SetOwnerWindow(owner))) return {DesktopFileError::Initialization, status, {}};
    const DWORD flags = FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD | FOFX_EARLYFAILURE |
        FOF_NOERRORUI | FOF_NOCONFIRMATION | FOF_NO_CONNECTED_ELEMENTS | FOF_NORECURSION |
        (policy == RecyclePolicy::RequireRecycling ? FOF_SILENT : FOF_WANTNUKEWARNING);
    if (FAILED(status = operation->SetOperationFlags(flags))) return {DesktopFileError::Initialization, status, {}};
    auto sink = Microsoft::WRL::Make<DeleteSink>();
    if (!sink) return {DesktopFileError::Initialization, E_OUTOFMEMORY, {}};
    sink->policy = policy; sink->unchanged = &unchanged; sink->stop = stop;
    for (const auto& path : files) {
        if (!regularFile(path) || (unchanged && !unchanged(path))) return {DesktopFileError::Changed, HRESULT_FROM_WIN32(ERROR_FILE_INVALID), {}};
        // Qt archive paths use '/', while the Shell parsing namespace expects
        // native separators. Keep this at the OS boundary so the old native
        // product and Qt callers use the same operation and identity checks.
        auto shellPath = path; shellPath.make_preferred();
        ComPtr<IShellItem> item;
        if (FAILED(status = SHCreateItemFromParsingName(shellPath.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()))) ||
            FAILED(status = operation->DeleteItem(item.Get(), sink.Get()))) return {DesktopFileError::RecycleFailed, status, {}};
    }
    status = operation->PerformOperations();
    if (sink->result.error != DesktopFileError::None) return sink->result;
    BOOL aborted = FALSE;
    const auto abortStatus = operation->GetAnyOperationsAborted(&aborted);
    // An operation stopped by a real error is not evidence of user cancel.
    // Preserve the HRESULT before considering the separate aborted flag.
    if (FAILED(status) || FAILED(abortStatus))
        return {DesktopFileError::RecycleFailed, FAILED(status) ? status : abortStatus, {}};
    if (aborted) return {DesktopFileError::Cancelled, HRESULT_FROM_WIN32(ERROR_CANCELLED), {}};
    if (sink->completed != files.size()) return {DesktopFileError::RecycleFailed, E_FAIL, {}};
    return sink->result;
}

DesktopFileResult revealDesktopFile(const std::filesystem::path& path)
{
    if (path.empty() || !regularFile(path)) return {DesktopFileError::InvalidInput, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), {}};
    const ComApartment com(COINIT_APARTMENTTHREADED);
    if (FAILED(com.result())) return {DesktopFileError::Initialization, com.result(), {}};
    auto shellPath = path; shellPath.make_preferred();
    auto* id = ILCreateFromPathW(shellPath.c_str());
    if (!id) return {DesktopFileError::System, E_FAIL, {}};
    const auto status = SHOpenFolderAndSelectItems(id, 0, nullptr, 0); ILFree(id);
    return {SUCCEEDED(status) ? DesktopFileError::None : DesktopFileError::System, status, {}};
}

DesktopFileResult openDesktopDirectory(const std::filesystem::path& path, HWND owner)
{
    const auto attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return {DesktopFileError::InvalidInput, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), {}};
    const ComApartment com(COINIT_APARTMENTTHREADED);
    if (FAILED(com.result())) return {DesktopFileError::Initialization, com.result(), {}};
    auto shellPath = path; shellPath.make_preferred();
    SHELLEXECUTEINFOW operation{}; operation.cbSize = sizeof(operation);
    operation.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC; operation.hwnd = owner;
    operation.lpVerb = L"open"; operation.lpFile = shellPath.c_str(); operation.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&operation)) return {DesktopFileError::System, HRESULT_FROM_WIN32(GetLastError()), {}};
    return {};
}

} // namespace HuhuCapture::Platform::Windows
