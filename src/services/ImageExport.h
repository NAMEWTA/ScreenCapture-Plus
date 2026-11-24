#pragma once
#include <windows.h>
#include <string>

// Image output boundary: tightly packed, top-down BGRA pixels in, PNG/file
// clipboard formats out. Keep UI dialogs and Win32 ownership details here.
class ImageExport final {
public:
    static void saveToClipboard(int width, int height, BYTE* data);
    static bool saveToFile(const std::wstring& path, int width, int height, BYTE* data);
    static std::wstring getSaveFilePath(HWND owner, const std::wstring& extension = L"png");
    static std::wstring createFileName(const std::wstring& extension);
    static void addFileToClipboard(const std::wstring& filePath);
};
