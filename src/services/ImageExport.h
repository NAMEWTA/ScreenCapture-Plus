#pragma once
#include <windows.h>
#include <string>

// Image output boundary: tightly packed, top-down BGRA pixels in, PNG/file
// clipboard formats out. Keep UI dialogs and Win32 ownership details here.
class ImageExport final {
public:
    static bool saveToClipboard(int width, int height, const BYTE* data, HWND owner);
    static bool saveToFile(const std::wstring& path, int width, int height, const BYTE* data);
    static std::wstring getSaveFilePath(HWND owner, const std::wstring& extension = L"png");
    static std::wstring createFileName(const std::wstring& extension);
    static bool addFileToClipboard(const std::wstring& filePath, HWND owner);
};
