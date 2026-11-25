# 共享 Qt 应用宿主与退出边界

`src/app/qt_main.cpp` 和 `QtApplication` 是 Windows、Linux 共用的实际程序入口。
入口使用现有品牌、`build/version.json` 生成的版本和嵌入图标，并设置稳定的
desktop file name `io.github.NAMEWTA.huhucapture`。CMake 的
`HUHUCAPTURE_BUILD_APP=ON` 构建此入口；完整链接并不表示 P12 的桌面、安装、
性能和媒体验收已经完成，当前发布边界仍见 [迁移状态](qt-migration.md)。

## 一个进程内的所有者

宿主先确定配置与当前桌面的单实例范围，再装配服务。成功转发到已有进程的
调用者不会创建第二套归档、快捷键或局域网监听器。独立模式仍使用真实服务，
只是没有常驻进程专属的快捷键、局域网控制器和系统托盘。

| 所有者 | 共享的实际职责 |
| --- | --- |
| `QtApplication` | 路径、启动角色、可见入口、托盘、服务接线和最终退出屏障 |
| `SettingsStore`、`LanguageCatalog` | 同一配置快照、未知字段保护和六语资源；不因打开窗口创建新的存储 |
| `MediaArchiveService`、`SystemClipboard`、`FileRevealer` | 受管媒体、唯一系统剪贴板发布服务和真实文件定位 |
| `MediaWorkspace` | 真实历史、缩略图、编辑输出、另存、复制与已有归档输出 |
| `CaptureWorkspace`、`RecordingWorkspace` | 截图动作会话与录制生命周期；平台后端只提供实际采集、音频和编码能力 |
| `SettingsController`、`LanController` | 复用同一配置和归档，管理实际系统设置、设备发现、发送准备与接收归档 |

窗口是这些服务的视图，关闭历史、设置、编辑或传输视图不会销毁全局服务。
局域网发送使用历史服务提供的确切条目，准备与读租约仍由 `LanController`
的有界 worker 持有；接收归档完成后通知同一 `MediaWorkspace` 刷新。文件定位
也回到共享媒体服务核对归档身份，不因接收到一个路径就直接交给系统打开。

完整 GIF 与 MP4 历史检查分别使用生产的 gifski/媒体检查边界和平台完整视频
解码实现。Windows 入口按真实格式选择既有原生 GIF 或 MP4 录制后端；设备、
编码器和音源是否可用由真实准备结果决定。Linux 录制提供者尚在接线阶段，
入口不安装空后端，也不把尚未接入的音频或录屏能力标为可用。更新下载仍等待
实际更新服务提供状态和动作，关于页面不会以占位链接启用下载。

## 配置、便携目录与会话范围

应用名称为小写 `huhucapture`，不设置额外 organization name，避免改变旧
Windows 配置路径。Windows 默认配置使用 Qt 的 `AppDataLocation`，对应
Roaming AppData；`AppConfigLocation` 在固定 Qt 的 Windows 实现中属于
Local AppData，不可互换。已存在的 EXE 旁 `config.json` 仍按
`SettingsStore` 的 Windows 兼容规则优先使用。Linux 使用实际用户的
`AppConfigLocation` 和 `AppDataLocation`。

显式便携参数为 `--portable-dir=/absolute/path`，Windows 可使用绝对盘符路径。
该参数只选择配置及私有资源目录，不移动已有媒体，也不把截图默认目录改到
程序旁。空值、相对目录、含 NUL 或换行的路径、已存在的非目录以及 AppImage
只读挂载目录会明确失败。配置路径经过同一物理路径规范化后传给存储和实例
协调器，别名路径不会建立两个独立配置 scope。

AppImage 存在 `APPIMAGE` 环境值时，自启服务收到的是外层可执行文件路径。
提供但不合法的值仍交给真实能力检查报告错误，不能悄悄把挂载目录里的内部
二进制注册为自启。初始化只读取系统现状；持久化的 `autoStart` 字段不会在
普通启动时擅自修改操作系统任务或桌面文件。

Windows 实例范围包含规范化配置路径、真实进程会话 ID、window station 和
当前线程 desktop。窄平台适配器只读取系统提供的信息，不切换桌面、不关闭
借来的 station/desktop 句柄。Windows 本地管道仍采用当前用户权限。
Linux 范围包含配置物理路径及实际 `XDG_SESSION_ID`、`WAYLAND_DISPLAY` 和
`DISPLAY`，端点位于经过 Qt 检查的用户 runtime 内，不回退到不受限的全局
socket 命名空间。完整锁和投递语义见 [启动与单实例](qt-startup.md)。

路径选择依据已阅读的固定 Qt 6.12.1
[Windows 实现](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/io/qstandardpaths_win.cpp)
与 [Unix 实现](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/io/qstandardpaths_unix.cpp)。
这是对 Qt 公共接口的直接复用，没有复制或维护第二套系统目录实现。

## 启动行为与无托盘桌面

生产入口和测试使用同一个 `parseApplicationArguments`，内部继续调用共享的
`Core::parseStartupArguments`。未知参数、重复值、裸布尔标志及大小写保留
原产品规则，不用一个严格参数解析器无意改变已有启动脚本。

| 启动方式 | 应用行为 |
| --- | --- |
| 默认启动或普通转发 | 进入截图；已有兼容会话可被重新激活，忙碌且不能接管时明确拒绝 |
| `--auto-start` 或 `--enter=tray` | 常驻后台，不自动截图 |
| `--auto-quit` | 独立实例，优先于后台参数；完成真实采集/编辑/输出旅程后尝试退出 |
| 已有实例明确接受 | 新进程直接结束，不把 IPC 确认描述为截图或保存成功 |
| 截图能力不可用 | 显示实际失败，等待用户确认后按当前启动角色继续或退出 |

托盘使用同一组 Qt 菜单动作：截图、历史、设置、局域网和退出。系统托盘不可用
时显示一个共享的可见入口，用户仍能打开已有功能窗口并退出。托盘服务运行中
消失也会显示该入口，不让应用留在用户无法访问的隐藏状态。

从入口启动截图时先隐藏入口窗口；若请求不能接管则恢复它。截图旅程结束且
桌面没有托盘时恢复入口，不主动抢回焦点。普通窗口激活使用已有
`WindowActivation`；全局快捷键提供的 Wayland activation token 只沿截图
请求传递，不写入配置、日志或全局环境。

独立模式的完成条件不只是选区窗口关闭。宿主等待截图、录制、媒体、剪贴板和
文件定位实际空闲，错误已经确认，并且编辑、贴图或二维码等旅程窗口不再可见。
保留中的编辑/贴图窗口仍可使用，不因截图状态已经结束而被自动销毁。

## 正常退出顺序

退出进入不可逆的排空阶段，停止接受新单实例动作，但保留主锁直至所有服务
对象销毁。设置与局域网可以并行停止；录制必须先完成自己的 finalize 或保留
恢复文件，之后截图服务才能关闭活动来源和视图，防止截图取消先触发录制丢弃。

1. 停止新动作、托盘操作和新的窗口激活，发起设置与局域网关闭。
2. 等待 `RecordingWorkspace::shutdownFinished`，再关闭截图和媒体工作区，
   取消宿主已跟踪的目录打开请求。
3. 等待设置、局域网、录制和截图真实关闭回执，以及媒体和文件定位 worker
   实际不再忙碌。
4. 调用唯一 `SystemClipboard::shutdown()`，等待它的实际
   `shutdownFinished`，包含已经取消但仍未完成的校验 future 和回执监听者。
5. 显示尚未确认的错误，处理 Linux 剪贴板所有权，最后才发出 `exitReady`。

整个正常过程由事件和回调驱动，不在 GUI 线程阻塞等待工作池。对象析构中的
最后防线不能代替上述屏障。局域网关闭、媒体写入失败或未知持久化结果仍使用
各服务原有错误与恢复合同，不因为应用正在退出就改报成功。

## Linux 剪贴板仍由本进程持有时

`shutdownFinished` 证明服务已排空，不证明桌面永久保存了复制内容。固定 Qt
的 X11 `SAVE_TARGETS` 析构流程没有公开成功回执，Wayland 普通数据源也没有
通用的永久接管确认；来源与服务细节见 [剪贴板生命周期](clipboard-lifecycle.md)。

宿主如果在排空后仍是 `Clipboard` 的所有者，会显示同一个入口窗口的
“保留以便粘贴”和“退出”动作，并明确说明退出可能使内容不可用。选择保留时
窗口保持可见，Qt 的真实数据源继续响应其他进程。选择退出才释放最终退出
屏障；若另一个实际所有者替换了剪贴板，宿主也可以完成退出。

这适用于一次性模式与常驻模式的最终退出，不把一次性实例偷偷变成隐藏后台
服务。Windows 已有原生发布使用系统拥有的剪贴板数据，不需要这项 Linux
持有窗口。该差异是实际系统所有权约束，不代表已经验证退出后的管理器接管。

## 构建和实际验收范围

完整应用目标要求 Qt、desktop services、同 CRT 的单一 Rust 静态库和完整媒体
检查；Linux 还需要固定 Portal、PipeWire 与 FFmpeg 依赖。Windows 的 Rust
组合使用 Release/RelWithDebInfo 的 `/MD`，不屏蔽 CRT 冲突伪造 Debug 兼容。
唯一版本源和资源中的应用名称由现有构建系统提供，宿主不另存一份发行版本。

`qt_application_lifecycle` 链接生产宿主，执行实际 CLI、真实配置文件、损坏
配置保护、一次性不启动常驻服务、真实工作池与租约排空。默认还启动独立真实
进程，检查 Primary/Forwarded/Independent、无托盘可见入口、现有设置窗口
复用和最终退出顺序。它使用实际不可用的 `offscreen` 后端验证失败路径，不
把该结果描述为截图成功，也不伪造 IPC、平台能力或工作完成回调。

Linux 的 `qt_application_clipboard_owner` 在实际 X11 server 下运行相同宿主，
通过生产剪贴板服务发布完整 Unicode 文本，排空后选择可见保留，再由另一个
真实 Qt 进程读取并核对内容，最后点击退出。该测试只证明保留窗口打开期间的
真实接收，不宣称进程结束后的持久保存。

本地 Linux amd64 已用固定 Qt 6.12.1 SDK、实际原生聚合 Rust 库和完整生产依赖
完成程序与生命周期测试的 Release 编译、链接；实际运行了 `--no-ipc` 的配置、
能力拒绝和异步排空分支，并检查其错误窗口 PNG 没有截断。该显式本地模式不
执行跨进程 socket 验收，CI 的默认命令保留完整 IPC，不带这一跳过参数。
本地受限环境没有可用 X server，不能据此宣称原生输入、托盘、X11 剪贴板持有
或 Wayland 桌面授权通过。

Windows 完整应用构建、双端默认 IPC 测试和 X11 持有测试的结果以实际 CI 记录
为准。真实双屏、混合 DPI、物理快捷键、IME、桌面剪贴板管理器、实体两机互传、
录制音源、长期运行、安装升级卸载和性能门槛仍需各自的实际证据；此宿主测试
不替代它们。
