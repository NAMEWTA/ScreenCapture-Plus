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
#include "platform/FileIdentity.h"
#include "recording/VideoGif.h"
#include <atomic>
#include <filesystem>
#include <new>
#include <optional>
#include <system_error>

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

struct CapVideo::Mp4Recording {
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
    return mp4Param || gifParam;
}

void CapVideo::startMp4(bool useSpeaker, bool useMic)
{
    if (isRecording()) return;
    const auto path = recordingPath(L"mp4");
    if (path.empty()) { showRecordError(Lang::get(L"video.recordFailed"), Lang::get(L"about.sysTip")); return; }
    const auto& cut = win->cutMask->maskRect;
    const auto left = std::max(static_cast<long>(win->x), static_cast<long>(win->x + cut.left));
    const auto top = std::max(static_cast<long>(win->y), static_cast<long>(win->y + cut.top));
    const auto right = std::min(static_cast<long>(win->x + win->w), static_cast<long>(win->x + cut.right));
    const auto bottom = std::min(static_cast<long>(win->y + win->h), static_cast<long>(win->y + cut.bottom));
    HuhuCapture::Recording::BackendRequest request;
    request.region = {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(right - left), static_cast<std::int32_t>(bottom - top)};
    request.systemAudio = useSpeaker; request.microphone = useMic;
    mp4Param = std::make_shared<Mp4Recording>(); mp4Param->path = path;
    const auto operation = mp4Param;
    const auto errText = Lang::get(L"video.recordFailed");
    const auto errTitle = Lang::get(L"about.sysTip");
    const auto dispatcher = Ling::App::get()->dq;
    const std::weak_ptr<int> ownerAlive = lifetime;
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
            } else if (!CloseHandle(file)) {
                operation->result.media.fail(RecordingStage::Finalize, HRESULT_FROM_WIN32(GetLastError()));
            } else {
                std::error_code error;
                const auto claimed = HuhuCapture::Platform::readFileSnapshot(operation->path, error);
                if (!claimed) operation->result.media.fail(RecordingStage::Prepare, error.value());
                else {
                    operation->identity = claimed->identity;
                    auto writer = HuhuCapture::Platform::openOwnedEmptyFileForWrite(operation->path, claimed->identity, error);
                    if (!writer) operation->result.media.fail(RecordingStage::Prepare, error.value());
                    else operation->result = HuhuCapture::Platform::Windows::runNativeMp4(request, std::move(writer), {}, stop);
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
}

void CapVideo::startGif()
{
    if (isRecording()) return;
    auto& cutMask = win->cutMask;
    const auto videoTempPath = recordingPath(L"gif");
    if (videoTempPath.empty()) { showRecordError(Lang::get(L"video.recordFailed"), Lang::get(L"about.sysTip")); return; }
    gifParam = std::make_unique<VideoGif::GifParam>();
    gifParam->x = (int)(win->x + cutMask->maskRect.left);
    gifParam->y = (int)(win->y + cutMask->maskRect.top);
    gifParam->w = (int)(cutMask->maskRect.right - cutMask->maskRect.left);
    gifParam->h = (int)(cutMask->maskRect.bottom - cutMask->maskRect.top);
    gifParam->path = videoTempPath;
    const auto errText = Lang::get(L"video.recordFailed");
    const auto errTitle = Lang::get(L"about.sysTip");
    captureThread = std::jthread([this, errText, errTitle](std::stop_token) {
        try {
            VideoGif::createGif(gifParam.get());
            if (gifParam->result.firstError)
                showRecordError(errText + L"\n\n" + gifParam->path, errTitle);
        }
        catch (...) {
            // The GIF encoder and DIB own their resources even on allocation failure.
            gifParam->result.fail(HuhuCapture::Core::RecordingStage::Encode, E_FAIL);
            showRecordError(errText + L"\n\n" + gifParam->path, errTitle);
        }
    });
}

bool CapVideo::onSaveKey(bool toClipboard)
{
    return tool ? tool->onSaveKey(toClipboard) : false;
}

std::wstring CapVideo::stop()
{
    if (!mp4Param && !gifParam) return L"";
    // 遮罩也别留在屏幕上，录完这一帧就该收工了
    win->hide();
    std::wstring filePath;
    if (mp4Param) {
        captureThread.request_stop();
        filePath = mp4Param->path;
    }
    if (gifParam) {
        gifParam->isFinish = true;
        filePath = gifParam->path;
    }
    stopWorker();
    const auto& result = mp4Param ? mp4Param->result.media : gifParam->result;
    if (!result.canArchive()) {
        // A clean immediate stop has no media to keep. On any capture, encode
        // or finalize failure retain this unique session path for recovery,
        // but never hand it to archiveFile as a completed recording.
        if (result.completion(true) == HuhuCapture::Core::RecordingCompletion::Cancelled) {
            if (mp4Param && mp4Param->identity) {
                std::error_code error;
                const auto current = HuhuCapture::Platform::readFileSnapshot(filePath, error);
                if (current && current->identity == *mp4Param->identity) DeleteFileW(filePath.c_str());
            } else if (!mp4Param) DeleteFileW(filePath.c_str());
        }
        filePath.clear();
    }
    const auto failedMp4 = mp4Param && result.firstError && !mp4Param->errorReported.exchange(true)
        ? mp4Param->path : std::wstring{};
    // 参数置空即"已停止"，stopIfRecording / dispose 再进来时不会重复 join
    mp4Param.reset();
    gifParam.reset();
    if (!failedMp4.empty()) showRecordError(Lang::get(L"video.recordFailed") + L"\n\n" + failedMp4,
        Lang::get(L"about.sysTip"));
    return filePath;
}

void CapVideo::stopWorker()
{
    if (mp4Param) captureThread.request_stop();
    if (gifParam) gifParam->isFinish = true;
    if (captureThread.joinable()) captureThread.join();
}
