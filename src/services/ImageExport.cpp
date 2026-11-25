#include "pch.h"
#include "services/ImageExport.h"
#include "services/Lang.h"
#include "services/MediaHistory.h"
#include "core/PixelBuffer.h"
#include "platform/windows/DesktopClipboard.h"
#include <wincodec.h>
#include <shobjidl.h>
#include <format>
#include <filesystem>
#include <limits>

using Microsoft::WRL::ComPtr;

bool ImageExport::saveToClipboard(const int w, const int h, const BYTE* data, HWND owner)
{
    const auto layout = HuhuCapture::Core::pixelLayout(w, h, 4);
    if (!data || !layout) return false;
    auto prepared = HuhuCapture::Platform::Windows::prepareClipboardImage(w, h, layout->stride,
        {data, layout->bytes});
    if (prepared.result.error != HuhuCapture::Platform::Windows::ClipboardError::None) return false;
    return HuhuCapture::Platform::Windows::publishClipboard(prepared.payload, owner).completed();
}

bool ImageExport::saveToFile(const std::wstring& path, const int w, const int h, const BYTE* data)
{
	const auto layout = HuhuCapture::Core::pixelLayout(w, h, 4);
	if (path.empty() || !data || !layout || layout->bytes > (std::numeric_limits<UINT>::max)()) return false;
	const std::filesystem::path destination(path);
	GUID id{};
	wchar_t name[40]{};
	if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, 40)) return false;
	const auto temporaryPath = destination.parent_path() /
		(L".huhucapture-" + std::wstring(name) + L".tmp");
	// Claim a unique sibling before WIC opens it. Only a file created by this
	// operation is eligible for cleanup, even if a name collision occurs.
	HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	CloseHandle(file);
	struct TemporaryFile {
		std::filesystem::path path;
		~TemporaryFile() { DeleteFileW(path.c_str()); }
	} temporary{temporaryPath};
	ComPtr<IWICImagingFactory> factory;
	auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
	if (FAILED(hr)) return false;
	ComPtr<IWICStream> stream;
	hr = factory->CreateStream(stream.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = stream->InitializeFromFilename(temporaryPath.c_str(), GENERIC_WRITE);
	if (FAILED(hr)) return false;
	const bool encoded = SUCCEEDED(HuhuCapture::Platform::Windows::encodeDesktopPng(
        stream.Get(), w, h, layout->stride, {data, layout->bytes}));
	// The encoder/frame have committed and released their stream references by
	// now. Close the final WIC handle before renaming on Windows.
	stream.Reset();
	if (!encoded) return false;
	return MoveFileExW(temporaryPath.c_str(), destination.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::wstring ImageExport::getSaveFilePath(HWND hwnd, const std::wstring& ext)
{
	std::wstring result;
	ComPtr<IFileSaveDialog> saveDialog;
	auto hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.GetAddressOf()));
	if (FAILED(hr)) return result;
	DWORD dwFlags{ 0 };
	saveDialog->GetOptions(&dwFlags);
	saveDialog->SetOptions(dwFlags | FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES);
	auto pattern = L"*." + ext;
	auto typeName = Lang::get(L"util.file");
	COMDLG_FILTERSPEC filterSpec[]{ { typeName.c_str(), pattern.c_str() } };
	saveDialog->SetFileTypes(_countof(filterSpec), filterSpec);
	saveDialog->SetFileTypeIndex(1);
	saveDialog->SetDefaultExtension(ext.c_str());
	const auto historyPath = HuhuCapture::MediaHistory::directory();
	if (!historyPath.empty()) {
		std::error_code error;
		std::filesystem::create_directories(historyPath, error);
		ComPtr<IShellItem> folder;
		if (!error && SUCCEEDED(SHCreateItemFromParsingName(historyPath.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()))))
			saveDialog->SetFolder(folder.Get());
	}
	auto fileName = createFileName(ext);
	saveDialog->SetFileName(fileName.c_str());
	// 用户取消时 Show 返回 HRESULT_FROM_WIN32(ERROR_CANCELLED)，一样走 FAILED 分支
	hr = saveDialog->Show(hwnd);
	if (FAILED(hr)) return result;
	ComPtr<IShellItem> item;
	hr = saveDialog->GetResult(item.GetAddressOf());
	if (FAILED(hr)) return result;
	PWSTR filePath{ nullptr };
	hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
	if (FAILED(hr)) return result;
	result = filePath;
	CoTaskMemFree(filePath);
	return result;
}

std::wstring ImageExport::createFileName(const std::wstring& ext)
{
    return HuhuCapture::MediaHistory::suggestedName(ext);
}

bool ImageExport::addFileToClipboard(const std::wstring& filePath, HWND owner)
{
    auto prepared = HuhuCapture::Platform::Windows::prepareClipboardFile(filePath);
    if (prepared.result.error != HuhuCapture::Platform::Windows::ClipboardError::None) return false;
    return HuhuCapture::Platform::Windows::publishClipboard(prepared.payload, owner).completed();
}
