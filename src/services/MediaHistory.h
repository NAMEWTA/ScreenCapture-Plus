#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace HuhuCapture::MediaHistory {

struct Entry {
    std::filesystem::path path;
    std::wstring name;
    std::uintmax_t bytes{};
    bool video{}; // GIF is copied as a file so its animation survives.
};

// Resolves the Windows Documents known folder, including user redirection.
// Reading the path does not create directories; an unavailable folder is empty.
std::filesystem::path directory();
std::vector<Entry> list(std::error_code* error = nullptr);
std::wstring suggestedName(const std::wstring& extension);

// Commit through a unique temporary file and a no-replace rename. An empty
// result means no complete history entry was committed. The source is retained.
std::filesystem::path archiveImage(int width, int height, const BYTE* pixels);
std::filesystem::path archiveFile(const std::filesystem::path& source);
bool copyImage(HWND owner, int width, int height, const BYTE* pixels);
bool saveImage(HWND owner, const std::wstring& destination, int width, int height, const BYTE* pixels);
void reportError(HWND owner, const std::wstring& detail = {});

bool copy(HWND owner, const Entry& entry);
bool reveal(HWND owner, const std::filesystem::path& path);
// The caller confirms. Only current, top-level managed media are recycled;
// unrelated files, directories and reparse points are never passed to Shell.
bool clear(HWND owner);

// Background-thread API. Caller initializes COM and owns the returned bitmap.
// Returns a real thumbnail or nullptr, never a generic file icon.
HBITMAP loadThumbnail(const std::filesystem::path& path, unsigned maximumEdge);

} // namespace HuhuCapture::MediaHistory
