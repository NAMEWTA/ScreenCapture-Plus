#include "pch.h"
#include <include/Ling.h>
#include "recording/CapVideo.h"
#include "capture/WinCap.h"
#include "capture/CutMask.h"
#include "ui/toolbars/ToolVideo.h"
#include "app/App.h"
#include "services/Setting.h"
#include "services/Lang.h"
#include "platform/windows/NativeMp4Recording.h"
#include "platform/windows/NativeGifRecording.h"
#include "platform/FileIdentity.h"

namespace {
    std::wstring recordingPath(const wchar_t* extension)
    {
        const auto root = Setting::get()->getDataPath() / L"recordings";
        std::error_code error;
        std::filesystem::create_directories(root, error);
        GUID id{};
        wchar_t name[40]{};
        if (error || FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, 40)) return {};
        return (root / (std::wstring(L"capture-") + name + L"." + extension)).wstring();
    }

    // MP4 录制失败以前是静默 return，用户那边就是"录完什么都没有"，连缓存文件都不生成。
    // 失败原因基本都在对方机器上（混合显卡笔记本把进程放在独显上跑，桌面复制就不成立），
    // 实测让用户在系统图形设置里给本程序选"节能"（把进程放回核显）能解决，
    // 所以不去区分错误码了，统一给这一句最有用的话。
    // 录制界面的窗口是 WS_EX_TOPMOST 的，弹框得跟上，不然会被压在后面看不见
    void showRecordError(const std::wstring& text, const std::wstring& title)
    {
        MessageBoxW(nullptr, text.data(), title.data(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }


}

struct CapVideo::ActiveRecording {
    std::wstring path;
    HuhuCapture::Recording::BackendResult result;
    std::optional<HuhuCapture::Platform::FileIdentity> identity;
    std::atomic<bool> errorReported{};
};

CapVideo::CapVideo(WinCap* win) : win(win)
{
    // 区域已经定了，接下来所有交互都归工具条，宿主窗口整体让出鼠标
    win->setMouseTransparent(true);
}

CapVideo::~CapVideo()
{
    lifetime.reset();
    stopWorker();
}

void CapVideo::makeTool()
{
    tool = std::make_unique<ToolVideo>(win);
    // 尺寸在 ToolVideo 构造里算好了，这里只定位；两者都要在建窗口之前设好
    layoutTool();
    tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
    // 全屏录制时工具条只能压在录制区内部（选区外面上下都放不下），不摘出去就会被录进去
    App::excludeFromCapture(tool->hwnd);
}

void CapVideo::layoutTool()
{
    if (tool) win->layoutTool(tool.get());
}

void CapVideo::dispose()
{
    stop();
    if (tool) tool->close();
}

bool CapVideo::isRecording() const
{
    return static_cast<bool>(recording);
}

void CapVideo::startMp4(bool useSpeaker, bool useMic)
{
    if (isRecording()) return;
    const auto& cut = win->cutMask->maskRect;
    const auto left = std::max(static_cast<long>(win->x), static_cast<long>(win->x + cut.left));
    const auto top = std::max(static_cast<long>(win->y), static_cast<long>(win->y + cut.top));
    const auto right = std::min(static_cast<long>(win->x + win->w), static_cast<long>(win->x + cut.right));
    const auto bottom = std::min(static_cast<long>(win->y + win->h), static_cast<long>(win->y + cut.bottom));
    HuhuCapture::Recording::BackendRequest request;
    request.region = {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(right - left), static_cast<std::int32_t>(bottom - top)};
    request.systemAudio = useSpeaker; request.microphone = useMic;
    startRecording(request);
}

void CapVideo::startGif()
{
    if (isRecording()) return;
    const auto& cut = win->cutMask->maskRect;
    HuhuCapture::Recording::BackendRequest request;
    request.format = HuhuCapture::Recording::Format::Gif;
    request.region = {static_cast<std::int32_t>(win->x + cut.left),
        static_cast<std::int32_t>(win->y + cut.top),
        static_cast<std::int32_t>(cut.right - cut.left), static_cast<std::int32_t>(cut.bottom - cut.top)};
    request.framesPerSecond = 16; request.maximumDuration = std::chrono::minutes(6);
    request.systemAudio = false; request.microphone = false;
    startRecording(request);
}

void CapVideo::startRecording(HuhuCapture::Recording::BackendRequest request)
{
    if (isRecording()) return;
    const auto path = recordingPath(request.format == HuhuCapture::Recording::Format::Mp4 ? L"mp4" : L"gif");
    if (path.empty()) { showRecordError(Lang::get(L"video.recordFailed"), Lang::get(L"about.sysTip")); return; }
    recording = std::make_shared<ActiveRecording>(); recording->path = path;
    const auto operation = recording;
    const auto errText = Lang::get(L"video.recordFailed");
    const auto errTitle = Lang::get(L"about.sysTip");
    const auto dispatcher = Ling::App::get()->dq;
    const std::weak_ptr<int> ownerAlive = lifetime;
    try {
        captureThread = std::jthread([operation, request, dispatcher, ownerAlive, errText, errTitle](std::stop_token stop) {
            using HuhuCapture::Core::RecordingStage;
            try {
                if (stop.stop_requested()) { operation->result.stopRequested = true; return; }
                // Claim the legacy host's GUID path exclusively. The shared backend
                // then receives the same identity-checked writer as the Qt session.
                const auto file = CreateFileW(operation->path.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    operation->result.media.fail(RecordingStage::Prepare, HRESULT_FROM_WIN32(GetLastError()));
                } else {
                    // Read identity from CREATE_NEW's actual handle before closing
                    // it. A pathname replacement in the later reopen gap must not
                    // become the file this legacy host claims to own.
                    BY_HANDLE_FILE_INFORMATION identity{};
                    if (!GetFileInformationByHandle(file, &identity)) {
                        operation->result.media.fail(RecordingStage::Prepare, HRESULT_FROM_WIN32(GetLastError()));
                    } else operation->identity = HuhuCapture::Platform::FileIdentity{identity.dwVolumeSerialNumber,
                        (std::uint64_t{identity.nFileIndexHigh} << 32) | identity.nFileIndexLow};
                    if (!CloseHandle(file))
                        operation->result.media.fail(RecordingStage::Finalize, HRESULT_FROM_WIN32(GetLastError()));
                    if (!operation->result.media.firstError) {
                        std::error_code error;
                        auto writer = HuhuCapture::Platform::openOwnedEmptyFileForWrite(operation->path, *operation->identity, error);
                        if (!writer) operation->result.media.fail(RecordingStage::Prepare, error.value());
                        else operation->result = request.format == HuhuCapture::Recording::Format::Mp4
                            ? HuhuCapture::Platform::Windows::runNativeMp4(request, std::move(writer), {}, stop)
                            : HuhuCapture::Platform::Windows::runNativeGif(request, std::move(writer), {}, stop);
                    }
                }
            } catch (const std::bad_alloc&) { operation->result.media.fail(RecordingStage::Encode, E_OUTOFMEMORY); }
            catch (...) { operation->result.media.fail(RecordingStage::Encode, E_FAIL); }
            // Reuse the existing WinClipboard dispatcher/lifetime pattern. A worker
            // must not block the GUI's stop/join in a worker-owned modal MessageBox.
            if (operation->result.media.firstError) {
                try {
                    dispatcher.TryEnqueue([operation, ownerAlive, errText, errTitle] {
                        if (!ownerAlive.expired() && !operation->errorReported.exchange(true))
                            showRecordError(errText + L"\n\n" + operation->path, errTitle);
                    });
                } catch (...) { /* stop() also reports any unreported worker error. */ }
            }
        });
    } catch (...) {
        recording.reset();
        showRecordError(errText + L"\n\n" + path, errTitle);
    }
}

bool CapVideo::onSaveKey(bool toClipboard)
{
    return tool ? tool->onSaveKey(toClipboard) : false;
}

std::wstring CapVideo::stop()
{
    if (!recording) return L"";
    // 遮罩也别留在屏幕上，录完这一帧就该收工了
    win->hide();
    auto filePath = recording->path;
    captureThread.request_stop();
    stopWorker();
    auto& result = recording->result.media;
    if (!result.canArchive()) {
        // A clean immediate stop has no media to keep. On any capture, encode
        // or finalize failure retain this unique session path for recovery,
        // but never hand it to archiveFile as a completed recording.
        if (result.completion(true) == HuhuCapture::Core::RecordingCompletion::Cancelled) {
            if (recording->identity) {
                std::error_code error;
                const auto current = HuhuCapture::Platform::readFileSnapshot(filePath, error);
                if (current && current->identity != *recording->identity) {
                    result.fail(HuhuCapture::Core::RecordingStage::Finalize, HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
                } else if (current && !DeleteFileW(filePath.c_str())) {
                    const auto cleanup = GetLastError();
                    if (cleanup != ERROR_FILE_NOT_FOUND && cleanup != ERROR_PATH_NOT_FOUND)
                        result.fail(HuhuCapture::Core::RecordingStage::Finalize, HRESULT_FROM_WIN32(cleanup));
                } else if (!current && error && error != std::errc::no_such_file_or_directory) {
                    result.fail(HuhuCapture::Core::RecordingStage::Finalize, error.value());
                }
            }
        }
        filePath.clear();
    }
    const auto failed = result.firstError && !recording->errorReported.exchange(true)
        ? recording->path : std::wstring{};
    // 参数置空即"已停止"，stopIfRecording / dispose 再进来时不会重复 join
    recording.reset();
    if (!failed.empty()) showRecordError(Lang::get(L"video.recordFailed") + L"\n\n" + failed,
        Lang::get(L"about.sysTip"));
    return filePath;
}

void CapVideo::stopWorker()
{
    if (recording) captureThread.request_stop();
    if (captureThread.joinable()) captureThread.join();
}
