#pragma once

#include <windows.h>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>

namespace HuhuCapture::Platform::Windows {

enum class DesktopFileError { None, InvalidInput, Initialization, Changed, Cancelled, RecycleFailed, System };
struct DesktopFileResult {
    DesktopFileError error{DesktopFileError::None};
    HRESULT nativeCode{S_OK};
    std::filesystem::path recycledPath;
    bool completed() const noexcept { return error == DesktopFileError::None; }
};
enum class RecyclePolicy { RequireRecycling, AllowSystemPermanentConfirmation };

// The old Windows history and the Qt service share these native operations.
// The GUI-free strict mode rejects a permanent-delete branch in PreDeleteItem.
// The legacy policy retains the existing Shell's extra permanent-delete prompt.
DesktopFileResult recycleDesktopFiles(std::span<const std::filesystem::path> files,
    HWND owner, RecyclePolicy policy, const std::function<bool(const std::filesystem::path&)>& unchanged,
    std::stop_token stop = {});
DesktopFileResult revealDesktopFile(const std::filesystem::path& path);
DesktopFileResult openDesktopDirectory(const std::filesystem::path& path, HWND owner = nullptr);

} // namespace HuhuCapture::Platform::Windows
