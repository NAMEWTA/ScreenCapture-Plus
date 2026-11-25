# Linux 软件编码依赖验证

本入口对应 Issue #5 P00 的软件 H.264 / AAC 和 Qt 链接验证。它直接复用固定 FFmpeg 官方 API 示例，避免在依赖可用性尚未确认时另写临时编码器。**它不是正式 Linux 录屏实现，也不验证系统声音、麦克风、PipeWire 或音画长期同步。**

## 来源、配置和许可

| 项目 | 固定来源 | 复用方式 |
| --- | --- | --- |
| FFmpeg 9.0.2 | `946fcce07b6dcd0331c8cc609192aeff5e1924f8` | 直接构建 libavcodec / libavformat / libavutil / libswscale / libswresample |
| x264 API build 164 | `3a21e97bf23676a0bf4616df8bc2207c9fd7b1d3` | 直接链接软件 H.264；采用 VLC 固定依赖所选提交，源自 VideoLAN，GitHub mirror 保留相同 Git 对象 |
| `doc/examples/mux.c` | FFmpeg 同提交，blob `e037bc7815ef5b9f218b3a688b7525445dc89c42` | 直接编译；受控补丁补齐视频帧 duration，编译定义改名 main，在 Qt 验证进程内调用 |
| `doc/examples/demux_decode.c` | FFmpeg 同提交，blob `5c38742e7f76555923f5433f3cff93270d1cc747` | 借鉴解码及排空 API 流程，适配到完整解码断言和 Qt 图像转换 |

准确恢复输入与完整 configure 参数在 [dependencies.lock.json](../build/dependencies.lock.json)，示例的逐文件来源与许可证在 [upstream.json](../third_party/ffmpeg-examples/upstream.json)。`mux.c` 保留 Fabrice Bellard 的 MIT 声明；解码流程保留 Stefano Sabatini 的 MIT 归属和全文许可。

x264 原源码采用 GPL-2.0-or-later；FFmpeg 此构建明确启用 `--enable-gpl --enable-version3 --enable-libx264`，配置结果按 GPL-3.0-or-later 处理。原许可文本随验证源码保存。没有启用 nonfree、fdk 或硬件编码器，也不把这个组合标成 LGPL-only。最终与 Qt、gifski 等组合的发行包、对应源码和再链接 / 许可义务仍须在 P00 / P14 审查，当前没有完成发行部署。

构建只开启本探针所需的 H.264 / AAC 编解码、MP4 mux / demux、file 协议、像素转换与音频重采样。禁用网络、命令行程序和自动探测外部库，不把主机偶然安装的依赖带入产物。这个最小依赖集合不等于最终 P09 / 媒体接收验证所需格式清单；后续增加格式需显式修改锁文件并验证。

## 上游示例的帧时长修复

真实编码 / 解码检查暴露了末帧计数不一致。固定 `libavcodec/libx264.c` 把 `AVFrame.duration` 经重排状态传给输出 packet，但 `libavcodec/encode.c` 在未设置 `AV_CODEC_FLAG_FRAME_DURATION` 时会主动清零输入 duration；该要求也明确写在同版本 `avcodec.h`。原示例没有声明这个合同，视频包时长均为零。受控 [补丁](../third_party/ffmpeg-examples/0001-video-frame-duration.patch) 在打开视频编码器前启用此标志，并明确设置一个 codec time-base tick，也就是该示例的 1/25 秒。保留原来必须完整解码 251 帧的断言，不把预期数量改小。

同一补丁检查 `av_write_trailer` 和 `avio_closep` 的返回值，保留首个收尾错误；解码探针在最终 flush 后必须得到 EOF，不能把仍需输入的 EAGAIN 当作完成。原始 blob / SHA256、补丁和本地修改后哈希一起保留；后续生产帧时长必须来自实际时间轴，不能直接照搬示例的固定值。

## 实际验证做什么

`qt_software_codec` 在同一 QGuiApplication 进程中链接 Qt、固定 FFmpeg 和 libx264。它在私有目录中建立自己拥有的文件，调用经过上述窄补丁的官方示例，用已知测试图像和音频生成约十秒 MP4，然后：

- 比较编译头与已加载库的版本，检查实际配置包含 GPL / version3 / libx264 且没有 nonfree。
- 必须找到 `libx264` 和内置 `aac`，不能悄悄更换编码器或使用外部 ffmpeg 命令。
- 实际解码完整 MP4 和两个流，检查 251 个视频帧、音频采样数范围、非空且有限的音频样本。
- 用 libswscale 将实际解码帧转成 Qt BGRA，保存真实视频封面 PNG。
- 编码和解码句柄结束后，通过公共 FileCommitter 无覆盖提交完整 MP4；每次执行使用独占的新证据目录。

示例自己的输入为 352 × 288 / 25 FPS 和 44.1 kHz 立体声。它包含 PTS 恰好为 10 秒的末帧，251 帧每帧 40 ms 对应视频时长 10.04 秒；不能将其写成精确 10 秒。以上是上游测试信号参数，**不改变产品的 30 FPS 默认值或其他录制合同**。没有把生成的测试信号作为真实桌面或音源验收，测试通过也不能据此判断 1080p30 性能、长时漂移或 120 分钟上限。

## 构建与运行

Linux 需要固定 Qt 6.12.1 SDK，以及 C / C++ 编译器、make、pkg-config、NASM、Git 和 Python 3。CI 在 Ubuntu 24.04 使用与其他 Qt 验证相同的 SDK。

```sh
python3 scripts/build-software-media.py
export PKG_CONFIG_PATH="$PWD/.deps/media-validation/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$PWD/.deps/media-validation/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cmake --preset qt-validation -DHUHUCAPTURE_FFMPEG_VALIDATION=ON
cmake --build --preset qt-validation --target qt_software_codec --parallel 2
ctest --preset qt-validation -R '^qt_software_codec$'
```

脚本只使用空的受控工作区，不 reset / clean 已存在源码。重新构建依赖时传入新的空 `--workspace` 并相应修改前缀路径。源码 SHA、实际 configure 命令、pkg-config 版本和编译日志随 CI 保存；`artifacts/cmake/qt-validation/software-codec` 保留各次执行的 MP4 / PNG，包括失败时的真实编码结果。测试源码和复现入口并不预先表示验证成功，实际结果以对应提交的 CI 为准。

下一阶段应将验证过的库接入真实有界 FrameLease、系统声 / 麦克风采集、单调时间对齐、重采样混音以及录制结果状态；不能将本示例封装后当作完整生产录屏会话。
