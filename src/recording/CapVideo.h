#pragma once
#include <thread>
#include <include/Ling.h>

// Encoder implementations are private translation units; window/tool headers only
// need forward declarations for the session parameters.
namespace VideoMp4 { struct DESKTOPCAPTUREPARAMS; }
namespace VideoGif { struct GifParam; }

class WinCap;
class ToolVideo;
// 屏幕录制。区域已经由 WinCap 定好了，所以它不是窗口，只管工具条和编码线程。
class CapVideo
{
public:
	CapVideo(WinCap* win);
	~CapVideo();
	// ToolCap 原地换成 ToolVideo
	void makeTool();
	// ToolVideo 的摆放规则（就是 WinCap 那套通用规则）。建窗口时走一遍，
	// 工具条或宿主的 DPI 变了之后回头再走一遍
	void layoutTool();
	// 宿主窗口要销毁了：停掉录制、收掉工具条
	void dispose();
	bool isRecording() const;
	void startMp4(bool useSpeaker, bool useMic);
	void startGif();
	// 停止录制并返回录好的临时文件路径；没在录制时返回空串
	std::wstring stop();
	// Ctrl+S / Ctrl+C 转给工具条上的"存文件" / "存剪切板"；没在录制时返回 false
	bool onSaveKey(bool toClipboard);
private:
	void stopWorker();
	WinCap* win;
	std::unique_ptr<ToolVideo> tool;
	std::unique_ptr<VideoMp4::DESKTOPCAPTUREPARAMS> mp4Param;
	std::unique_ptr<VideoGif::GifParam> gifParam;
	std::jthread captureThread;
};
