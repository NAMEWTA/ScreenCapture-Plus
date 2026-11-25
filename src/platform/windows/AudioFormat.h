#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmreg.h>
#include <cstring>

namespace HuhuCapture::Platform {

// The source must be a complete format returned by GetMixFormat: its allocation
// contains sizeof(WAVEFORMATEX) + cbSize bytes. Do not read an extensible format
// from a shorter PCM allocation or pass a truncated extension to Initialize.
inline HRESULT copyWaveFormat(const WAVEFORMATEX* source,
                              WAVEFORMATEXTENSIBLE& destination) noexcept
{
    if (!source) return E_POINTER;
    const auto bytes = sizeof(WAVEFORMATEX) + source->cbSize;
    if (bytes > sizeof(WAVEFORMATEXTENSIBLE)
        || (source->wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && bytes < sizeof(WAVEFORMATEXTENSIBLE)))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    WAVEFORMATEXTENSIBLE copied{};
    std::memcpy(&copied, source, bytes);
    destination = copied;
    return S_OK;
}

} // namespace HuhuCapture::Platform
