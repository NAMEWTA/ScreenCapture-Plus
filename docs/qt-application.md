# 统一 Qt 应用宿主

本页描述 `HUHUCAPTURE_BUILD_APP=ON` 实际编译的应用，以及自动化验证的范围。
它继续执行 [Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5)，不替代该 Issue 的
P12 验收。正式版本仍为 v0.0.5；旧 Ling 产品及其回归入口在 P12 通过前保留。

## 实际入口与所有权

`src/app/QtMain.cpp` 创建唯一 `QApplication`，使用 `VERSION` 生成的版本头、现有
启动参数解析器、真实用户目录和安装标记。`DesktopApplication` 持有设置、语言、媒体
归档、剪贴板、文件定位、截图、滚动、录制、局域网和更新服务。历史、设置、传输、录制
控制窗口可以关闭及重开；服务的寿命不依赖某个窗口是否可见。

普通启动通过 `SingleInstance` 的真实用户私有 socket/pipe 协调。已存在的主进程接受
动作后才确认接管；关闭时先停止接受新请求，仍持有主锁直到资源排空。
`--auto-quit` 保留独立一次性实例语义。一次性实例不注册全局快捷键，不连接或改变主实例，
并在会话、输出、窗口和实际后台任务均结束后退出。

支持系统托盘时提供截图、历史、设置、局域网及退出入口；没有可用托盘时显示可见启动窗口。
全局快捷键由 `SettingsController` 和系统后端管理。正在录制或滚动时再次激活，会恢复
已有控制窗口。Wayland 激活令牌只应用到真实可见的本机窗口角色；令牌已传递不等于合成器
一定授予焦点。

## 截图、编辑和输出

`DesktopCapture` 提供带令牌的异步截图。Windows 使用保留的 GDI 采集边界；X11 使用
真实 XCB/SHM、RandR、XFixes 和窗口枚举；Wayland 使用实际 Portal 请求及授权来源。
异步接收完成与取消均在 Qt 事件循环交付，取消不伪装为底层工作已经结束。

`CaptureSession` 保留选择、窗口吸附、移动/缩放、放大镜、像素取色和动作合同。
`CaptureWindow` 将验证过的屏幕绑定映射到界面；Portal 来源像素不能伪造成全局桌面坐标。
贴图通过共享 `EditorWindow` 和九种现有工具进入同一个文档与输出路径。
初始贴图位置只使用已经验证的实际桌面绑定，后续跨屏移动不会重新居中覆盖用户位置。

普通截图、编辑器、历史、二维码结果、录制及局域网接收使用同一媒体归档及桌面输出服务。
已归档录制通过真实 `Media::Entry` 身份输出，避免重编码或创建第二份归档。
保存对话框取消、剪贴板拒绝、源文件身份变化及持久化状态未知各自保留真实结果。
二维码识别在工作线程执行；每次复制均重新准备一次性剪贴板内容，不重用已消耗的发布对象。

## 滚动与录制

滚动截图由 `ScrollController` 调度共享拼接器和平台帧源。自动滚动仅用于真实具备该能力
的后端；授权来源不具备全局滚轮注入能力时使用明确的手动流程。隐藏控制窗口、截图、恢复
控制窗口及取消都在同一个状态机内协调，输出进入共享编辑器与归档。

`RecordingSession` 统一开始、停止、保存、复制、丢弃、失败保留和归档身份。
Windows MP4 继续使用原生 Media Foundation/WASAPI；Linux 使用固定 FFmpeg/x264 和
PipeWire 音频实现；GIF 使用已有 gifski Rust 聚合库。帧源传递受控 lease，并核对格式、
裁剪、变换和来源代数。帧生产者停止之后才刷新编码器和关闭输出句柄。

Windows 保留实际系统版本判断与显示亲和性读回；未支持排除的系统不会把黑色遮挡误报为
成功排除。可验证桌面几何时，将控制窗口放到选区外并由原生端再次检查。Wayland 不虚构
全局坐标，也不声称能够排除任意应用窗口；界面显示相应操作提示。

旧版 Windows MP4 的 MP3 音轨继续保留。固定解码构建启用 `mp3float` 和 `mpegaudio`
解析器。`qt_legacy_mp3_recording` 在 Windows 上用真实 Media Foundation 生成 H.264/MP3
文件，另一平台使用相同 SHA256 的字节做完整解码，并检查后段音频损坏被拒绝。
这项验证必须使用真实 Windows 生成物，缺失文件不以合成替代品或跳过冒充通过。

## 设置、传输和更新

设置窗口直接使用 `SettingsStore` 与 `LanguageCatalog`。运行时语言、自启动、快捷键和
媒体目录接线保留未知配置字段、损坏原始字节和旧格式迁移备份。系统操作失败时回滚相应
设置；Portal 授权缺失与快捷键没有生效会显示真实状态。

局域网由应用持有的 `LanController` 管理真实身份、网卡、发现、证书确认、传输及接收归档。
默认关闭时不会为用户生成传输身份。取消和关闭等待实际工作结束，重新打开窗口不会丢失
仍在进行的任务。跨平台真实 LocalSend 用户旅程仍属于 P10/P12 的独立验收。

更新检查依据实际安装标记选择 Windows setup/ZIP 或 Linux DEB/AppImage。
清单、同一次构建的 build-info、文件大小及 SHA256 都需验证；下载使用无覆盖发布。
开发标记和脏源码标记会被拒绝。下载完成仅提供明确安装说明和定位文件，参见
[更新边界](qt-updates.md)及[安装包](qt-packaging.md)。

## 协调退出

退出先拒绝新动作，再取消各服务并等待各自 `shutdownFinished`。剪贴板准备线程、
文件定位线程、Portal/采集工作、录制编码器、滚动帧源、传输和更新下载都参与排空。
Qt 对话框隐藏或关闭不充当工作结束信号。归档 lease 在实际读写结束后才释放；GUI 线程
不以阻塞等待或嵌套事件循环推进正常退出。

## 构建与验证

固定 Qt、Rust 聚合库和原生依赖恢复命令见 [构建说明](building.md)。完整应用使用：

```sh
# Linux amd64，已恢复固定 Qt/Portal/PipeWire/FFmpeg/Rust 输入
cmake --preset qt-application-linux
cmake --build --preset qt-application-linux --parallel 2
ctest --preset qt-application-linux

# Windows x64，已恢复固定 Qt、Rust 和 Windows FFmpeg DLL
cmake --preset qt-application-windows
cmake --build --preset qt-application-windows --parallel 2
ctest --preset qt-application-windows
```

生产目标可以设置 `BUILD_TESTING=OFF`，不要求 Python 测试驱动、Xvfb 或测试 D-Bus 守护进程。
完整自动化验证需要这些真实依赖。受限本地环境可以明确设置 `HUHUCAPTURE_X11_TESTS=OFF`
以编译其他目标，但这不算 X11 测试通过，CI 保持真实 X11 测试开启。

`.github/workflows/qt-application.yml` 编译完整双平台程序，运行应用、输出、TLS、原生
接口和退出测试，执行真实安装树部署及开发包生成。Windows 产生的旧音轨样本另由 Linux
独立解码。所有这些测试结果以具体 commit 的运行记录为准，不从另一版本借用通过状态。

自动化仍不能证明实体混合 DPI 双屏、物理键鼠与中文 IME、GNOME/Plasma 的用户授权流程、
麦克风/系统声音四种实际组合、设备热拔插与长时同步，也不等同于干净机器安装、升级及
卸载。对应门槛保持在 Issue #5；满足之前不切换稳定发布，不删除旧架构。
