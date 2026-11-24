#include "pch.h"
#include "services/ImageRecognition.h"
#include "services/ImageExport.h"
#include "services/Setting.h"
#include "core/PixelBuffer.h"
#include "quirc/quirc.h"
#include <format>
#include <memory>

namespace {
	// quirc 交出来的是裸字节流：BYTE 类型的二维码现实中基本都是 UTF-8（微信、支付宝
	// 生成的都是），Kanji 类型按 ISO 18004 规定是 Shift-JIS。所以先按 UTF-8 严格解，
	// 解不通再退回对应的本地代码页，避免把中文变成一堆问号
	std::wstring qrPayloadToWStr(const uint8_t* payload, const int len, const int dataType)
	{
		if (len <= 0) return L"";
		auto convert = [payload, len](UINT codePage, DWORD flags) {
			auto str = (const char*)payload;
			auto count = MultiByteToWideChar(codePage, flags, str, len, nullptr, 0);
			if (count <= 0) return std::wstring();
			std::wstring result(count, 0);
			MultiByteToWideChar(codePage, flags, str, len, result.data(), count);
			return result;
		};
		auto result = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
		if (!result.empty()) return result;
		return convert(dataType == QUIRC_DATA_TYPE_KANJI ? 932 : CP_ACP, 0);
	}

	// 插件的查找顺序：先本 exe 同目录（绿色包一起解压的情况），
	// 再 %appdata%\ScreenCapture\plugin（后来单独下载的情况）
	std::filesystem::path findImageReader()
	{
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileName(nullptr, buffer, MAX_PATH);
		auto path = std::filesystem::path{ buffer }.parent_path().append(L"ImageReader.exe");
		if (std::filesystem::exists(path)) return path;
		path = Setting::get()->getDataPath().append(L"plugin").append(L"ImageReader.exe");
		if (std::filesystem::exists(path)) return path;
		return {};
	}
}

bool ImageRecognition::openWithImageReader(const int w, const int h, BYTE* data)
{
	auto exePath = findImageReader();
	if (exePath.empty()) {
		// 插件没装，直接把用户带到下载页，不再多弹一层提示
		ShellExecute(nullptr, L"open", L"https://github.com/xland/ImageReader/releases", nullptr, nullptr, SW_SHOWNORMAL);
		return false;
	}
	auto imgPath = Setting::get()->getDataPath().append(L"ocr_" + ImageExport::createFileName(L"png")).wstring();
	if (!ImageExport::saveToFile(imgPath, w, h, data)) return false;
	// --del-image=true：插件读完自己把缓存图删掉，免得在数据目录里越攒越多
	auto cmd = std::format(L"\"{}\" --image-path=\"{}\" --del-image=true", exePath.wstring(), imgPath);
	// 工作目录设成插件所在目录，它才找得到自己身边的依赖
	auto workDir = exePath.parent_path().wstring();
	STARTUPINFO si{ .cb = sizeof(STARTUPINFO) };
	PROCESS_INFORMATION pi{};
	if (!CreateProcess(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.data(), &si, &pi)) {
		std::error_code ec;
		std::filesystem::remove(imgPath, ec); //插件没起来，别留下垃圾文件
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

std::wstring ImageRecognition::decodeQrCode(const int w, const int h, BYTE* data)
{
	std::wstring result;
	if (!ScreenCapture::Core::pixelLayout(w, h, 4) || !data) return result;
	// quirc_new 和 quirc_resize 是这个库里唯一会申请内存的两个函数，选区大的时候
	// 那块灰度缓冲不小，所以下面每条返回路径都得走到 quirc_destroy
	std::unique_ptr<quirc, decltype(&quirc_destroy)> qr{ quirc_new(), quirc_destroy };
	if (!qr) return result;
	if (quirc_resize(qr.get(), w, h) < 0) {
		return result;
	}
	// quirc_begin 给的就是它内部那块缓冲，一个像素一字节，直接把灰度写进去
	int bufW{ 0 }, bufH{ 0 };
	auto buffer = quirc_begin(qr.get(), &bufW, &bufH);
	const size_t count = (size_t)w * h;
	for (size_t i = 0; i < count; i++) {
		auto px = data + i * 4; //入参是 BGRA
		buffer[i] = (uint8_t)((px[2] * 77 + px[1] * 150 + px[0] * 29) >> 8);
	}
	quirc_end(qr.get());
	auto codeCount = quirc_count(qr.get());
	for (int i = 0; i < codeCount; i++) {
		quirc_code code{};
		quirc_data qrData{};
		quirc_extract(qr.get(), i, &code);
		auto err = quirc_decode(&code, &qrData);
		if (err == QUIRC_ERROR_DATA_ECC) {
			// 可能是镜像的码（ISO 18004:2015 允许），翻过来再试一次
			quirc_flip(&code);
			err = quirc_decode(&code, &qrData);
		}
		if (err != QUIRC_SUCCESS) continue;
		auto text = qrPayloadToWStr(qrData.payload, qrData.payload_len, qrData.data_type);
		if (text.empty()) continue;
		if (!result.empty()) result += L"\n";
		result += text;
	}
	return result;
}
