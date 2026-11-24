#include "pch.h"
#include "services/ImageExport.h"
#include "services/Lang.h"
#include "services/MediaHistory.h"
#include "core/PixelBuffer.h"
#include <wincodec.h>
#include <shobjidl.h>
#include <format>
#include <filesystem>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace {
	// 把 BGRA top-down 像素编码成 PNG 写进 stream。saveToClipboard 和 saveToFile 共用这段。
	bool encodePng(IStream* stream, const int w, const int h, BYTE* data)
	{
		const auto layout = HuhuCapture::Core::pixelLayout(w, h, 4);
		if (!data || !layout || layout->bytes > (std::numeric_limits<UINT>::max)()) return false;
		const auto rowBytes = static_cast<UINT>(layout->stride);
		const auto imgBytes = static_cast<UINT>(layout->bytes);
		ComPtr<IWICImagingFactory> factory;
		auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
		if (FAILED(hr)) return false;
		ComPtr<IWICBitmapEncoder> encoder;
		hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
		if (FAILED(hr)) return false;
		hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
		if (FAILED(hr)) return false;
		ComPtr<IWICBitmapFrameEncode> frame;
		hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
		if (FAILED(hr)) return false;
		hr = frame->Initialize(nullptr);
		if (FAILED(hr)) return false;
		hr = frame->SetSize((UINT)w, (UINT)h);
		if (FAILED(hr)) return false;
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
		hr = frame->SetPixelFormat(&fmt);
		if (FAILED(hr) || !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) return false;
		hr = frame->WritePixels((UINT)h, rowBytes, imgBytes, data);
		if (FAILED(hr)) return false;
		hr = frame->Commit();
		if (FAILED(hr)) return false;
		return SUCCEEDED(encoder->Commit());
	}

}

bool ImageExport::saveToClipboard(const int w, const int h, BYTE* data, HWND owner)
{
	const auto layout = HuhuCapture::Core::pixelLayout(w, h, 4);
	if (!data || !layout || layout->bytes > (std::numeric_limits<DWORD>::max)()) return false;
	const auto rowBytes = static_cast<DWORD>(layout->stride);
	const auto imgBytes = static_cast<DWORD>(layout->bytes);

	// ---------- 1) PNG 编码到内存流 ----------
	ComPtr<IStream> pngStream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, pngStream.GetAddressOf()))) return false;
	if (!encodePng(pngStream.Get(), w, h, data)) return false;
	// 流内部的 HGLOBAL 尺寸可能大于实际字节数，拷一份精确大小的出来给剪切板
	STATSTG stat{};
	if (FAILED(pngStream->Stat(&stat, STATFLAG_NONAME))) return false;
	SIZE_T pngSize = (SIZE_T)stat.cbSize.QuadPart;
	if (pngSize == 0) return false;
	HGLOBAL hPngSrc{ nullptr };
	if (FAILED(GetHGlobalFromStream(pngStream.Get(), &hPngSrc)) || !hPngSrc) return false;
	auto srcPtr = GlobalLock(hPngSrc);
	if (!srcPtr) return false;
	HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, pngSize);
	if (!hPng) { GlobalUnlock(hPngSrc); return false; }
	auto dstPtr = GlobalLock(hPng);
	if (!dstPtr) { GlobalUnlock(hPngSrc); GlobalFree(hPng); return false; }
	CopyMemory(dstPtr, srcPtr, pngSize);
	GlobalUnlock(hPng);
	GlobalUnlock(hPngSrc);

	// ---------- 2) 构造 CF_DIBV5（带 alpha） ----------
	HGLOBAL hDibV5 = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + imgBytes);
	if (!hDibV5) { GlobalFree(hPng); return false; }
	auto pv5 = static_cast<BYTE*>(GlobalLock(hDibV5));
	if (!pv5) { GlobalFree(hDibV5); GlobalFree(hPng); return false; }
	auto bv5 = reinterpret_cast<BITMAPV5HEADER*>(pv5);
	*bv5 = {};
	bv5->bV5Size = sizeof(BITMAPV5HEADER);
	bv5->bV5Width = w;
	bv5->bV5Height = -h;                  // 负 = top-down
	bv5->bV5Planes = 1;
	bv5->bV5BitCount = 32;
	bv5->bV5Compression = BI_BITFIELDS;   // 让接收端识别 alpha
	bv5->bV5SizeImage = imgBytes;
	bv5->bV5RedMask = 0x00FF0000;
	bv5->bV5GreenMask = 0x0000FF00;
	bv5->bV5BlueMask = 0x000000FF;
	bv5->bV5AlphaMask = 0xFF000000;
	bv5->bV5CSType = LCS_sRGB;
	bv5->bV5Intent = LCS_GM_GRAPHICS;
	CopyMemory(pv5 + sizeof(BITMAPV5HEADER), data, imgBytes);
	GlobalUnlock(hDibV5);

	// ---------- 3) 构造 CF_DIB（24bpp、BI_RGB、自下而上） ----------
	// 老软件（比如 Illustrator 2020）只认最传统的这一种 DIB：注册格式 PNG 它不查，
	// CF_DIBV5 它不认，32bpp + BI_BITFIELDS 和 top-down 也读不了。系统虽然能从 CF_DIBV5
	// 合成出 CF_DIB，合成出来的仍是那份带 alpha 的 32 位数据，一样不合它的口味。
	// 所以显式再放一份最保守的：丢掉 alpha 写成 24 位，行按 4 字节对齐，自下而上排列
	DWORD dibRowBytes = ((DWORD)w * 3 + 3) & ~3u;
	DWORD dibImgBytes = dibRowBytes * (DWORD)h;
	HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPINFOHEADER) + dibImgBytes);
	if (!hDib) { GlobalFree(hDibV5); GlobalFree(hPng); return false; }
	auto pDib = static_cast<BYTE*>(GlobalLock(hDib));
	if (!pDib) { GlobalFree(hDib); GlobalFree(hDibV5); GlobalFree(hPng); return false; }
	auto bi = reinterpret_cast<BITMAPINFOHEADER*>(pDib);
	*bi = {};
	bi->biSize = sizeof(BITMAPINFOHEADER);
	bi->biWidth = w;
	bi->biHeight = h;                     // 正 = 自下而上
	bi->biPlanes = 1;
	bi->biBitCount = 24;
	bi->biCompression = BI_RGB;
	bi->biSizeImage = dibImgBytes;
	auto dibPixels = pDib + sizeof(BITMAPINFOHEADER);
	for (int row = 0; row < h; row++) {
		auto src = data + (size_t)row * rowBytes;                 //入参是 top-down
		auto dst = dibPixels + (size_t)(h - 1 - row) * dibRowBytes;
		for (int col = 0; col < w; col++) {
			dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];     //BGRA -> BGR
			src += 4;
			dst += 3;
		}
	}
	GlobalUnlock(hDib);

	// ---------- 4) 写入剪切板 ----------
	if (!owner || !IsWindow(owner) || !OpenClipboard(owner)) {
		GlobalFree(hDib);
		GlobalFree(hDibV5);
		GlobalFree(hPng);
		return false;
	}
	if (!EmptyClipboard()) {
		GlobalFree(hDib); GlobalFree(hDibV5); GlobalFree(hPng);
		CloseClipboard();
		return false;
	}
	bool copied = false;
	// SetClipboardData 成功后 HGLOBAL 归剪切板所有，不能再 GlobalFree；失败了才要自己释放
	if (!SetClipboardData(CF_DIBV5, hDibV5)) GlobalFree(hDibV5);
	else copied = true;
	if (!SetClipboardData(CF_DIB, hDib)) GlobalFree(hDib);
	else copied = true;
	UINT cfPng = RegisterClipboardFormatW(L"PNG");
	if (cfPng == 0 || !SetClipboardData(cfPng, hPng)) {
		GlobalFree(hPng);
	}
	else copied = true;
	CloseClipboard();
	return copied;
}

bool ImageExport::saveToFile(const std::wstring& path, const int w, const int h, BYTE* data)
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
	const bool encoded = encodePng(stream.Get(), w, h, data);
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
    if (filePath.empty() || !owner || !IsWindow(owner)) return false;
    const auto attributes = GetFileAttributesW(filePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const auto totalSize = sizeof(DROPFILES) + (filePath.length() + 2) * sizeof(wchar_t);
    auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSize);
    if (!memory) return false;
    auto drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!drop) { GlobalFree(memory); return false; }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto destination = reinterpret_cast<wchar_t*>(drop + 1);
    wcscpy_s(destination, filePath.length() + 1, filePath.c_str());
    GlobalUnlock(memory);
    // Prepare the data before taking ownership, preserving the old clipboard
    // when allocation/opening fails. The final list has two terminating NULs.
    if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
    const bool result = EmptyClipboard() && SetClipboardData(CF_HDROP, memory);
    if (!result) GlobalFree(memory);
    CloseClipboard();
    return result;
}
