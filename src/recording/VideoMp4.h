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
    // Optional owned-file seam. The worker retains the stream until this call
    // returns; a supplied stream takes precedence over a filename.
    IMFByteStream* OutputStream{};
    std::function<void(GUID codec)> Prepared;
    std::function<void()> Finalizing;
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
    bool VerifyDesktop{};
    RECT ExpectedDesktop{};
    bool UseGdiCapture{};
    RECT GdiRegion{}; // physical virtual-desktop coordinates, never monitor-local
    std::uint64_t MaximumFrameBytes{256ull * 1024 * 1024};
    std::uint64_t MaximumQueuedAudioBytes{16ull * 1024 * 1024};
    unsigned long long FirstFrameTimeoutMS{5000};
    bool DurationLimitReached{}; // worker-owned, read after completion

    unsigned long long StartMS = 0; // 0, none
    unsigned long long EndMS = 0; // 0, none
    std::atomic<bool> MustEnd{ false };
    std::atomic<bool> Pause{ false };

    // Worker-owned completion facts; read only after the worker has joined.
    HuhuCapture::Core::RecordingResult result;
};

// Call on the recording worker after initializing its COM apartment. Selecting
// a missing/failed endpoint is an explicit preparation failure, never mute.
bool setAudio(DESKTOPCAPTUREPARAMS* params, bool useSpeaker, bool useMic);
int DesktopCapture(DESKTOPCAPTUREPARAMS& params);

} // namespace VideoMp4
