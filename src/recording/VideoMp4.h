#pragma once

#include "core/RecordingResult.h"
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

    // Worker-owned completion facts; read only after the worker has joined.
    HuhuCapture::Core::RecordingResult result;
};

void setAudio(DESKTOPCAPTUREPARAMS* params, bool useSpeaker, bool useMic);
int DesktopCapture(DESKTOPCAPTUREPARAMS& params);

} // namespace VideoMp4
