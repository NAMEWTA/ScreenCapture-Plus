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

## 同次授权的录制来源

[`LiveRecording`](../src/platform/linux/LiveRecording.h) 消费共享 `LiveCaptureSource` 已经交出的单个 `LiveSourceConsumer`。授权、Portal parent/session、PipeWire FD 和预览生命周期仍只有共享来源这一处所有者；录制器不创建第二次授权，也不复用早先 Screenshot 的选区坐标。`BackendRequest` 明确区分 `DesktopPixels` 和 `AuthorizedSourcePixels`，后者必须匹配已确认选区、`sourceId` 和 `generation`。Windows 原生后端显式拒绝来源像素域，避免在没有映射证据时把它当成桌面物理像素。

共用 `RecordingSession::start(request, provider)` 只为本次任务捕获一个提供者，不改变构造时的默认后端。busy、关闭或错误线程导致 token 为零时，提供者不会执行；宿主必须取消并归还尚未消费的 claim。非零 token 也可能在建立 staging 文件时失败而尚未调用提供者，因此宿主在每个终态都必须完成来源停止和 `consumerFinished` 握手，不能只在成功归档时处理。一个提供者副本集合只能消费同一个 claim 一次。

来源首帧是同次授权产生的真实 `FrameLease`。录制 worker 优先取较新的排队帧；静止桌面没有新 damage 时才使用交接的首帧。开始时钟在实际来源与所选音频准备完成后建立，不把用户框选期间的旧 source PTS 差值计算为录制时长。第一帧的初始 discontinuity 是时钟基线；开始后的身份、尺寸、格式、crop、方向或 continuity 改变会终止该录制并保留首错。

像素选择、stride 和八种旋转/镜像只调用共享 `FrameNormalizer`。MP4 保留当前区域内 4×2 对齐规则，prepared 返回最终实际区域；GIF 保留选区尺寸。只有来源报告真实 `STREAMING` 才允许按录制时钟重复静止图像。`PAUSED` 时停止提交新视频帧，保留间隙；不因来源仍有一个旧 lease 就声称仍在实时采集。MP4 使用前一帧暂存到下一个实际时间点的方式确定 duration，停止时送出最后一个真实间隔并 drain 编码器，保持现有 B 帧与 AAC 尾样本修复。

停止时先请求原生来源退出，释放来源/归一化 lease，消费到来源队列的明确终态，然后结束音频 worker 和编码器。此等待发生在录制 worker，原生生产者独立发布终态，不需要 GUI 执行被等待的回调。GUI 所有者另行等待生产者实际返回、预览返回和录制消费者结束后才释放 Portal。来源失败终态与编码失败分别保留，迟到的正常停止不会覆盖首错。

## 音频实现复用与未完成的设备验收

音频层优先复用同一项目 PR [#7](https://github.com/NAMEWTA/huhucapture/pull/7) 的固定 commit `251bc0071278c6b711c840bf5188d16870bc7349`，已完整阅读 `AudioSource`、`PipeWireAudio`、`SoftwareMp4Recording` 和对应测试。原实现以锁定 PipeWire 1.6.11 的 `audio-capture.c` 为依据；精确 blob、作者和许可证见 [`pipewire-audio/upstream.json`](../third_party/pipewire-audio/upstream.json)。移植复用了双来源采集、PCM 队列、prepare/read 接口、sum-and-limit 混音和 worker 清理，没有复制另一套 GIF/MP4 编码器、文件输出或 Portal owner。

系统声音使用 PipeWire sink monitor，麦克风使用默认输入设备，各自通过实际原生 stream 请求 stereo 48 kHz float PCM。它们不使用仅限 ScreenCast 的远端 FD。所有被用户选择的来源都必须在期限内产生真实 PCM；一个来源失败不能悄悄省略该音轨。实际 GAP/EMPTY buffer 可表达静音，缺失的 PCM 时间区间不能用补造静音当作采集成功。适配层按 caller 的总字节预算为被选择的来源分配 ring，检查声道位置、chunk 范围、原生错误和非有限样本；编码器只接收有界连续样本序列。

当前继承的 PipeWire graph clock 映射仍须用实际音频图、设备和长录制核对；严格缺口拒绝可能暴露设备时钟、量化与调度差异。实际 monitor/microphone、双源混音、热插拔、权限变化、48 kHz 转换质量和长时间 A/V drift **未由本批测试验收**。不能仅凭缺失端点测试成功就把系统音频、麦克风和 P09 标为完成。

新增 [`live_recording_tests.cpp`](../tests/qt/live_recording_tests.cpp) 已用固定 SDK 严格编译、实际运行并通过项目代码的 ASan/UBSan：有界帧池/队列、静止首帧、变化帧、共享归一化、真实 GIF/H.264 编码和严格解码、来源权限/方向终态、claim 一次性、per-request Qt 会话及持久归档。输入明确是 codec/worker fixture，不冒充屏幕设备。新增 [`pipewire_audio_tests.cpp`](../tests/platform/pipewire_audio_tests.cpp) 实际检查三种来源组合在不存在的原生端点下失败、取消、线程回收、文件描述符归还及过小总 PCM 预算拒绝；它没有产生设备音频。两者的 sanitizer 限制与上文一致。

## X11 原生录制接入

[`makeX11RecordingBackend()`](../src/platform/linux/X11Recording.h) 在任务被接纳后才创建 worker 私有的现有 `X11Capture`，共用已有 MIT-SHM/readback、像素转换、真实光标混合和 stop-token socket 清理。不重新实现 XCB 采集，不让 GUI 每帧构造 `QImage`。factory 本身不打开显示器，也不持有媒体；应用仍使用一个 `RecordingSession`，Wayland 来源通过注入的 `LiveBackendFactory` 进入同一条消费管线。

原生请求必须是 `DesktopPixels` 且没有 Portal 来源标识。X11 adapter 读取当前 root/monitor 布局，并在每次采集前核对与首次布局一致；根窗口尺寸、显示器身份、位置或旋转改变即停止。应用还必须在开始前核对截图选区的 desktop generation，并在失效通知时停止录制。Wayland 环境和实际 XWAYLAND extension 均显式拒绝，不能据 XWayland 可读像素声称捕获了 compositor 的完整桌面。

生产者仅将借用的真实 BGRA 行复制一次到四槽 `FramePool`，总来源像素预算为 `min(4 × maximumFrameBytes, 512 MiB)`，队列容量二。暂存首帧也计入同一池。随后交给已有 `LiveRecording`、`FrameNormalizer`、GIF/MP4 编码与归档服务。内部 `(0, 0)` 只表示实际已读的选区图像；prepared/result 再根据这次实际读回的原生 region 映射回桌面坐标并标记 `CaptureSource::X11`，不推断 DPI。producer 先销毁 XCB/SHM 资源再发布队列终态，录制 worker 在返回前实际 join；GUI 不参与被等待的原生工作。

2026-11-25 使用固定 251 SDK 实际严格编译了 provider 和 [`x11_recording_failure_tests.cpp`](../tests/qt/x11_recording_failure_tests.cpp)，并运行通过：不存在的真实 XCB display、预取消、来源坐标域/标识拒绝、Wayland 环境拒绝、空 staging 身份保护、重复六次线程与 FD 清理。初始 `xcb_connect` 的认证/setup 尚未暴露可取消 socket，这是既有 X11 adapter 的明确边界；建立后的阻塞 I/O 可通过自己拥有的 socket 中断，不能把该故障测试称为任意卡住 X server 的有界退出证明。

[`x11_recording_probe.cpp`](../tests/qt/x11_recording_probe.cpp) 提供实际隔离 X server 的验收入口：真实窗口绘制 → 现有 X11 readback → 共用 GIF/H.264 → 全文件检查与持久归档，并从服务器读取原生 client geometry、核对录制后图像象限和 GUI heartbeat。它拒绝 offscreen/Wayland 运行；本地环境无法启动原生显示服务，**该正向 probe 待实际 CI 运行**。它也不代替物理双屏/混合 DPI、设备音频、热插拔和 UI 控件排除验收。X11 没有 Windows 的 display-affinity 排除机制；选区内的工具条/outline 会成为真实桌面内容，宿主必须另行处理控件的实际位置或可见性。

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

每个 case 保留真实 registry、port 链接、daemon/信号/采集日志和 JSON 结果；失败不会中止其余独立 case 的诊断，整项工作仍以非零退出报告未通过。2026-11-25 两个 C++ probe 已用固定 251 SDK 实际严格编译/链接，Python 入口通过语法与参数检查。本地启动自己拥有的 PipeWire server 时，`protocol-native` 创建端点实际返回 `Operation not permitted`，进程退出 255 且未建立 socket；这不是正向测试通过。**六项实际图运行等待 CI，音频设备与发布门槛继续未验收。**
