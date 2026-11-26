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

## 同次授权的录制来源与唯一编排

应用只使用 [`RecordingController`](../src/app/RecordingController.cpp) → [`RecordingSession`](../src/recording/RecordingSession.cpp) → `GifRecording` / `SoftwareMp4Recording` 这一套来源与停止编排。纯编码服务仍为 `GifEncoder` / `Mp4Encoder`。`ScreenCastSource` 是共享 `LiveCaptureSource` 的一次性 `FrameSource` facade，没有第二个 Portal 授权 owner 或 PipeWire producer。

授权预览选区不会直接开始录制。控制和选择界面隐藏并确认实际 QWindow 已 unexpose、外部停止入口仍可用后，刷新同次授权的数据连接。新的真实首帧必须保留所选 source/session、格式、crop、方向与尺寸；随后冻结新 generation，在借用信号返回后才把 reader 交给 worker。此前 Screenshot 的 URI 或任意图片矩形不能代替这个合同。

`RecordingSession::start(request, backend)` 只为这一任务持有一次性 provider。拒绝 admission 或尚未调用 backend 的 staging 失败也必须释放来源，不能在空闲 session 留住首帧。worker 在结束前通过 `stopAndWait()` 请求并等待独立 producer 实际退出；GUI 另行等待 Portal owner 终态，原生等待不依赖 GUI 回调。

输入只在真实 Streaming 的 `Idle` 时允许重复静止图像。`Paused`、来源结束、geometry / generation / crop / transform 或 timeline 改变都明确终止并保留已有完整媒体，不能拿旧 lease 冒充直播。单一 `FrameNormalizer` 为预览、滚动和录制执行 crop 后的八种旋转 / 镜像；已有 `FramePixels` 接口只作兼容包装，不维护第二套逐像素转换。

## 原生 X11 与音频

[`RecordingBackend`](../src/platform/linux/RecordingBackend.cpp) 在 media worker 创建唯一 `X11Capture(stop)`，复用真实 XCB、MIT-SHM / GetImage、native monitor identity 和光标混合。布局每帧与最初 native bounds/monitors 比较；Wayland / XWayland 不能当作原生完整桌面。借用 BGRA 在一次回调内复制到两个 slot 的受限 `FramePool`；消费者保留上一帧的 duration，下一帧仍来自独立存储。既有 XCB 连接可通过自己拥有的 socket 响应取消，初始 `xcb_connect` setup 尚未暴露 socket 的边界保持明确。

GUI 发布一个不可变控制窗口快照。每次读回使用 X11 短 server grab 原子检查实际 visibility、geometry、SHAPE 和 owned-window 身份，然后读取真实区域；前后快照不同即拒绝该帧。不采用每帧让 GUI 隐藏、等待固定毫秒再恢复的握手，worker 不等待 GUI。Windows 的同一合同使用实际 capture affinity 或已验证的区域外物理边框。没有安全位置时必须先有真实独立停止入口，才能把控制整个采集期间隐藏。

[`PipeWireAudio`](../src/platform/linux/pipewire/PipeWireAudio.cpp) 延续项目原有 `AudioSource::prepare/read`、双来源 native stream、PCM ring 和 sum-and-limit mixer。系统声为 sink monitor，麦克风为默认输入源；各自请求真实 stereo 48 kHz float PCM，不使用 ScreenCast 专用 remote FD。全部已选来源必须成功，不能省略缺失的输入。

每个 packet 的原生 `time` 通过共同校准的 clock anchor 映射，精确样本索引计算 `floor(offset * sampleRate) + ordinal`，避免纳秒往返舍入重复样本。只分配所选来源的 ring，所有 ring 合计受 caller byte budget 约束；检查声道位置、chunk、时间范围和 finite PCM。只有 native GAP/EMPTY buffer 表达静音，真实空缺区间失败。实际物理设备、热插拔、长期 drift 和绝对 A/V 延迟仍需独立验收；纯 codec 和私有信号图不能代替。

## 私有原生音频图验证入口

[`test-pipewire-audio.py`](../scripts/test-pipewire-audio.py) 使用锁定构建 prefix 内的 PipeWire daemon、`pw-dump` 和 `pw-link`，在自己创建的 0700 runtime 中运行，没有系统服务修改、物理设备或用户会话连接。脚本核对实际 `build-info.json` 的版本、commit、patches 和 Meson 选项，记录实际二进制 SHA256。所有子进程由明确的 `Popen` 所有权管理，设独立 watchdog 与父进程退出保护；只终止自己启动的 daemon/信号源。

两个信号程序直接改编固定官方 `audio-src.c`：44.1 kHz 的 440 Hz 信号进入真实 null sink 的 monitor，48 kHz 的 880 Hz 信号作为独立输入源。临时配置和显式端口连接只负责该测试图的策略，不替换生产 `PipeWireAudio` 回调、SPA 转换、buffer 或 PCM。来源进程 PID、真实 FL/FR port 和生产 `stream.capture.sink` 选择属性都必须符合预期。源码与许可证定位见 `upstream.json` 中的 `nativeGraphValidation`。

`pipewire_audio_graph_probe` 使用真实生产 `makePipeWireAudio`，检查精确连续三秒 48 kHz 双声道样本、两个已知频率的幅度、单源隔离、双源混音与限幅，随后以独立 case 实际终止该图的 daemon 或输入源，检查有界失败和描述符归还。频率测试可以发现一部分重采样/时间轴错误，但不是长期 A/V drift 或绝对设备延迟校准。测试信号的存在始终记录在证据中，绝不冒称实体麦克风/系统扬声器录制。

构建 `tests/platform/pipewire_audio_signal.cpp` 时只需锁定 `PkgConfig::PIPEWIRE` 与线程库；`tests/platform/pipewire_audio_graph_probe.cpp` 还需生产 `PipeWireAudio.cpp`，二者不依赖 Qt。实际运行示例：

```sh
python scripts/test-pipewire-audio.py \
  path/to/pipewire_audio_graph_probe path/to/pipewire_audio_signal \
  --pipewire-prefix .deps/pipewire-validation/install \
  --pipewire-build-info .deps/pipewire-validation/build-info.json \
  --artifacts artifacts/pipewire-audio/debug
```

每个 case 保留真实 registry、port 链接、daemon/信号/采集日志和 JSON 结果；失败不会中止其余独立 case 的诊断，整项工作仍以非零退出报告未通过。2026-11-25 两个 C++ probe 已用固定 251 SDK 实际严格编译/链接，Python 入口通过语法与参数检查。本地启动自己拥有的 PipeWire server 时，`protocol-native` 创建端点实际返回 `Operation not permitted`，进程退出 255 且未建立 socket；本地没有正向图运行结果。实际 CI 结果见后文；音频设备与发布门槛继续未验收。

### 原生图归属核对的实际 CI 修正

首次完整应用 Linux CI 的六个图 case 都在连接前归属检查处失败，未执行到 PCM 验收。保留的真实 registry 显示：信号 Node 通过 `client.id` 指向 Client，PID 位于该 Client 的 `pipewire.sec.pid` / `application.process.id`，并不保证重复出现在 Node 上。wrapper 现在要求唯一对应 Client，核对由原生协议服务端确认的 `pipewire.sec.pid` 与自己启动的精确 `Popen.pid` 一致；Client 若同时给出 application PID，也必须一致。歧义、缺失 Client、错误 PID 都拒绝，没有以节点名称代替进程归属。

固定 PipeWire 的 `keys.h` 和 `impl-client.c` 确认安全 PID 是服务端来源且不可由 Client 更新覆盖，源码定位已加入 `pipewire-audio/upstream.json`。本地用六份实际 CI registry 离线核对，通过 12 个真实 Node→Client 关系，以及错误 PID、缺失/重复 Client、伪造应用 PID 等 48 个拒绝断言；这些检查只验证 registry 结构和所有权逻辑。

### 实际 PCM 时序失败与有界修正

提交 `1f516bf5799551af1776839f56a21a16e652a4c1` 的 Linux 完整应用 CI 实际通过 microphone、mixed、clipped-mix、daemon-loss、source-loss 五个私有图 case。前三项分别得到精确 144,000 个双声道样本；已选择的 440/880 Hz 分量、未选来源隔离和限幅断言通过。两个故障 case 在 24,576 个真实样本后分别终止自己的 daemon 或输入源，生产适配器返回原生 `EPIPE`，五项描述符差值均为零。system case 在第一段读取中报“requested audio interval has no captured samples”；整套六项仍未通过。原始证据 ZIP SHA256 为 `317918aee672c1f3f9f0833c8f75771b80755644a67aba1267dddd4cbf3a36b2`。

固定 `stream.h` 与 `stream.c` 区分了最新 `pw_time.now` 报告和每个 `pw_buffer.time`：后者是该 buffer 自己进入流时的原生 graph cycle。继承代码重新采样两次时钟来计算每包 offset，并用最新报告给旧队列 buffer 定时；调用间调度差异可被误当作 PCM 空洞。修正只在同一 worker 内校准一次原生单调时钟到录制 steady clock 的对应关系，两个来源共用这个锚点；四次有界括号读取得到最小测量区间，精度不足半个采样点即明确失败。每包随后使用其实际 `buffer.time`，保留真实 latency 和 resampler buffered 数值。读取位置直接保留整数样本网格：对请求起点相对当前包的实际 clock 偏移只做一次向下取整，再加整数样本序号；跨包、覆盖范围和真实缺口使用同一规则。不再把每个 `sample/48000` 先截断到纳秒再反算样本，避免 `1 → 20833 ns → 0` 重复读取前一个 PCM。负偏移仍保留为缺口；既不补静音，也不将 packet 起点强行拼接到上一包。大时钟差先拆整秒和不足一秒余数，再进行有检查的换算。

此修正不补造静音、不修改采样率或混音，也不把缺口强行贴到前包尾部。真实缺样本继续失败，并记录缺口纳秒数、请求样本位置、原生 cycle、report ticks、delay 与 buffered 值，便于下一次真实图裁决。固定 SDK 已实际严格编译这一修正并通过三种来源组合的真实缺失端点、取消、预算和 FD 清理测试；**修改后的六种正向图尚待 CI，实体设备、长期漂移和硬件延迟校准均未通过验收**。

本地时间换算回归另行验证整秒 48,000 个互不重复索引、实际纳秒表示的 4,096 帧包边界、正负偏移及原生时钟极值；这是数值单元验证，与真实 PipeWire 图分开记录。修正后的三个原生目标严格编译通过，缺失端点、取消及 FD 释放检查通过；六种真实私有音频图仍待下一 CI，不据此宣称实体设备或长期同步已经通过。
