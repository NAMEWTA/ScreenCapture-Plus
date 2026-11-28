#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <stdexcept>
#include <windows.h>

namespace HuhuCapture::Tests {

class ClipboardLock final {
public:
    ClipboardLock()
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!OpenClipboard(nullptr)) {
            if (elapsed.elapsed() >= 3000)
                throw std::runtime_error("The receiving process could not open the Windows clipboard");
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
    }
    ~ClipboardLock() { CloseClipboard(); }
    ClipboardLock(const ClipboardLock&) = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;
};

class GlobalRead final {
public:
    explicit GlobalRead(UINT format)
    {
        handle_ = static_cast<HGLOBAL>(GetClipboardData(format));
        if (!handle_) throw std::runtime_error("The Windows clipboard lacks a required format");
        size = GlobalSize(handle_);
        data = static_cast<const unsigned char*>(GlobalLock(handle_));
        if (!data) throw std::runtime_error("Could not lock the transferred Windows clipboard memory");
    }
    ~GlobalRead() { GlobalUnlock(handle_); }
    GlobalRead(const GlobalRead&) = delete;
    GlobalRead& operator=(const GlobalRead&) = delete;
    const unsigned char* data{};
    SIZE_T size{};
private:
    HGLOBAL handle_{};
};

}
