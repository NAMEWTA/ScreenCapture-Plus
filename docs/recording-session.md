# 录制会话与 Windows 原生后端

本页记录 Issue #5 P04 / P09 的实际提取边界。当前新增的是共用 Qt 录制会话、从 Ling 主窗口提取的 Windows MP4/GIF 后端及共用 GIF 编码服务；完整 Linux 录制与正式录制工具条旅程仍需接入和验收。不能把这个模块或单个探针视为 P09 完成。

## 分工与复用

[`RecordingSession`](../src/recording/RecordingSession.h) 只负责应用层的一次录制：任务准入、状态、停止意图、临时文件所有权、完整检查、归档和失败保留。UI 根据其信号显示准备、录制、停止、封装与归档，不参与逐帧处理。`PreparedRecording.started` 来自工作线程的单调时钟，准备成功后才用于界面计时。Qt 定时器不产生媒体时间戳。

[`RecordingBackend.h`](../src/recording/RecordingBackend.h) 是值和所有权合同，不包含 Qt 或 Win32 类型。它接收已经独占创建、核验身份的空文件写入句柄，消费并在返回前关闭该句柄。坐标单位是虚拟桌面的物理像素，允许负原点；Qt 逻辑坐标、DPR 和被授权源坐标的转换必须在来源已经验证的接缝完成。Wayland 截图 URI 不提供全局桌面映射，不能凭 DPR 猜出 Windows 风格的全局选区。

Windows 的 [`runNativeMp4`](../src/platform/windows/NativeMp4Recording.cpp) 从原有 `CapVideo` 提取设备选择、COM/MF 运行期、音源选择与 HEVC→H.264 回退，继续调用现有 [`VideoMp4`](../src/recording/VideoMp4.cpp)。DXGI/D3D11 采集、WASAPI 端点、采样转换与混音、MF 编码参数保持同一实现。旧 Ling `CapVideo` 也调用这个入口，没有为 Qt 另写一套录制器。

默认 30 FPS、120 分钟上限、系统声默认开启和麦克风默认关闭，继续遵守 F08。帧率、音源和上限在后端入口检查。当前 MP4 入口明确拒绝 GIF 请求；没有返回占位成功。底层既有 Pause 字段不会被推导为新的可见暂停/恢复功能。

旧 Ling 展示层也使用同一原生 MP4/GIF 入口，并复用同一任务、身份核验和 GUI 错误上报路径。其停止仍使用原有同步 join；这些展示层是尚待最终迁移删除的范围，不能描述成所有旧 UI 已经异步化。新 Qt 会话不使用该展示层。GIF worker 不再打开会阻塞 stop/join 的模态错误窗口。

## 文件对象与媒体收尾

媒体服务创建 staging 并提供 `StagingFile::openWriter`，平台取得该实际打开对象。MF 不再通过文件名重新打开或自行截断任意路径。桥接顺序是：

1. `FileHandle` 持有身份核验后的原生 CRT 描述符。
2. [`FileHandleStream`](../src/platform/windows/FileHandleStream.cpp) 用 `DuplicateHandle` 持有同一个文件对象，提供可定位的 `IStream`。
3. 官方 `MFCreateMFByteStreamOnStream` 生成 `IMFByteStream`，交给原有 SinkWriter。
4. 停止采集和音频 worker 后，检查 SinkWriter `Finalize`，再检查 byte stream `Flush` / `Close`，释放 COM 引用并显式关闭原始 `FileHandle`。
5. 共用服务完整解码验证所有声明的轨道，再核验文件身份并执行无覆盖持久提交。

桥接复用 Microsoft Windows SDK 的 XmlLite FileStream 样例。新增的有界 CopyTo、64 位 Seek/SetSize、独立 Clone 游标与串行原生访问，仅用于兑现 `IStream` 的真实文件合同；没有实现第二个 MP4 封装器。文件名被外部替换时，已经打开的媒体流继续写原对象，归档服务拒绝提交被替换的路径。编码回退只在上一个 writer 已释放后重置同一个会话拥有的 staging 对象。

来源 commit、逐文件 blob、修改说明和完整 Microsoft MIT 许可在
[`third_party/windows-sdk-stream`](../third_party/windows-sdk-stream/upstream.json)。官方桥接接口依据是
[MFCreateMFByteStreamOnStream](https://learn.microsoft.com/windows/win32/api/mfidl/nf-mfidl-mfcreatemfbytestreamonstream)，使用公开 API，未依赖未公开的按文件句柄创建 MF 文件函数。

[`RecordingResult`](../src/core/RecordingResult.h) 继续保持单 writer。主媒体 worker 在音频线程终止后合并首次音频错误，结果通过 future 完成后才交给 GUI 读取。首次采集/编码错误与附加收尾错误都保留，成功 Finalize 不能覆盖已有失败。无帧且干净停止是取消；有帧且失败保留独有临时路径，不能进入成功历史。`canArchive()` 只表示后端具备完整提交候选，仍须经过共用媒体检查及持久提交。

`framesWritten` 的既有字段名表示成功提交的输入帧数，不能解释为封装后的解码帧数。MF 链中视频处理器默认按输出媒体类型转换帧率；GIF 也可能合法合帧。Windows 每次 `WriteSample` 现在持有新建且提交后不再修改的 `IMFSample` / `IMFMediaBuffer`，遵循 Microsoft [Sink Writer 示例的真实 WriteFrame 源码](https://learn.microsoft.com/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video)，避免改写仍被异步转换/编码阶段引用的输入。输入媒体类型明确声明 progressive，30 FPS、实际单调输入 PTS 与原有转换/编码选择保持不变。

同步 `Finalize` 返回后记录官方 [GetStatistics](https://learn.microsoft.com/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-getstatistics) 的实际 HRESULT、received / encoded / processed 数量、各阶段最后 PTS、原最后输入 PTS/时长以及排队字节。查询失败只记录实际错误，不伪造统计，不覆盖媒体错误，也不代替完整解码。Microsoft 对 [统计各阶段](https://learn.microsoft.com/windows/win32/api/mfreadwrite/ns-mfreadwrite-mf_sink_writer_statistics) 与 [默认视频帧率转换](https://learn.microsoft.com/windows/win32/medfound/mf-xvp-disable-frc) 有分别定义，不能把几个阶段默认视为一一对应。

## 全区域与实际来源

DXGI 只选择能够完整包含目标物理选区的输出，并验证实际取得输出的桌面矩形与旋转状态。不能先把跨屏矩形裁到最近显示器，再把较小视频当作原请求成功。重建 duplication 时再次验证输出，防止显示器变化后录入不同区域；恢复后的尺寸、像素格式或 HDR 输入类别改变时，明确结束为采集失败并收尾保留已有数据。已经开始的 MF 输入媒体类型不会偷偷解释另一种布局，也不会把未经验证的动态重配置记作支持。

跨输出、旋转或 duplication 准备不可用时，复用现有 GIF 使用的 GDI 全桌面像素源进入同一 MF 编码/音频链。GDI 的 top-down BGRA 直接按行写入原有 bottom-up RGB32 输入缓冲，没有新增中间整帧图像。`PreparedRecording.source` 报告实际使用的 DXGI 或 GDI；它不承诺该机的性能或 HDR 已验收。DXGI 光标坐标减去实际显示器的桌面原点，避免负坐标/第二屏使用主屏原点。

尺寸和 stride 预算在分配前复用 `pixelLayout` 检查。MP4 选区按现有四像素宽、两像素高规则向内对齐，不向外扩大极小选区。首帧等待有绝对单调截止，超时明确失败；取得真实第一帧后，既有静止画面复用仍保持原媒体时钟。设备恢复、音源四组合、跨屏和 HDR 都需要真实环境进一步验证。WASAPI 原始 PCM 队列现在共享默认 16 MiB 的待消费 payload 预算，均分给实际选择的音源；接口拒绝零预算或超过 64 MiB 的配置。`REBUFFER::PushX` 在分配前检查加法与剩余空间，超限整个包拒绝并进入现有首次音频错误通道（`ERROR_NOT_ENOUGH_QUOTA`），之后仍归还该 WASAPI 包。不会以丢样本继续录制的方式隐藏积压。每个转换后的 float 声道也按该源最多容纳的帧数限制；现有 PCM16/float32 与声道映射下，其合计 payload 不超过原始预算的两倍。此处不改变重采样、混音、采样率、帧率或正常批次大小。

这只是明确的 PCM 排队 payload 上限，不是整个进程的 RSS 硬上限：std::vector 的容量/分配器开销、当前 PopBuffer/混音工作副本以及 MF 私有内存还要实际测量。源码拒绝路径与参数 preflight 也不能代替真实音频生产、编码停滞和设备变化测试；这些仍属于长时资源门槛。

## 共用 GIF 编码与原生帧源

[`GifEncoder`](../src/services/media/GifEncoder.h) 复用原有 gifski 1.34.0，保持 quality 80、fast、无限循环；Qt 架构从已有唯一 Rust aggregate 调用原 C ABI，没有新增编码器或 Rust runtime。对应 commit 和实际阅读的 `c_api.rs` / `collector.rs` / `lib.rs` blob 记录在 [`third_party/gifski/upstream.json`](../third_party/gifski/upstream.json)，AGPL-3.0-or-later 许可及作者归属仍然适用。保留的原 Windows 工程在最终迁移前继续使用其原有匹配 CRT 的 gifski 构建，不能把 `/MT` 库混入 Qt `/MD` 可执行文件。

编码器消费真实的空 staging writer，以 `gifski_set_write_callback` 写入同一文件描述符；它不调用会重新按路径创建文件、在失败时自行删除文件的 `gifski_set_file_output`。底层的顺序 write / flush 放在两平台 `FileOutput.cpp`，检查短写、系统错误和输出字节预算。写 callback 只传递原子首个 I/O 错误；`RecordingResult` 始终由录制 worker 单独写入。`gifski_finish` 先完成内部线程，再核对 I/O、flush 和 close，之后才能检查或归档；默认 32 GiB 输出预算与共享归档上限一致。没有帧时不启动 writer，明确保留干净取消语义，而不是吞掉真实 `INVALID_STATE` / finish 失败。

输入可以是原 GDI 的借用 BGRA 行，也可以是已有 `FrameLease`。RGB 工作缓冲只分配一次，BGRA 转换继续用原 `PixelBuffer` 规则；RGBA、padding 和负 stride 按共享平面描述逐行读取。gifski 接口在返回前已复制输入，因此生产者可立即释放 lease 或复用 DIB；测试实际复用了单槽帧池验证这一点。只接受已裁剪、已归一化方向的 opaque SDR BGRA/RGBA：旋转、非完整 crop、YUV、PQ/HLG、广色域或受限范围需要先经过实际颜色/几何服务，不能直接当线性 RGB 写入 GIF。未指定颜色沿用原桌面无色彩管理的 sRGB 输入约定，不宣称完成 HDR 色彩管理。

输入帧号由编码器按序产生，时间必须从零严格递增，防止上游对乱序索引排队或静默跳过重复 PTS。完整 GIF 检查的既有 6000 帧预算移到共享常量，编码和录制准入使用同一限制。gifski 的输入通道与处理管线复用上游有界队列；这不是其量化器所有内存和进程 RSS 的硬上限，仍需长时性能实测。

[`runNativeGif`](../src/platform/windows/NativeGifRecording.cpp) 继续使用 GDI 全选区、默认 16 FPS 和 6 分钟上限，不强加 MP4 的像素对齐，也不裁小跨输出选区。GIF 不支持音频，请求携带音源时明确拒绝。上游最小帧延时为两厘秒，所以该接口拒绝高于 50 FPS 的请求；更高帧率与时长的组合也须符合上述完整检查预算。生产默认 16 FPS 未改变。帧间等待使用可停止的单调时钟；Save / Copy / Discard 停止捕获后仍排空已提交帧，由同一 `RecordingSession` 决定归档、删除或保留。正常取消不会通过进度 callback 截断已提交的 GIF。GDI 调用在工作线程执行，首帧截止在实际返回后检查；不能把它描述成可以强行中断阻塞的显示驱动。

旧 `CapVideo` 的独占文件身份现在直接从 `CREATE_NEW` 返回的实际句柄读取，再按该身份重新打开；关闭创建句柄后发生的路径替换不会变成新的所有权。旧独立 `VideoGif` / cgif 已不被这个展示层调用，其源和构建项在原生接线通过回归后清理，不应被当成第二条生产业务路径继续扩展。

## 停止意图、回调和退出

一个应用级会话同一时刻只准入一个 worker。首个 Save / Copy / Discard 通过原子选择固定；后续点击不能把正在归档的保存变为丢弃。自动到时结束与 UI 停止也竞争同一个意图选择，决定完成后不再接受迟到的改变。

Save / Copy 停止后先完整收尾、关闭、检查和归档；`Completed` 表示 durable archive 已完成。之后的另存为对话框、真实文件剪贴板由共用输出服务处理，并报告自己的结果。取消另存为不会使已归档媒体丢失。Discard 则只显式清理该 staging，清理失败要保留路径并报告，不能只依靠析构中的静默清理。

工作线程在取得 staging writer 时记录实际文件身份，编码关闭后记录最终字节数，持久提交后核对归档路径仍指向该对象，并把完整的提交后 `FileSnapshot` 放入 `SessionResult.archivedEntry`。GUI 不重新读取路径或替换这个快照；输出服务必须对该 `Entry` 申请媒体 lease，并沿用现有变更检查。重命名可能改变原生 change-time，因此不能把重命名前后的全部 revision 当作相等条件；完整检查前后的严格快照比较仍由归档服务执行。最终快照读取失败、对象被替换或字节数变化时，结果为 `RecoverableFailure`，已发布的 `archive.path` 仍保留，不能假报复制/另存为可用。这个身份接缝不宣称阻止其他进程随后改写文件；后续 lease 会核验完整快照。

实现复用 QtConcurrent / QFutureWatcher。工作线程只持有值、服务与文件 token，不等待 GUI，不调用 QWidget。准备/收尾的 queued 通知有 QObject context、弱 delivery 生命周期和任务 token；终态回调支持启动下一次录制或同步销毁原 owner。future 的结果按值移出，不通过成员共享可变的原生录制参数。

正常退出必须先停止准入，再调用 `shutdown()`，保持应用和事件循环直到 `shutdownFinished`。它停止采集、取消尚未完成的完整检查，保留未提交的有帧媒体，既不假报成功也不把退出视为 Discard。意外销毁 QObject 时，不在 GUI 中 join 原生 flush；worker 仍拥有句柄和 staging，按相同规则清理/保留。这个析构兜底不能代替应用在正常退出时等待服务完成，也不能证明进程被强制终止后封装完整。

## 构建与验证边界

原生库继续编译 `VideoMp4.cpp`、`GdiCapture.cpp`，增加 `NativeMp4Recording.cpp` 与 `FileHandleStream.cpp`，链接已有 MF/DXGI/WASAPI 系统库及共享文件服务。新增 cpp 不使用 Ling PCH。Qt 会话独立链接 Qt6::Core、Qt6::Concurrent 和 `huhucapture_media_archive`，开启 AUTOMOC；其接口不链接到具体平台后端，后端由应用组合。

| 检查入口 | 实际覆盖 |
| --- | --- |
| `tests/recording_geometry_tests.cpp` | 纯 C++ 负原点、对齐、溢出、预算与小矩形边界 |
| `tests/platform/windows_file_stream_tests.cpp` | 真文件短读写、64 位游标、Clone、外部名称替换、原描述符关闭后的同对象存活；真实 MF stream 桥接；原生无效参数/预取消 |
| `tests/qt/recording_admission_tests.cpp` | 真实 Qt 服务的缺少平台能力拒绝、无任务不造文件、幂等退出和同步 QObject 销毁；不提供替代采集后端 |
| `tests/qt/recording_session_tests.cpp` | 实际原生 preflight 与文件系统失败、保留 staging、终态重入、QObject 删除和已关闭会话拒绝准入；不触碰采集设备 |
| `tests/qt/gif_encoder_tests.cpp` | 固定 Rust 编码器与严格完整解码、padding/负 stride/单槽 lease、像素/时间轴/无限循环、真实 OS 写失败、预算失败、重命名文件对象与归档身份拒绝 |
| `tests/qt/native_recording_probe.cpp` | 真实可见 Windows 窗口→实际 DXGI/GDI→MF MP4；保存/复制意图/丢弃、关闭与 owner 销毁；严格输入/输出阶段计数、逐帧及尾部时间轴、实际封面方向/裁剪、文件与 JSON 证据。同文件以 `HUHUCAPTURE_GIF_RECORDING_PROBE=1` 构建 GIF 探针，复用相同生命周期旅程并检查完整解码、实际像素与 16 FPS 时间轴 |

桌面探针需真实 Windows Qt platform、可见并已经绘制的窗口，不能以 `offscreen` 替代。它通过现有 native backend 取得桌面画面，不把 QWidget::grab 的内存图像送给编码器。探针明确关闭两个音源，逐例记录实际 source / codec；复制意图只证明归档前序，不冒充真实应用中的剪贴板粘贴旅程。退出清理和事件循环心跳仅说明这次短录制的行为，不替代长时间性能验收。

当前本地已实际以 g++14 编译运行纯 C++ 几何/完成结果测试；恢复固定 SDK 后，以 Qt6.12.1、CMake/Ninja、`-Wall -Wextra -Werror -Wpedantic` 实际编译共用会话/MOC、归档与 Linux 文件服务，并运行 `recording_admission_tests` 通过。这个测试没有替代采集后端。Windows 文件/MF 与真实 native preflight 的 Qt 会话测试已随 `6d2283f5bf640c159b065f1d4227aa0bb96282f8` 的 Windows Debug/Release 回归实际执行；桌面 MP4 的完整旅程仍需下述统计合同修正后的 CI 裁决。不得将测试设计写成已经通过。四音源、设备拔出/重选、实体混合 DPI/双屏、HDR、100 次启停、30 分钟同步、时长上限、Linux MP4/GIF 和正式 UI 旅程仍是未完成门槛。完整媒体检查的严格坏样本及系统 HEVC 封面差异见 [video-inspection.md](video-inspection.md)。

共用 GIF 测试已在本地固定 SDK 与来自实际通过 CI 的 Linux Rust aggregate 上运行通过：恢复前核对了 GitHub artifact digest、库 SHA256 及 metadata 中全部 98 个源文件指纹。测试使用明确生成的颜色帧来验证真实编码器、解码器和文件系统，没有替代编解码器或伪造写成功；它不证明桌面采集。GIF 合法合帧意味着输入帧数不必等于 GIF 图像块数；GIF 探针检查两个真实解码器的一致帧数、所有实际图像和完整时间轴，MP4 则检查下面的各阶段精确等式及完整实际时间轴。Windows GDI→共用 GIF 桌面探针已在 [workflow 45596538478 / job 139143974308](https://github.com/NAMEWTA/huhucapture/actions/runs/45596538478/job/139143974308) 实际通过（4.18 秒）；跨屏、物理 DPI 和长期录制仍未由该次短探针验收。

同一 GIF 用例也实际通过了 C++ AddressSanitizer / UndefinedBehaviorSanitizer 构建与执行；Qt 和已构建的 Rust archive 未重新插桩。LeakSanitizer 在当前受跟踪的执行环境中报 `does not work under ptrace`，没有取得泄漏检测结果；随后仅关闭该不可运行的检查，保留地址与未定义行为检查运行通过。不能据此声称 Rust 内部、整个进程或长时间录制无泄漏。

首次 Windows 实采（commit `51eabbfd6e56b23a4b924618bfcccc4b1d810bcb`，workflow `45588774226`，job `139129028534`）在输入/输出帧数相等断言失败，不能登记为录屏通过。实际 artifact 的 MP4 SHA256 为 `dea3e12af4f244cc4b4ad0652ce27cabe051fe3e58033f14603c33a81f1a0d65`：记录器报告 16 次输入提交，本地固定 FFmpeg 对实际文件逐包、逐帧解码得到 21 个 H.264 包和 21 帧，尺寸 252×190，time base 1/30000，PTS 从 0 到 20000、每步 1000，完整媒体时间轴为 700 ms。此时只知道数量不同，因此保留原断言，先修复样本所有权并收集官方阶段统计。

第二次实采在 [workflow 45596538455 / job 139150279329](https://github.com/NAMEWTA/huhucapture/actions/runs/45596538455/job/139150279329)、commit `e23d0bf66854e2e8b67f23b8c8b4a92dc67066a6` 上，真实统计明确显示 **15 submitted = 15 received；21 encoded = 21 processed = 21 decoded；queuedBytes = 0**，GetStatistics HRESULT 为零。最后输入 PTS/时长为 6,399,810 / 484,322（100 ns），最后 encoded/processed PTS 均为 6,666,667。完整再解码得到连续 30 FPS 输出，最终结束时间为 7,000,000，覆盖最后输入区间 6,884,132，超出量小于一个输出帧。实际媒体 SHA256 为 `d26237fb1affe30101ce00b53a2f917ae9875c084a7b19f5af125301f869f208`，下载的 Windows ZIP digest 为 `823a9329b78ac5c27c48fb012a74ff18752d2f49f01d5bdce3d6d1ea21e7b289`。这些实际阶段证据与官方默认帧率转换合同一致，定位的是处理阶段的帧数变化，不能把它称为编码尾帧丢失。

因此 [`recording_timeline.h`](../tests/qt/recording_timeline.h) 采用以下更精确的严格合同：submitted 必须等于 received，encoded 必须等于 processed 及完整解码帧数，末次接收必须等于末次提交，排队字节必须为零；逐个真实解码帧必须具有从零开始、连续的标称 FPS PTS/duration；最后解码 PTS 必须匹配 MF，完整输出槽数必须等于实际输入末端在标称 FPS 上的最近整数槽数。这里没有把数量断言改成 `>=`，也没有改变原有 30 FPS、FRC 或编码器选择。这个 helper 已在本地固定 SDK 上实际编译，并针对 e23 和以下 8e 的原始 Windows 文件及真实统计分别重放；故意不一致的阶段计数、排队状态和尾区间必须被拒绝。所有 Windows Save/Copy/Discard/关闭旅程仍等待修正后实际 CI，不能由重放两个文件宣称整套桌面探针通过。

第三次实采 [workflow 45603219018 / job 139182210720](https://github.com/NAMEWTA/huhucapture/actions/runs/45603219018/job/139182210720)、commit `8e21da0fffad8df77b96b7fec85b20010691ff2e` 在首个 Save 的尾时间断言失败。真实结果为 **16 submitted = 16 received；22 encoded = 22 processed = 22 decoded；queuedBytes = 0**；完整归档检查已经通过。输入末端为 7,338,865（100 ns），输出末端为 7,333,333，最后编码/解码 PTS 均为 7,000,000。原单向条件要求输出只能向上覆盖，错误拒绝了这次向下 5,532 ticks（0.5532 ms）的整帧量化。实际 MP4 SHA256 为 `faeabad89d7a5c38f96659658dfd1d02b5c40a6937ad3dbd8e01385e261c78ae`，Windows artifact ZIP digest 为 `c30c4b8204f6f10c1fe41ef3627ff46be91f411b944db21bfae1c2940749ffe6`。QtConcurrent 的 `QUnhandledException` 又将原始断言显示为 `Unknown exception`；探针现在重抛其保留的原始 exception_ptr，失败仍失败，同时输出真实断言和尾数值。

Microsoft [时间戳与时长规则](https://learn.microsoft.com/windows/win32/medfound/time-stamps-and-durations) 说明转换阶段可能改变时间划分及产生舍入；它没有公开保证上述最近整数槽的具体 FRC 算法。这里的精确量化条件来自这两次实际阶段/文件证据，是待持续验证的探针约定，不是对所有硬件转换器的未经验证承诺。它同时拒绝多出或缺少一个完整输出槽，并保留每个实际编码输出必须进入文件的精确数量关系；没有以毫秒容差吞掉未知尾帧损失。新的帧率、编码器、音轨或驱动仍需自己的实际证据。
