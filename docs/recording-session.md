# 共享录制会话、原生来源与输出

本实现对应 [Qt 迁移](qt-migration.md) 的 P09，并复用 P04 / P05 的原生采集边界。共享 Qt 界面、编码组件和真实文件测试已经接线；这不等于 P09 的长时音画同步或 P12 的双平台桌面验收完成。正式 Windows 产品在 P12 通过前继续保留。

## 一次录制由谁拥有

[`RecordingController`](../src/app/RecordingController.cpp) 为每次录制创建并持有一个 [`RecordingSession`](../src/recording/RecordingSession.cpp)，把本次来源 backend 一次性交给它；空闲控制器不保留消费 lease。[`RecordingWindow`](../src/ui/qt/RecordingWindow.cpp) 只提交开始、保存、复制、丢弃等意图，并显示会话的真实阶段。隐藏或关闭工具窗口不会析构仍在工作的录制器。应用通过托盘、主快捷键或桌面完成入口提交明确的停止意图；没有捕获风险的可见工具栏可以恢复和激活，因捕获而隐藏的工具栏在来源停止前保持隐藏，不能为了激活改变保存或丢弃意图。

会话在 GUI 线程受理请求，并在一个 QtConcurrent worker 中依次执行：创建受控暂存、取得同一文件对象的写句柄、准备真实采集与编码器、采集、停止并释放原生来源、排空编码器、完成容器、关闭写句柄、完整解码校验和持久归档。worker 不等待 GUI 回调，正常停止流程不在 GUI 同步 join 录制线程。正常应用退出等待 `shutdownFinished`；Portal 来源也必须确认自己的 producer 已返回，不能仅把停止请求已经发送当成完成。

`Core::FrameSource::stopAndWait()` 是 worker 使用的停止屏障。PipeWire reader 先请求 producer 停止，再等其实际退出；Portal 会话所有者保持到 producer 完成以后。尚未开始消费的选择可以在 GUI 上放弃 reader，其析构只发出停止请求，不阻塞 GUI。编码器不会在仍可能访问来源资源的 producer 工作时销毁来源。

唯一的 [`LiveFrameReader`](../src/services/capture/LiveFrameReader.cpp) 同时供录制和滚动使用，保留真实 bounded frame queue 与 producer 完成 future。首帧携带格式切换的 discontinuity 是来源初始化合同；建立本次格式后，后续 discontinuity、格式/代次变更或 Paused 都是失败。只有调用方或同一来源已经请求停止时，Stopped/Cancelled terminal 才转换为正常结束；无对应停止请求的来源关闭不能伪装成成功录制。`Idle` 只在真实 Streaming 下返回，允许静态画面按时钟持续录制。

会话令牌递增，旧回调不能作用于新的录制。停止意图采用第一次成功的原子选择：保存之后的重复丢弃不能反转结果，自动时限也不能与停止按钮重复发布。准备、采集、编码、finish、写入和关闭的第一个错误会保留；后续 finalize 成功不能把前面的失败洗掉。

## 平台路径与保留的行为

| 路径 | 实际来源和编码 | 当前行为 |
| --- | --- | --- |
| Windows MP4 | 保留的 DXGI Desktop Duplication / GDI、WASAPI、Media Foundation | 优先准备 HEVC，不支持时在开始前改试 H.264；保留 MP3 音轨、44.1 kHz 双声道、192 kbps |
| Linux X11 MP4 | 独立 XCB 连接的真实桌面像素、PipeWire 音源、固定 FFmpeg / libx264 / AAC | H.264 软件编码、48 kHz 双声道 AAC；请求的音源缺失即失败 |
| Linux Wayland MP4 | 新 ScreenCast 授权对应的 PipeWire 帧、独立 PipeWire 音源、同一软件编码器 | 不把 Screenshot URI 或图片位置当作授权 node 或桌面坐标 |
| 双平台 GIF | 原生 Windows / 共享 `FrameSource` 来源、同一 `GifEncoder` 与聚合 Rust 静态库里的 gifski | 最大 16 fps / 6 分钟，quality 80、fast、无限循环，无音轨 |

MP4 默认 30 fps、最大 120 分钟；MP4 区域向内对齐到宽度 4 / 高度 2 的像素倍数，负坐标显示器使用同一几何规则，不把所选区域扩大到未选中的像素。GIF 保留原始选区尺寸。选择 GIF 会关闭系统声和麦克风，切回 MP4 不暗中重新打开音源。

Windows 原生 MP4 提取在 [`NativeMp4Recording`](../src/platform/windows/NativeMp4Recording.cpp)。它枚举真实 adapter / output，只有单个未旋转输出完整覆盖选区时使用对应 duplication；旋转、跨屏或不适用的 duplication 使用原有 GDI 像素来源，不把跨屏选区裁成一个显示器。每个录制 worker 建立自己的 COM / Media Foundation 生命周期和物理像素 DPI 范围。

Windows 录制由 CMake 装配，MP4 使用原生后端，GIF 使用 `NativeGifRecording` 和共享 `GifEncoder`。Qt 与 Rust 聚合库使用同一动态 CRT。历史 Windows 单 EXE 的录制入口保存在 [v0.0.6](https://github.com/NAMEWTA/huhucapture/blob/v0.0.6/src/recording/CapVideo.cpp)。

## 来源选择、像素和窗口位置

[`FrameSource`](../src/recording/FrameSource.cpp) 将真实轮询捕获转换为两个 slot 的有界 frame pool；借用的 GDI / XCB 缓冲仅在回调返回前读取。返回给编码器的 `FrameLease` 拥有有效存储，不依赖下一次原生捕获还保留旧字节。超预算、完整帧不足、借用 span 越界或所有 slot 被占用均明确失败，不生成替代画面。

授权来源先取得真实首帧用于选择区域。选择时保存 source/session identity、格式代次、完整 `FrameFormat`、crop、transform 和规范化尺寸。确认外部停止入口可用并隐藏选择器、控制窗口及本进程 tooltip 后，等待这些实际 QWindow 退出 exposure，再通过 `ScreenCastSource::refresh()` 刷新同一次授权的数据连接。新首帧必须保留原选区的 identity、格式、crop、transform 和尺寸；只有此时允许冻结新的格式代次并一次性交出 reader。`refreshed` 信号返回之后才启动消费者，避免 worker 移动仍被 GUI 借用的首帧。录制时每帧检查同一合同；即使新尺寸仍容得下旧矩形，代次或裁剪变更也会要求重新选择。`FramePixels` 是共享 `FrameNormalizer::copyNormalizedBgra` 的兼容边界，统一执行源 crop 后的八种旋转 / 反射，支持带 padding 或负 stride 的 BGRA / RGBA。未知布局和当前未支持的 HDR transfer 不伪装成普通 SDR。

Windows 工具窗口调用原有 `WDA_EXCLUDEFROMCAPTURE` 能力，并按真实 `RtlGetVersion` build >= 19041 限制使用、读回实际 affinity。旧 DWM 会接受数值却画黑矩形，因此不以函数返回成功代替系统版本能力判断。Linux 明确返回不支持单窗口排除。

没有 OS 排除能力时，原生桌面选区通过已经验证的逐显示器绑定，把工具栏约束到选区外的工作区。每个绑定检查当前 QScreen 名称、几何、工作区和各轴比例。worker 在每一帧读回前后检查同一不可变控制窗口快照；Windows 读回实际 affinity / 物理边框，X11 在短 server grab 内核验真实窗口、几何和 SHAPE，再读取像素。移动、缩放、原生 ID 更换或布局变化不能仅凭开始时曾经安全就继续被接受。快照缺失会关闭帧受理，不生成黑框代替真实排除。

没有安全位置时，仅在宿主已验证独立的停止入口可用后隐藏控制窗口。Wayland 的授权 source 没有可通用推导的全局坐标，始终使用隐藏与同授权刷新流程，不按桌面坐标猜测。隐藏后外部停止能力失效会停止工作并保留可恢复媒体；不会把它转换为用户的丢弃意图。刷新、source lease 和 native producer 完成是可验证的生命周期合同，不代表已证明每个 compositor 都在同一时刻完成画面呈现；Windows DWM、GNOME、Plasma 和实际 X11 捕获仍分别需要桌面像素证据。

## Windows 原生末帧和时间边界

[`VideoMp4`](../src/recording/VideoMp4.cpp) 使用相邻真实捕获时间确定连续中间样本区间，不用成功帧数除以帧率缩短慢采集的录制。每次捕获创建独立的 MF sample / buffer，像素交付后不再改写。视频路径额外保留一份尚未交给 sink writer 的样本；下一帧到来时提交前一份，因此增加的应用持有量有明确的一帧上限。已捕获的 pending 首帧不会被当成“尚无首帧”而触发超时，GIF / Framer 路径不经过这个队列。

离开采集循环时、等待音频线程退出之前，固定实际捕获终点 `E = captureEnd`，以它作为最后一个 pending 样本原本区间的终点。[纯时间规则](../src/core/RecordingTime.h) 在 100 ns 单位中求名义帧率边界 `B(k) = round(k * 10000000 / fps)`，选择首个不早于 `E` 的 `T`；恰好落在已舍入边界上的终点保持不动。只把仍未提交的末样本时长延长至 `T - timestamp`，随后提交一次并 finalize。原始区间不会被缩短，延长量小于相邻 CFR 边界之间的一帧；目标不会随着排空耗时或 sink statistics 反复后移，也不会在用户停止后重新捕获画面。音频的样本时钟、MP3 配置、暂停判定和录制时限的停止条件继续使用原路径，视频末尾只承担明确的帧粒度对齐。

成功计数仍只表示 `WriteSample` 已接受的输入；pending 不提前计数，失败的提交不在清理阶段重试。停止前的采集 / 音源错误先保存，末样本或 finalize 的后来结果不能覆盖它。可恢复的已有媒体仍走原来的失败保留路径。最终统计同时记录 captured、实际输入 duration 总和、captureEnd、原始 `E`、冻结的 `T`、最后成功输入区间和 MF 各阶段计数。成功路径要求每个捕获都提交一次，中间区间无缝衔接；输入总 duration 与最后输入终点都等于 `T`，而 `captureEnd == E`。

Windows 原生 probe 使用独立 FFmpeg 有理数换算检查：`E <= T`、前一个帧边界严格早于 `E`、最终输入终点等于 `T`，并完整解码验证连续呈现时间线恰好具有 `T` 对应的输出槽数。最后输出终点与 `T` 仅允许 100 ns 有理数换算的一 tick 误差；整帧缺失或多出整帧仍失败。输入受理数、编码 / 处理数、实际 packet 数和完整解码帧数按各自阶段核对，不把输入一帧必然等于转换后输出一帧作为前提。纯整数测试只能证明舍入 / 溢出规则，不能替代实际 Windows MF 排空与桌面录制证据。

## 软件编码和音画时间

[`SoftwareMp4Recording`](../src/recording/SoftwareMp4Recording.cpp) 只编排来源、实际时间和停止，编码与文件写入由唯一的 [`Mp4Encoder`](../src/services/media/Mp4Encoder.cpp) 承担，不调用外部录屏或编码命令。共享 Linux 软件配置为 libx264、YUV420P、veryfast、CRF 18、最多两个 B 帧；排空检查保留真实重排与 AAC 尾样本，不改变 Windows MF 编码器。RGB 转 YUV 使用明确的 BT.709 矩阵和视频范围，并保留可映射的源 primaries / transfer 标签。

视频时间来自单调时钟，帧 duration 由下一次实际捕获时刻决定，最后一帧补至实际停止时刻。慢帧不会通过 `frame_index / fps` 压缩录制时间。当前真实 codec 测试包含每帧约 90 ms 的受控慢来源，检查 450 ms 的录制仍保留约 450 ms 的容器时间，并且所有已提交帧连同最后一帧都能完整解码。

[`PipeWireAudio`](../src/platform/linux/pipewire/PipeWireAudio.cpp) 为系统声音 monitor 和默认麦克风分别创建真实 PipeWire input stream；两个都请求时必须都准备成功。它不复用 ScreenCast 的受限 video remote FD 作为音频授权，不用生成的静音替代缺少的设备。输入协商为 48 kHz、双声道交错 float，固定容量队列保存 PCM；开始消费后溢出、损坏 chunk、异常 timestamp 或设备断开都作为错误传播。

音频 stream 使用每个实际 `pw_buffer.time`，通过一次有界校准的共同 native-to-steady anchor、delay / buffered 与样本数映射到单调时间轴。精确样本网格计算 `floor(offset * 48000) + ordinal`，不把每个 ordinal 先舍入为纳秒再换回采样索引。混音读取相同时间区间，求和并限幅；只有 native GAP / EMPTY 数据能表达静音，缺失的 PCM 区间必须失败，不能以补零掩盖来源缺失。`FrameReadStatus::Idle` 只表示真实 Streaming 下的静止图像；`Paused` 明确终止采集并保留已有媒体，不能继续重复旧 lease。停止时有界地取得已经录制时间段的尾部音频，然后停止来源、排空音视频编码器并写入 trailer。原生设备时钟漂移、双来源混音、热插拔和长时同步仍需实际设备验证，受控 PCM 编码测试不覆盖这些能力。

## 文件、归档、保存和恢复

各编码器只接收 `FileHandle`，不按路径重新打开并截断。Windows [`FileHandleStream`](../src/platform/windows/FileHandleStream.cpp) 把已有身份检查后的文件对象通过 `DuplicateHandle` 包装为可寻址的 IStream，再交给正式 MF byte-stream bridge。Linux FFmpeg 使用写入 / seek 回调，GIF 使用 gifski 写入回调；实际写入、flush、finish 和 close 的错误不能被忽略。

会话在调用 backend 之前记录实际打开对象的身份，完成后核对暂存对象与字节长度；完整解码和持久发布之后再次核对发布路径，并返回包含 post-publication snapshot 的 `Media::Entry`。后续 [`MediaWorkspace::outputArchived`](../src/app/MediaWorkspace.cpp) 接收这个精确 entry，不能只按路径重新列举并授权一个替换文件。

保存和复制先形成一份完整、持久、可再次验证的归档，然后复用现有 Save As / 精确覆盖确认 / 剪贴板回执流程。取消 Save As 保留已完成的归档，输出不会再制造第二个归档。失败保留已有暂存或已发布路径，并明确告知位置；不完整文件不进入历史。显式丢弃只删除当前身份仍匹配的自有暂存。应用退出会请求停止并保留可恢复文件，不把退出转换为丢弃。

## Windows MP3 音轨的两端完整校验

旧 Windows MP4 的音频默认是 MP3，新的完整解码器不能只接受 Linux 新录制的 AAC。依赖锁在两个生产 FFmpeg profile 中同时启用 `mp3float` decoder 和 `mpegaudio` parser，构建脚本检查实际 `config_components.h`，不靠配置字符串猜测功能已编入。没有新增 MP3 encoder、网络协议或运行时命令依赖。

依据固定 FFmpeg commit `946fcce07b6dcd0331c8cc609192aeff5e1924f8` 的源文件：

| 文件 | Git blob | SHA-256 |
| --- | --- | --- |
| `libavcodec/mpegaudiodec_float.c` | `e713e9e251c181ff8756acf25dfe0bd1208fac27` | `50f02b8b43709291ca7efb8f68a21d52143461685d54c8d69957d3211bbd2113` |
| `libavcodec/mpegaudio_parser.c` | `8c33d36dfefa29a38c54a003fed85bd5968fa7b6` | `c3ebefdce56d8ace2b13af332b4c3b47dbd66f658c2e5f1285edb58d3da68e9f` |

[`legacy_mp3_recording_tests`](../tests/qt/legacy_mp3_recording_tests.cpp) 在 Windows 通过实际软件 MF H.264 / MP3 编码器，向正式文件句柄 adapter 写入 51 帧视频和非零 PCM，生成有效 MP4，再复制并破坏后段实际 MP3 容器 sample 的物理字节。定位器独立采用 `NOPARSE | NOFILLIN`，逐个核验 sample 范围和独立只读 `QFile` 中的对应字节；生产检查器仍保留官方 parser 和 side-data，见 [完整检查合同](video-inspection.md#windows-mp3-的容器样本与-codec-帧)。生成过程使用 [Microsoft MP3 Audio Encoder](https://learn.microsoft.com/en-us/windows/win32/medfound/mp3-audio-encoder) 要求的 16-bit PCM 类型和 [MPEG-4 file sink](https://learn.microsoft.com/en-us/windows/win32/medfound/mpeg-4-file-sink) 支持的 MP3 sample description；不会悄悄改为 AAC。清单记录真实生成系统版本、源码 pin 和每份文件的 SHA-256。

Windows 运行 `qt_legacy_mp3_recording --generate <全新目录>`；Linux 和 Windows 都运行 `qt_legacy_mp3_recording <同一份生成物目录>`。两端必须读取相同 SHA-256 字节，完整解码有效文件的所有轨道和 51 帧视频，并拒绝后段音频损坏的版本。只生成了缩略图不算通过。此测试验证真实编码和容器 / 全轨解码，输入图案与 PCM 是明确的 codec fixture，不能用来声称桌面或麦克风采集成功。

## 复用和许可

优先保留项目已有 `VideoMp4` / `GdiCapture` / WASAPI / gifski 行为，再复用已有固定依赖：

- `Mp4Encoder` 的 stream setup、send/receive 和 mux API 流程参考固定 FFmpeg `doc/examples/mux.c`（Fabrice Bellard，MIT）。完整原文、原版权和既有补丁在 [`third_party/ffmpeg-examples`](../third_party/ffmpeg-examples/mp4-encoder-upstream.json)；产品添加真实帧 / PCM 来源、句柄回调、实际时间、首错保留和关闭流程，不使用示例生成媒体作为采集来源。
- PipeWire stream 创建、SPA 格式协商和 dequeue / process / queue 流程参考固定 1.6.11 `src/examples/audio-capture.c`（Wim Taymans，MIT）。精确 commit / blob / hash、改动说明和许可在 [`third_party/pipewire-audio`](../third_party/pipewire-audio/upstream.json)。
- `FileHandleStream` 参考 Microsoft Windows SDK XmlLite FileStream 示例；精确 source / blob、MIT 许可和身份句柄 / Clone / 64 位 seek 等改动在 [`third_party/windows-sdk-stream`](../third_party/windows-sdk-stream/upstream.json)。
- 共享会话的发布身份检查及 admission / native-session / visible-desktop probe 延续本仓库已有实现；不引入额外第三方代码或改变仓库许可。FFmpeg / x264 / Qt / Rust 聚合各自的分发条件仍依照依赖锁及许可清单执行。

## 验证范围

下列目标各自承担不同证据，不能互相替代：

| 验证 | 实际覆盖 | 不覆盖 |
| --- | --- | --- |
| `recording_geometry` / `recording_frame` | 真实帧池、八种变换、RGBA / BGRA、负 stride、裁剪、预算、代次 / source 变化拒绝 | OS 采集和设备 |
| `recording_time` | 末区间保留、30 / 60 fps 分数 tick 舍入、最小上取整和有符号整数边界 | MF 编码、排空和原生录制 |
| `qt_recording_pipeline` | 实际 gifski / libx264 / AAC、完整解码、尾帧 / duration、真实文件归档、首停止意图、GUI heartbeat、恢复和异步退出 | 原生桌面和音源采集 |
| `live_recording_tests` | 同一生产 LiveFrameReader、真实帧池/队列、gifski/libx264 完整编码解码，首 discontinuity、暂停、格式变化、请求/非请求停止及 native future/lease 排空 | Portal 授权、原生屏幕和音源 |
| `qt_recording_admission` | 真实 QObject 生命周期、缺少 backend 的拒绝、空闲关闭和同步销毁回调 | 采集与编码 |
| `qt_recording_session`（Windows） | 真实 native preflight 和阻塞目录失败、写句柄已关闭、暂存保留、重新开始与 owner 销毁 | 成功桌面采集 |
| `qt_native_recording_probe`（Windows） | 真正可见的 Qt 图案窗口，经 DXGI / GDI → MF → 全文件解码与封面像素；保存、复制意图、丢弃、正常退出、owner 销毁 | 音频、实体多屏 / 混合 DPI / HDR / 长时录制 |
| `qt_legacy_mp3_recording` | 实际 Windows MF 编码与两端同字节全轨校验、后段 MP3 损坏拒绝 | 麦克风 / 系统音源 |
| `qt_gif_encoder` / `qt_mp4_encoder` | 同一生产纯编码服务、真实像素/PCM、完整帧和精确 AAC 尾样本、文件身份与关闭 | 桌面、权限或设备采集 |
| 私有 PipeWire audio graph | 真实受控 daemon 和官方测试信号，系统 monitor / microphone / mix / clip / daemon-loss / source-loss 六种路径 | 物理设备、会话策略和长时漂移 |
| PipeWire audio failure test | 实际不存在的 native endpoint、准备失败、取消、有界退出、FD 回收 | 有音设备、混音、热插拔和漂移 |
| `qt_media_workspace` | 完成媒体复用真实 Save As、取消保留、真实字节导出、未受管路径及同路径替换对象拒绝 | 真实桌面剪贴板目标程序 |

Windows visible-desktop probe 的参数是证据父目录，会独占创建 `run-XXXXXX` 并写入 JSON、实际 MP4 和解码 PNG。它要求真正的 `windows` Qt platform，不能把 `offscreen` 当作实际桌面通过。

固定 SDK 的 Linux 构建与组件测试可以验证上述真实库 / 文件路径。完整 P09 / P12 仍需另行记录 Windows 和 GNOME / Plasma / X11 的实际授权、光标、控制栏、四种音源组合、取消 / 权限撤销、设备热插拔、6 分钟 GIF、120 分钟上限及至少 30 分钟音画同步、资源峰值和用户关闭恢复旅程；这些未执行项保持明确的未验证状态。
