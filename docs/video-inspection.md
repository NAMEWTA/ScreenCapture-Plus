# MP4 完整检查与历史封面

本页对应 Issue #5 的 P06 媒体归档接缝。接口位于
[`VideoInspection.h`](../src/services/media/VideoInspection.h)。完整文件检查现在由两平台共用的严格 FFmpeg 服务执行；Windows 保留原生 MF 录制，封面优先 MF，系统缺少解码器时使用同一软件封面服务。首轮真实反例已证实默认 MF 检查不可作为完整归档门槛，严格共享实现仍需通过同一批全部坏样本后才能登记验收完成。

## 两种操作的不同责任

`inspectVideo` 在工作线程同步检查本地文件，返回 `Inspection`。成功要求所有声明的音频、视频轨道经过实际解码，并完成各自的结束与尾帧处理。`frames` 是各视频轨道实际解出的帧数总和，`dimensions` 是第一条解出视频帧的轨道尺寸。容器时长、第一张缩略图或单独一个 EOF 都不是完整检查的替代。

`videoThumbnail` 同样是工作线程 API，仅取得第一条视频轨道的首张可用画面，返回拥有自身像素的 `QImage`。它不放大原始视频，不创建 QWidget 或 QPixmap，不修改原文件。后段音频或第二条视频轨道损坏时，首帧封面仍可能成功；调用者绝不能据此允许归档或发送。

两种操作使用现有媒体服务的 `Failure`、`Limits`、`std::stop_token`。无视频、解码器缺失、损坏、预算不足和取消都显式失败。调用者应把长时间检查放在不等待 GUI 的工作线程，保留 staging 的所有权，只有完整检查与最终文件身份复核均通过后才能调用现有独占提交。该服务不会自行提交、删除或覆盖媒体文件。

## 实现与复用依据

### Windows

[`platform/windows/VideoInspection.cpp`](../src/platform/windows/VideoInspection.cpp)
复用当前生产版 [`MediaHistory.cpp`](../src/services/MediaHistory.cpp) 的 Media Foundation SourceReader、RGB32 媒体类型、CPU 像素复制流程。录制后端仍为现有 MF / WASAPI 路径，没有添加第二个 Windows 录制器。

原生封面调用以 MTA 初始化当前工作线程的 COM，以平衡的 MFStartup/MFShutdown 管理媒体运行期。文件由只读、拒绝并发写入的 IStream 持有，再包装为 IMFByteStream。只选择第一条视频轨道，检查实际 RGB32 尺寸和 sample 长度；首帧即能返回，不再保留已经被反例证伪的原生全轨完整性实现。

当原生路径明确返回 `Unsupported` 时，先释放 SourceReader、byte stream、文件与 COM/MF 运行期，再调用共用的 [`FfmpegVideoThumbnail.cpp`](../src/services/media/FfmpegVideoThumbnail.cpp)。该实现直接提取自已经通过 Linux H.264/HEVC 真实文件测试的封面适配，继续复用固定解码器和 swscale，不另写 YUV/RGB 转换。读取错误、取消、资源预算和已报告的损坏错误不会触发回退。返回的 `ThumbnailDecoder` 记录实际产出像素的 MF 或 FFmpeg 路径，测试 JSON 同步记录；失败的两条路径均保留诊断。

选择这一最窄回退的实际依据是 [Windows job 139097475283](https://github.com/NAMEWTA/huhucapture/actions/runs/45581491082/job/139097475283)：严格共同检查已正确接受正常 HEVC、拒绝全部 HEVC 坏样本，但 8 个本应有首帧的文件均在 MF 的 `SetCurrentMediaType(RGB32)` 返回 `0xc00d5212`（`MF_E_TOPO_CODEC_NOT_FOUND`）。相同 runner 的 H.264 15 个测试通过。原始 JSON 来自 artifact `15008720480`，ZIP SHA-256 为 `44c09fbef44d785e0f4f1fbfd2e2666645130b1893facac917bb2fbb745007f5`。这说明依赖可选系统 HEVC codec 会使有效历史缺封面；它不是放宽预览断言或安装系统扩展的理由。回退修复仍须实际重跑两配置、同一 25 个样本，不把源码搬移当作 Windows 通过。

封面优先采用 `IMF2DBuffer2::Lock2DSize`，用它返回的实际 buffer 起点、长度、scanline 和带符号 stride 检查全部行是否位于持有的分配内；不能获得该接口时，才使用普通 IMFMediaBuffer 锁定和当前媒体类型的默认 stride。成功与所有失败路径均释放锁。负 stride 的首行定位复用纯核心 `validFramePlane` 规则；不会因步幅符号不同就越界访问。

原生 `ReadSample` 是同步接口。停止令牌在操作之间与音频/像素循环中检查，不能中断一个已经进入的系统解码调用。不得在 GUI 线程 join 这类工作线程，也不得把“发出取消请求”写成“系统解码已经停止”。官方说明见
[ReadSample](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-readsample)、
[Lock2DSize](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imf2dbuffer2-lock2dsize)。

`MFSampleExtension_FrameCorruption` 的非零值必须失败，但值为零可能表示“未发现”，不是 bitstream 完整证明；来源是
[官方属性说明](https://learn.microsoft.com/en-us/windows/win32/medfound/mfsampleextension-framecorruption)。
[官方 H.264 解码器说明](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder)
也没有提供一个可据以宣称完整文件严格验证的公开模式。首轮真实 CI 已用实际后段损坏及额外轨道文件证实 MF 会忽略损坏：两个视频反例分别只产生 250 / 501 帧但仍返回成功，两个音频反例也返回成功。因此完整检查迁移到共用 FFmpeg 路径，MF 仅用于成熟的首帧封面和录制；没有删除反例或放宽拒绝条件。

### 两平台共用的完整检查

[`services/media/FfmpegVideoInspection.cpp`](../src/services/media/FfmpegVideoInspection.cpp)
复用锁定的 FFmpeg 9.0.2 `demux_decode.c` 与现有
[`software_codec_probe.cpp`](../tests/qt/software_codec_probe.cpp) 的真实 send/receive/drain 流程。
每条声明的音视频轨道都有真实解码器；不使用 `av_find_best_stream` 跳过其余轨道。

输入由 Qt 打开的同一个本地 QFile 提供 custom AVIO。读、seek、interrupt 回调均能观察停止令牌；外部 `io_open` 被拒绝，MOV 的外部 data reference 也被禁用。媒体中出现的路径或 URL 不会触发额外本地文件或网络访问。

MOV 已从 sample description 给出 codec parameters，因此本路径不在安装像素预算前调用可能自行探测解码的 `avformat_find_stream_info`。每条解码器在 open 前设置单线程、明确像素/音频样本预算、错误检查及关闭错误隐藏。包的物理范围、`AV_PKT_FLAG_CORRUPT`、解码帧的 corruption / decode error flags 都会检查；每条音频轨道的全部声道及浮点样本必须有效。

完整检查要求每个被索引的样本范围位于已打开文件内；分片过程中增长的索引继续检查，EOF 后重新核对最终索引，并确认实际提交的包数量。空轨道、读取失败、解码失败、提前 EOF、结束排空后仍请求更多输入均失败。支持范围取决于锁定构建中实际存在的解码器；缺失返回 Unsupported，而不是把未检查的轨道当作成功。

Linux 的薄适配层 [`platform/linux/VideoInspection.cpp`](../src/platform/linux/VideoInspection.cpp) 和 Windows 缺少系统 codec 的回退共用一个软件封面实现。它使用同一解码服务保留的首个 CPU AVFrame；其 AVBuffer 引用在 decoder/input 关闭后仍持有像素，然后通过 swscale 缩放到受限 QImage，保留 SDR 的颜色矩阵和 full/limited range。未经验证的 PQ / HLG 色调映射明确 Unsupported。旋转元数据、非方形像素、HDR 显示一致性尚没有本批实测证据，不能作为已经完成的 UI 验收项。

## 共用的物理容器前置检查

[`Mp4Envelope.cpp`](../src/services/media/Mp4Envelope.cpp) 仅复用固定 FFmpeg
`mov_probe` / `mov_read_default` 的顶层 size、extended-size、size-zero 遍历。它直接读取解码器持有的同一 IStream 或 QFile 的固定长度头部，并检查：

- 每个头部实际完整，声明 box 长度不小于头部、不越过物理文件结尾；所有加法先核对剩余范围。
- 存在物理 `moov` 与 `mdat`；没有把音频-only 文件误认为视频的责任仍由真实解码层承担。
- 除 `mdat`、`free`、`skip` 之外的顶层元数据总字节数在配置预算内。
- 每次读头之间检查取消。

这里没有实现 sample table、codec 或完整 MOV 语义解析。内部子 box 或 STSZ 表是否一致仍必须由成熟解析器及实际反例验证。

加入该检查的具体原因是固定版
[`mov.c`](https://github.com/FFmpeg/FFmpeg/blob/946fcce07b6dcd0331c8cc609192aeff5e1924f8/libavformat/mov.c)
在 `mov_read_default` 中把超出父 box 的尺寸夹到剩余范围，并对部分 overread 只警告后回退。上游
[`a9901840`](https://github.com/FFmpeg/FFmpeg/commit/a9901840079b156c88c6d9ad4a61bc6e7e9c6235)
还特意将部分 STSZ 截断改为警告后成功。这些播放恢复策略不能直接成为“收到的原文件完整”的证据。实际 STSZ 多声明一项的反例已在未打补丁的 Linux 检查中被错误接受。受控 [`0001-strict-mov-validation.patch`](../third_party/patches/ffmpeg/0001-strict-mov-validation.patch) 只为明确请求 AV_EF_EXPLODE 的调用者拒绝内部 box 尺寸夹断、parser overread、采样表 payload/count 不符及 STSZ short-read；默认播放恢复策略不变。它还在 STSZ/TRUN 展开分配前执行显式采样索引预算。没有继续扩展自有 MOV 语义解析器。

## 预算的实际边界

当前明确核对文件字节数、顶层元数据字节数、最多 16 条轨道、声明/解码后的像素、组合视频轨道像素、压缩包大小、音频样本/声道、物理采样索引范围。Qt 图像仅在 worker 内创建并通过拥有像素的值返回。

这些检查不是媒体库所有内部堆分配的操作系统硬上限。受控补丁为明确的 STSZ/TRUN 展开施加 `maximumImageBytes / 16` 的每轨采样索引预算，默认每轨 32 MiB；它与 decoder 私有工作集、其他 metadata 类型的展开不同。上游未打补丁的
[`AVFormatContext::max_index_size`](https://github.com/FFmpeg/FFmpeg/blob/946fcce07b6dcd0331c8cc609192aeff5e1924f8/libavformat/avformat.h)
的官方注释明确说明，必须保留完整索引的解复用器可以忽略它；小的 STSZ 头也能声明很大的表。不能把事后索引检查或顶层 moov 大小限制描述为已经证明所有元数据展开分配有界。内部表展开、极端动态尺寸与总解码器内存仍需严格补丁/实测证据；没有满足必要预算与完整性门槛前，不得发布。

## 真实文件测试

[`video_inspection_fixtures.cpp`](../tests/qt/video_inspection_fixtures.cpp)
在 Linux 使用固定的上游 `mux.c` 生成真实 352×288 H.264 和 44.1 kHz 双声道 AAC。它通过固定 `remux.c` 的 API 流程生成 video-only、audio-only、fast-start 和两组 AV 轨道，然后修改实际压缩文件中经过真实解复用确定的后段 packet 或容器字节。不是模拟解码器，也没有用 Mock 媒体替代文件。

生成器接收一个尚不存在的私有目录，写入 15 个 MP4 与 `manifest.json`。清单记录每个文件的 SHA-256、预期接受/拒绝、预期视频帧数和首帧是否应可用。相同文件应作为 CI artifact 交给 Windows 与 Linux 的同一个
[`video_inspection_tests.cpp`](../tests/qt/video_inspection_tests.cpp)。测试入口接收 fixture 目录和 evidence 目录两个参数。

| 样本/检查 | 必须得到的实际结果 |
| --- | --- |
| 正常 AV、fast-start、video-only | 完整接受，251 个视频帧 |
| 两组真实 AV 轨道 | 完整接受，视频帧合计 502；两条音轨均解码 |
| audio-only、空文件 | 拒绝，没有可用视频封面 |
| 第一视频/音频轨的后段单包损坏 | 完整检查拒绝；未损坏的首帧可独立生成封面 |
| 第二视频/音频轨的后段单包损坏 | 完整检查拒绝，证明没有仅检查第一条轨道 |
| fast-start 文件最后一个媒体字节缺失 | 拒绝，不把 EOF 当成完整尾帧 |
| 最后一个容器字节缺失、残缺 trailing free box | 拒绝，不能因为先前 AV 可解码而忽略物理截断 |
| STSZ 声明 252 项但实际仅 251 项 | 拒绝，不能把下一 sibling 的头部当成额外 size 后仍接受 |
| 内部 stsd 子 box 声明超出其父范围 | 拒绝，不能依赖宽容的尺寸夹断 |
| Unicode 路径、封面边长与不放大 | 使用真实文件并验证实际图像尺寸/像素变化 |
| 文件/像素预算、预取消、运行中停止 | 显式正确失败，返回后文件句柄释放 |

每轮测试在独占临时目录保留真实封面 PNG 和 JSON 结果，包括意外接受的文件。坏样本断言失败必须使测试进程失败；不得为了让 CI 变绿而把它们标为可接受。工作区缺少 Qt / FFmpeg 开发包，不能将本地源审当作编译或运行通过；实际平台证据以对应 CI commit、日志和 artifact 为准。

## 已取得的首轮反例证据

[commit 38e3906](https://github.com/NAMEWTA/huhucapture/commit/38e3906a219d7efedd51d39f1c318c168a66bdc3) 的独立
[视频检查 workflow 45569261154](https://github.com/NAMEWTA/huhucapture/actions/runs/45569261154) 已实际构建并执行相同 15 个文件：

- [Linux job 139051576055](https://github.com/NAMEWTA/huhucapture/actions/runs/45569261154/job/139051576055)：14 个预期符合；`stsz-extra-entry.mp4` 被默认 MOV 恢复后解出 251 帧并误接受，因此 job 正确失败。
- [Windows job 139053165531](https://github.com/NAMEWTA/huhucapture/actions/runs/45569261154/job/139053165531)：4 个 late/second 音视频损坏被 MF 误接受，视频帧数为 250 / 251 / 501 / 502，因此 job 正确失败。
- 实际共享 fixture artifact ID 为 15000868614，上传与 Windows 下载 ZIP SHA-256 均为 `a23116f8d4ff3056079f948fdcdebf58b1005d977a5012c4705a65a731851f58`。每个媒体文件自身还由 manifest 的 SHA-256 再检查。

这些是选择共同严格检查路径的已观测依据，不是重构完成或新实现通过的证据。两平台在修复后仍须用同一套拒绝断言重跑，不能把失败 CI 改写成成功。

## 构建接线与来源

共同服务源是 `Mp4Envelope.cpp`、`FfmpegVideoInspection.cpp` 与 `FfmpegVideoThumbnail.cpp`，再加一个平台的 `VideoInspection.cpp` 封面适配。共同代码需要 core、Qt6::Gui、固定 AVCodec/AVFormat/AVUtil/swscale。Windows 原生封面另需 `huhucapture_media_frames` 及 MF、COM、SHCreateStream 库。

Windows 使用 [`build-windows-video-inspection.py`](../scripts/build-windows-video-inspection.py) 从同一固定 FFmpeg commit 与同一严格补丁构建，包含 H.264/HEVC/AAC 软件解码器、MOV demuxer、所需 AVUtil 和封面转换所需 swscale 10.1.102；不加入 x264、录制编码器、网络协议或新 GUI 运行时。脚本采用官方 MSVC `configure --toolchain=msvc` 路径，保留 x64 NASM 优化，明确构建 `install/Debug` 的 `/MDd` 和 `install/Release` 的 `/MD`，各自对应相同 Qt/应用配置。不得把两个配置的 DLL 混入同一测试 PATH。四个原生 `.lib` 导入库与带 major 的 DLL（含 `swscale-10.dll`）均由固定上游安装到各配置的 `bin/`，CMake 逐个核对这八个文件的真实 SHA-256；链接 PDB 保存在 `symbols/`。

只读取真实 VSDevCmd 的编译环境，不把 MinGW 库混入 MSVC。NASM 3.01 与 GNU Make 4.4.1-3 从官方固定 URL 下载并逐个核对 Microsoft vcpkg 源码记录的 SHA-512，在私有 workspace 解压；Bash/coreutils 使用 Windows runner 已有 MSYS2，记录其实际 runtime。不会安装或覆盖系统文件。构建元数据保留源 commit、补丁 SHA、实际 CFLAGS、工具来源、DLL/导入库 SHA；缺少文件、PDB 或实际 CRT 不符均失败。

fixture generator 仍只在 Linux 使用固定完整软件编码链与 `ffmpeg_mux_example` 生成一次。Windows 使用同一 artifact，不另写或模拟编码器。

精确来源、blob、改动范围及完整许可证位于：

- [`ffmpeg-examples/video-inspection-upstream.json`](../third_party/ffmpeg-examples/video-inspection-upstream.json)：MIT 的 demux/decode 与 remux 示例，保留 Stefano Sabatini 的相应年份与完整许可。
- [`ffmpeg-mov/upstream.json`](../third_party/ffmpeg-mov/upstream.json)：LGPL-2.1-or-later 的物理 box 遍历来源，保留原作者与完整 LGPL 文本。示例的 MIT 许可不能替代 MOV 实现许可。
- [`software-codecs.md`](software-codecs.md)：实际 FFmpeg/x264 链接配置、GPL-3.0-or-later 发行要求及固定依赖来源。
- [`media-history.md`](media-history.md)：文件身份、staging、归档与发送租约的服务边界。

本批不替代真实截图/录屏、系统/麦克风音频、安装升级卸载、混合 DPI 或整套应用交互验收，也不以一组自生成样本证明所有第三方媒体均可完整检测。
