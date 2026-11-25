# Linux 录制编码接缝

本页记录 P09 已实现的软件编码器与实际验证边界。它不代表 Linux 屏幕录制、设备音频和发行验收已经全部完成。实时源授权与 PipeWire 生命周期归共享 `LiveCaptureSource` 所有；编码器不再授权、不打开设备、不推断截图与后续录屏的坐标关系。Windows 继续使用原有 Media Foundation 录制路径。

## 成熟实现与依赖

[`Mp4Encoder`](../src/services/media/Mp4Encoder.h) 直接链接锁定的 FFmpeg 9.0.2 和 libx264。复用官方 `doc/examples/mux.c` 的编码器送入、取包、交错封装和收尾流程，以及 `avio_read_callback.c` 的自定义 AVIO 所有权方式。示例作者与完整 MIT 许可保留在实现开头，逐文件 commit、blob 和用途见 [`mp4-encoder-upstream.json`](../third_party/ffmpeg-examples/mp4-encoder-upstream.json)。实际还阅读了固定 `libx264.c`、`aacenc.c`、`mux.c`、`movenc.c` 和 `aviobuf.c`，没有依据 README 自行实现编码格式。

此软件链沿用 [software-codecs.md](software-codecs.md) 的构建输入与 GPL-3.0-or-later 配置，没有新增外部程序或可浮动的编码库。需要 `libavformat`、`libavcodec`、`libavutil`、`libswscale` 和 `libswresample`。Windows 的 inspection-only FFmpeg 构建没有 libx264；不能把这个编码器链接进去而称已保留原生录制后端。

## 视频、音频与文件合同

一个录制 worker 持有编码器和已经打开、经过身份核对的空普通文件描述符。写入、64 位绝对寻址、flush 和 close 都通过该描述符完成，不再按路径重新打开文件。路径被改名或被其他文件替换后，编码器仍只写原对象；归档服务在提交时继续核对原身份。AVIO 错误、尾包、trailer、系统 flush 和 close 任一失败均保留首错，成功清理不会把失败改成成功。首帧之前不初始化编码流，没有实际输入的正常取消保留零字节文件。

视频输入为已完成 crop/旋转的 opaque 8 位 BGRA 或 RGBA。支持有界的 padded/负 stride，调用返回后不再持有来源 lease。HDR、未支持的 gamut、有限范围 RGB、半透明和不匹配的帧大小会显式失败。swscale 实际执行 full-range RGB 到 BT.709 矩阵、limited-range YUV420 转换；保留明确的 sRGB/BT.709 transfer，未标注的桌面 RGB 沿用现有未管理 sRGB 合同，不宣称完成 HDR tone mapping 或色域转换。第一帧 PTS 必须为零，随后 PTS 严格递增且不与上一帧显式 duration 重叠；时基固定为 100 ns。

libx264 当前使用 `veryfast`、CRF 18、2 个线程、10 帧 lookahead、最多 2 个 B 帧和两秒 GOP。帧对象通过 `av_frame_make_writable` 隔离编码器可能保留的引用，设置 `AV_CODEC_FLAG_FRAME_DURATION` 并传入真实 duration。停止时真正 drain 到 EOF 后写 MP4 trailer。这些参数有实际短媒体正确性证据，尚无双平台画质、能耗、4K 或长录制性能达标结论。

可选音频是已经完成源时钟对齐、混音和设备重采样的有限 interleaved float PCM，支持 44.1/48 kHz、单声道或双声道。输入以从零开始的连续 sample index 标识；缺样本、重复、非有限数或选择音频却没有实际样本都会失败，不自动造静音。swresample 此处只做同采样率 float packed 到 planar 格式转换，native AAC 以 192 kbit/s 编码；最后不足一个 AAC frame 的真实输入由固定编码器的 `SMALL_LAST_FRAME` 能力处理。设备采集、loopback、麦克风、同时混音和真实 A/V clock 校正属于来源层，不能由这项 codec 测试代替。

像素由 `PixelBuffer` 和 `FrameLease` 检查溢出与平面范围；默认单帧预算 256 MiB。PCM 调用和 FIFO 增长受默认 16 MiB 预算约束，分 codec frame 处理；文件 extent 上限 32 GiB，时间轴上限两小时。mux interleave delta 为一秒，避免缺少伴随流时无限等包。该策略不是对 FFmpeg/x264 内部分配的完整 RSS 证明；最大尺寸、长时间及资源释放性能仍须单独验证。

## 实际发现并修复的 AAC 尾样本问题

实测 31 帧 30 fps 加 45,570 个双声道采样点时，最初完整解码只能得到 45,555 个采样点。固定 `movenc.c` 用 100 ns 视频时基和 44.1 kHz 音频时基自动选择 movie clock 时，最小公倍数超过 `INT_MAX`，回退至 1 kHz；AAC priming edit 的取整随后裁掉了 15 个真实尾样本。

修复直接使用该版本官方 `movie_timescale` 选项：有音频时取实际采样率，无音频时取 10 MHz。各 track 保留自身原时基，未修改编码器或 MP4 语义。相同真实测试在提交前和持久归档后均完整解码得到 **45,570/45,570**；新增 48 kHz 得到 **49,600/49,600**，单帧单声道得到 **1,470/1,470**。断言保持精确数量，没有用 AAC padding 容差掩盖缺失。

## 已执行验证与尚未覆盖项

2026-11-25 在本地恢复的固定 Qt 6.12.1 / FFmpeg 9.0.2 SDK 上，以 C++20 和 `-Wall -Wextra -Werror -Wpedantic` 实际编译、链接并运行 [`mp4_encoder_tests.cpp`](../tests/qt/mp4_encoder_tests.cpp)：

- 1 帧、31 帧视频；包括 B 帧重排、尾包、每帧图像内容、PTS、duration 和严格全轨检查，持久归档后重新解码。
- 44.1/48 kHz 双声道和 44.1 kHz 单声道实际 AAC 编码，精确解码样本数及非零声道能量。
- 单槽来源池立即复用、BGRA/RGBA padded/负 stride、实际图像方向。
- 零帧取消、重复 PTS、短像素内存、PCM sequence/NaN 错误和已选音轨缺失。
- 真只读文件描述符写失败、10 字节输出预算、编码中改名和同路径外部替换保护。

同一组测试用本项目 C++ 代码的 ASan/UBSan 构建实际通过。预编译的 Qt、FFmpeg、x264 和 Rust 库未被这次重新插桩；运行环境不支持 LeakSanitizer，因此 `detect_leaks=0`，不记录为泄漏检测通过。测试使用已知实际像素/PCM 输入并运行真实 codec，**没有**代替物理显示器、Portal 授权、屏幕/音频设备采集、混合 DPI、暂停恢复、长时间 A/V 同步、安装升级和发布门槛。
