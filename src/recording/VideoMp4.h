#pragma once

#include <windows.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <atomic>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace VideoMp4 {

struct DESKTOPCAPTUREPARAMS
{
    bool HasVideo = 1;
    bool HasAudio = 1;
    std::vector<std::tuple<std::wstring, std::vector<int>>> AudioFrom;
    GUID VIDEO_ENCODING_FORMAT = MFVideoFormat_H264;
    GUID AUDIO_ENCODING_FORMAT = MFAudioFormat_MP3;
    std::wstring f;
    void* cb = 0;
    std::function<HRESULT(const BYTE* d, size_t sz,void* cb)> Streamer;
    std::function<HRESULT(DXGI_FORMAT dxgi,int wi,int he,const BYTE* d, size_t sz,void* cb)> Framer;
    std::function<void(IMFAttributes* a)> PrepareAttributes;
    int fps = 25;
    int NumThreads = 0;
    int Qu = -1;
    int vbrm = 0;
    int vbrq = 0;
    int BR = 4000;
    int NCH = 2;
    int SR = 44100;
    int ABR = 192;
    bool Cursor = true;
    RECT rx = { 0,0,0,0 };
    HWND hWnd = 0;
    IDXGIAdapter1* ad = 0;
    UINT nOutput = 0;

    unsigned long long StartMS = 0; // 0, none
    unsigned long long EndMS = 0; // 0, none
    std::atomic<bool> MustEnd{ false };
    std::atomic<bool> Pause{ false };

    // 真正 WriteSample 成功的视频帧数。一帧都没写就不是有效视频，函数末尾据此报错 ——
    // 循环里所有失败都是 break，跟"用户点停止"走同一个出口，光看返回值分不出这两种情况
    unsigned long long framesWritten = 0;
};

void setAudio(DESKTOPCAPTUREPARAMS* params, bool useSpeaker, bool useMic);
int DesktopCapture(DESKTOPCAPTUREPARAMS& params);

} // namespace VideoMp4
