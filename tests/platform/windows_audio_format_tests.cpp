#include "platform/windows/AudioFormat.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main()
{
    // A plain WAVEFORMATEX allocation has no extension to read. Its destination
    // extension must be initialized, not copied from adjacent stack memory.
    WAVEFORMATEX pcm{};
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = 2;
    pcm.nSamplesPerSec = 44100;
    pcm.wBitsPerSample = 16;
    pcm.nBlockAlign = 4;
    pcm.nAvgBytesPerSec = 176400;
    WAVEFORMATEXTENSIBLE copied{};
    copied.Samples.wValidBitsPerSample = 0xFFFF;
    copied.dwChannelMask = 0xFFFFFFFF;
    require(SUCCEEDED(HuhuCapture::Platform::copyWaveFormat(&pcm, copied)),
            "plain PCM format was rejected");
    require(std::memcmp(&pcm, &copied.Format, sizeof(pcm)) == 0,
            "plain PCM fields changed during the copy");
    require(copied.Samples.wValidBitsPerSample == 0 && copied.dwChannelMask == 0,
            "plain PCM read bytes beyond the source allocation");

    // Put the same short source immediately before an inaccessible page. This
    // makes an accidental sizeof(WAVEFORMATEXTENSIBLE) read fail deterministically.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto pageSize = static_cast<size_t>(system.dwPageSize);
    auto* pages = static_cast<BYTE*>(VirtualAlloc(nullptr, pageSize * 2,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(pages != nullptr, "could not allocate the guarded format buffer");
    DWORD oldProtection = 0;
    require(VirtualProtect(pages + pageSize, pageSize, PAGE_NOACCESS, &oldProtection) != FALSE,
            "could not protect the guard page");
    auto* guardedPcm = reinterpret_cast<WAVEFORMATEX*>(pages + pageSize - sizeof(WAVEFORMATEX));
    std::memcpy(guardedPcm, &pcm, sizeof(pcm));
    require(SUCCEEDED(HuhuCapture::Platform::copyWaveFormat(guardedPcm, copied)),
            "guarded plain PCM format was rejected");
    require(std::memcmp(&pcm, &copied.Format, sizeof(pcm)) == 0,
            "guarded format fields changed during the copy");
    require(VirtualFree(pages, 0, MEM_RELEASE) != FALSE,
            "could not release the guarded format buffer");

    WAVEFORMATEX floating = pcm;
    floating.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    floating.nSamplesPerSec = 48000;
    floating.wBitsPerSample = 32;
    floating.nBlockAlign = 8;
    floating.nAvgBytesPerSec = 384000;
    require(SUCCEEDED(HuhuCapture::Platform::copyWaveFormat(&floating, copied)),
            "plain floating-point format was rejected");
    require(std::memcmp(&floating, &copied.Format, sizeof(floating)) == 0,
            "floating-point format changed during the copy");

    WAVEFORMATEXTENSIBLE extensible{};
    extensible.Format = floating;
    extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible.Format.cbSize = static_cast<WORD>(sizeof(extensible) - sizeof(WAVEFORMATEX));
    extensible.Samples.wValidBitsPerSample = 32;
    extensible.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    extensible.SubFormat = { 3, 0, 0x0010, { 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 } };
    require(SUCCEEDED(HuhuCapture::Platform::copyWaveFormat(&extensible.Format, copied)),
            "complete extensible format was rejected");
    require(std::memcmp(&extensible, &copied, sizeof(extensible)) == 0,
            "extensible fields changed during the copy");

    const auto original = copied;
    require(HuhuCapture::Platform::copyWaveFormat(nullptr, copied) == E_POINTER,
            "null source did not report E_POINTER");
    require(std::memcmp(&original, &copied, sizeof(copied)) == 0,
            "null source changed the output");

    WAVEFORMATEX incomplete = pcm;
    incomplete.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    require(HuhuCapture::Platform::copyWaveFormat(&incomplete, copied)
                == AUDCLNT_E_UNSUPPORTED_FORMAT,
            "extensible format with no extension was accepted");
    incomplete.cbSize = static_cast<WORD>(sizeof(extensible) - sizeof(WAVEFORMATEX) - 1);
    require(HuhuCapture::Platform::copyWaveFormat(&incomplete, copied)
                == AUDCLNT_E_UNSUPPORTED_FORMAT,
            "short extensible format was accepted");

    WAVEFORMATEX oversized = pcm;
    oversized.cbSize = (std::numeric_limits<WORD>::max)();
    require(HuhuCapture::Platform::copyWaveFormat(&oversized, copied)
                == AUDCLNT_E_UNSUPPORTED_FORMAT,
            "oversized extension was truncated and accepted");
    require(std::memcmp(&original, &copied, sizeof(copied)) == 0,
            "a rejected format changed the output");

    std::cout << "Windows audio format tests passed\n";
}
