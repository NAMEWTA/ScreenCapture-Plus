# Qt 跨平台迁移状态与边界

[Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5) 是唯一主实施计划，包含 P00–P14 的依赖、F01–F15 功能合同及发布验收。[Issue #4](https://github.com/NAMEWTA/huhucapture/issues/4) 仅补充 Linux 与局域网协议依据。本页记录代码已经承担的职责和仍然存在的门槛，不另建一份与 Issue 竞争的实施方案。

## 当前构建是什么

当前正式版本仍是 v0.0.5 的 Windows 产品。根目录 CMake 已提供共享 C++20 模型、Qt 服务、原生适配和实际 `huhucapture` 程序目标。`HUHUCAPTURE_BUILD_APP=ON` 装配同一套启动、托盘、截图、编辑、历史、设置、录制与局域网服务；双平台已完成完整目标及 `BUILD_TESTING=OFF` 生产配置的真实编译链接，Linux 开发 DEB、AppImage、符号与校验文件已由同源流水线生成。**该宿主仍在功能集成中，不是已通过 P12 的完整产品或正式安装包**。集成测试发现的问题仍须修复重验，组件、链接或打包成功不能替代真实桌面、音源、两端互传、安装升级和性能验收。

迁移目标是一个 C++20 / Qt 6 Widgets 产品，两端使用同一套界面、编辑器状态与应用服务。平台目录只处理真实系统差异。现有 Windows 原生采集、Media Foundation 和 WASAPI 继续复用；Linux 的 Portal 控制面复用固定 libportal，局域网协议复用固定 LocalSend Rust core。

按照 Issue #5 的 P13 依赖，只有 P12 全量验收通过后才删除 Ling、旧生产工程及旧 UI。过渡期间修改共享规则时，现有 Windows 产品必须实际调用它们并继续通过回归，不能另复制一套规则等待最后切换。

## 已接入的公共边界

| 路径 | 当前职责 | 需要继续完成的部分 |
| --- | --- | --- |
| `src/core/` | 无 Qt / Win32 的像素、批注排版、活动历史、归档命名、录制结果和时间合同 | 会话、能力与跨屏坐标的完整产品合同 |
| `src/app/QtApplication.*`、`SingleInstance.*` 与 `src/core/Startup.h` | 同一 QApplication / 托盘 / 可见后备入口、配置和服务所有者；既有 CLI、非阻塞动作转发、录制/滚动/更新接线及真实关闭屏障；旧产品复用启动规则 | 完整双端 IPC / 托盘、真实桌面启动与退出旅程 |
| `src/services/desktop/DesktopCaptureAction.*` | 已安装 GNOME 完成动作的真实 GIO 查找、进程/既有 IPC 验证及注册变化撤回；共享录屏/长截图继续拥有停止业务 | 正向子进程 IPC、真实 GNOME 菜单可达性及无托盘完整采集验收 |
| `src/editor/` 的模型与 `EditorDocument.h` | 共用几何、拖动、文字、编号、批注、工具参数与唯一活动历史；原 Windows 工具和 Qt 工作区直接复用 | 完整产品的物理输入与长期会话验收 |
| `src/ui/qt/Editor*` 与 `MediaWorkspace` | 共用九工具画布、单一文字输入、完整批注面板、原图马赛克及带令牌的异步归档 / 剪贴板 / 贴图输出；双平台组件构建与受控交互测试 | 真实 IME、混合 DPI、完整桌面字体和导出旅程 |
| `src/services/rendering/` | Qt 字体测量、完整备注排版和离屏合图，保留原图像素与失败时旧输出；共享编辑器使用同一导出路径 | 真实桌面字体 / IME 与大图资源验收 |
| `src/services/config/` 与 `SettingsController` | UTF-8 / UTF-16 JSON 读取、未知字段保留、首次迁移备份、路径选择与原子替换；同一设置页接入真实自启、快捷键和运行时状态 | X11 映射、Portal 实际授权、自启和配置迁移的完整桌面验收 |
| `src/services/localization/` | 六份原语言、同一 BOM 解析器、英语回退、目录优先级和缓存刷新 | 正式窗口语言切换、长译文与 IME 验收 |
| `src/services/media/` | 共用受管命名、PNG / GIF / MP4 完整检查、流式导入、身份租约、缩略图、历史装配及持久化结果；真实 X11 / PipeWire 来源与固定 FFmpeg / x264 / AAC 编码已接入；Windows MP3 容器样本与解析帧分开核对 | 真实音源四组合、长时同步、新增严格 MP3 库补丁的双端完整回归 |
| `src/services/desktop/` | 共用剪贴板准备/发布及排空、回收/第二次永久删除许可、异步定位；宿主和 Windows 旧入口复用同一实现 | Linux 真实剪贴板接管、桌面文件管理器、退出持有和物理交互验收 |
| `src/services/transfer/`、`LanController` 与 `src/ui/qt/Transfer*` / `ReceivePrompt` | 有界 Qt actor、真实持久身份和网卡装配、独立接收 worker、完整校验与归档、设备 / 证书确认 / 进度 UI及窗口重开快照 | 物理双端、完整产品操作旅程与性能门槛 |
| `src/platform/FileCommitter.h` | 同一份提交结果和暂存所有权合同 | 所有图片、录屏及接收导入统一使用，文件 lease 和恢复界面 |
| `src/platform/windows/` | 原生文件发布、采集和音频格式边界；原 Windows 归档已使用共享提交合同 | 完整 Windows 能力从 Ling 解耦、Qt 线程和窗口集成 |
| `src/platform/linux/portal/` | libportal 截图 / ScreenCast / GlobalShortcuts 控制面、取消、绑定替换及 FD 所有权，真实总线断开处理；指定 Sway 父窗生命周期和实际授权采流已验证；同一会话的数据连接刷新已接入 | 新刷新首帧、快捷键活动会话排空及真实 GNOME / Plasma 授权验收 |
| `src/core/MediaFrame.*` 与 `src/platform/linux/pipewire/` | 无 Qt 的有界帧池、跨线程只读 lease 与 CPU / MemFd 数据面，固定客户端和受控失败路径补丁；真实 Sway 采流、尺寸变化和服务断开已通过 Debug / Release 探针 | 新 Wayland 刷新像素时序、GNOME / Plasma 和物理音源验收 |
| `third_party/localsend/` 与 `src/platform/rust/` | 固定上游 core 与 gifski 的单一 Rust 静态库、C ABI、真实 TLS / 文件流与受控关闭；共享 Qt 设备 / 任务服务及完整媒体检查已装配 | 聚合 ABI、真实取消和同进程 Qt 控制层已通过指定双平台 CI；物理双端和完整产品旅程尚未验收 |

同一工具的交互规则不能因为平台迁移而被“统一”成不同语义。例如普通矩形越过对边时可以翻转，批注范围则限制在原图内；普通序号按活动最大值加一，批注高水位编号永不复用。移动、文字与样式修改仍遵循现有历史语义，不借模型抽取改变撤销行为。

## 配置与媒体保护

`SettingsStore` 读取旧格式时保留未知字段。UTF-16 LE / BE BOM、UTF-8 BOM 和严格 UTF-8 使用一个解码入口；坏编码、坏 JSON、非对象根和未知未来 schema 不会被默认配置覆盖。原有空便携文件仍可启用配置，第一次写入前也保存原始字节备份。旧默认 `Ctrl+Alt+A` 仅在旧 schema 迁移时转为 F1，新 schema 中明确设置的同一按键保留。

Windows 已存在的 EXE 旁配置优先；Linux 默认使用调用方提供的实际用户配置目录，便携目录必须明确指定，不把 AppImage 的只读挂载目录作为配置目录。规范化后的物理路径同时用于 `QLockFile` 和 `QSaveFile`，避免两个路径别名使用不同写锁。写前比较读取时的原始字节，其他进程已修改或删除目标时返回冲突，不覆盖它。

显式导入只读取用户选定的文件，不扫描其他磁盘。先备份源文件和旧目标，再原子替换目标；同一内容重复导入保持用户后续修改，仍检查目标是否被外部改变。导入自身、硬链接或符号链接别名不改写源文件。

普通配置写入使用 `QSaveFile` 的原子替换，关闭 direct-write fallback。这里不把原子替换宣传为断电情况下的严格持久化保证：Qt 的该接口没有完整提供媒体提交所需的文件与目录同步结果。配置迁移备份则使用 `FileCommitter`，只有备份明确成功后才继续覆盖配置。

备份写入复用平台的空文件身份检查和受控文件句柄，不以路径重新打开并截断文件。写入、flush 和关闭均须成功，发布前再次核对文件身份与长度；失败清理只针对仍有相同身份的暂存文件。保存配置在备份及新内容写入后、原子替换前再次比较原始快照，检测这段时间内的外部修改，并保留已经成功的备份。该检查与替换之间不是跨进程原子比较交换；受支持的并发应用写入仍由同一物理路径的 `QLockFile` 协调。

媒体提交明确区分未提交、已提交、以及已发布但持久化状态未知。发布之后的清理错误不能把最终文件当临时文件删除。Linux 使用同文件系统的无覆盖硬链接发布并同步文件与目录，文件系统不支持时返回失败；Windows 使用独占暂存和无覆盖移动。该合同只接受受控暂存命名空间，不声称能隔离同一用户任意修改应用私有目录的攻击。接收端的类型和完整媒体解码检查仍属于更上层的导入服务。

## 工作包状态

这些状态描述集成范围，不将已经落地的个别文件当作整个工作包验收通过。

| 工作包 | 当前状态 | 尚未解除的主要门槛 |
| --- | --- | --- |
| P00 | 实施与验证中；Qt / gifski / TLS 同进程已在双平台通过 | 完整原生媒体组合、Portal 三类接口实际授权、Linux 音源与许可分发、固定机器性能基线 |
| P01 | CMake、唯一产品版本源、实际共享 Qt 宿主、生产/测试分离与双平台 CI 已接入 | 完整固定恢复、同源安装链实际验收 |
| P02 | 共享设置 / 历史 / 关于窗口已进入真实组件测试 | Linux 桌面完整交互、真实弹层与混合 DPI 拖动 |
| P03 | 九工具模型、工具参数、马赛克与 EditorDocument 已由旧产品和 Qt 编辑器共用 | 会话及正式 Qt 产品集成 |
| P04 | 原生媒体 / 文件边界解耦中 | 截图、热键、窗口 / 光标、COM 与录制全生命周期 |
| P05 | Portal 控制面实施与验证中 | X11、PipeWire、全局键及真实桌面能力矩阵 |
| P06 | 配置、语言和文件发布合同已实现 | QR / 回收 / 剪贴板 / 缩略图 / lease 的完整迁移 |
| P07 | 同一 Qt 宿主、CLI / 单实例 / 托盘、设置 / 历史 / 关于、截图 / 编辑输出与完整包更新已装配，未验收 | 真实双平台完整旅程、字体 / IME / DPI |
| P08 | 旧产品和共享滚动会话共用长截图模型；原生持续帧源、授权源和控制窗口已装配 | 真实滚动、窗体遮挡、Wayland 手动滚动与完整输出验收 |
| P09 | 共用录制会话 / 工具条、Windows 原生后端、X11 / 授权 PipeWire 消费者及真实音频适配已接入并分层验证 | 控件排除、真实音源四组合、长时同步、完整原生时间线与恢复 |
| P10 | 上游 core、聚合 C ABI、真实 TLS 归档和取消、共享任务 / 设备 UI 与应用身份 / 网卡装配已接入；聚合验证双端通过 | 完整应用身份与界面回归、P10a 物理双平台互传；P10b 独立记录 |
| P11 | 完整包更新、旧 Windows 流式桥接、实际安装树与四种开发包流水线已接入；旧桥接原生受控检查、Linux 两包安装/重装/移除及数据保护通过 | Windows 两包安装生命周期、真实完整包跨版本升级、干净机器卸载和对应源码审核 |
| P12 | 未验收 | F01–F15、真实双屏 / IME / 音视频、性能、稳定性和干净机器升级卸载 |
| P13 | 等待 P12 | 删除旧生产架构后从干净检出重新验证 |
| P14 | 文档随实现更新，未进入发布 | 许可证 / 对应源码与同 SHA 全部发行资产、必要验收 |

应用所有权、启动和退出屏障见 [共享 Qt 宿主](qt-application.md)。截图与录屏的接线见 [共享工作区](capture-workspaces.md)，来源与坐标约束见 [采集服务](screen-capture.md) 和 [授权实时源](live-capture-source.md)，长截图的保留规则见 [共享长截图模型](scroll-capture.md)，软件编码和 AAC 尾样本合同见 [Linux 录制](linux-recording.md)。[完整包更新](qt-updates.md)、[同源打包](qt-packaging.md) 和 [实际安装生命周期](installation-validation.md) 分别记录下载、部署与运行验证边界。这些实现尚不代表整个工作包已经验收。

提交 `11c6033e51a26cd8dda5a403da239ccd5953fe72` 的 [Qt 验证](https://github.com/NAMEWTA/huhucapture/actions/runs/45629021964) 已完成：Windows Debug / Release 各 66 项、Linux 各 75 项通过。真实锁定 Sway / Portal / PipeWire 的两个配置均收到并逐像素核对实际 MemFd 帧，完成尺寸变化、服务断开、缓冲归还和共享来源排空。[Windows / Linux 聚合运行](https://github.com/NAMEWTA/huhucapture/actions/runs/45629019877) 也通过。这些结果确认已有采流与释放路径，不代替新加入的隐藏控件后首帧刷新探针，也不覆盖 GNOME / Plasma 或物理桌面。

加入刷新和更完整关闭门槛后的 `24f94d403acfe82212c955729e6e34de16bce5b4` [Qt 运行](https://github.com/NAMEWTA/huhucapture/actions/runs/45637885852) 中，Windows 两配置各 67 项通过，Linux Debug 79 项与直接 PipeWire 三个场景通过；随后共享来源探针在刷新后的第一消费帧像素断言失败。该 Linux job 失败，新的专用刷新探针和 Release 后续检查没有执行。旧配置通过不能覆盖这个新失败，首帧门槛保持不变，实际原因仍须结合失败帧与原生日志验证。

同一 `24f94d4` 的 [完整应用运行](https://github.com/NAMEWTA/huhucapture/actions/runs/45637885695) 中，Linux 93 项、Windows 79 项集成检查通过，两平台完整应用及 `BUILD_TESTING=OFF` 生产配置均实际编译、安装运行时并通过原生 TLS 检查。Linux 六种真实 PipeWire 音频图通过：系统、麦克风、混音和限幅混音各处理 144,000 个样本帧；服务/来源断开均按故障合同退出，六种场景 FD 增量与清理错误均为零。这是实际隔离音频图的短程验证，`physicalDevices` 与 `longRunningAvDrift` 均未验证。Windows 生成的原 H264 / MP3 文件也由独立 Linux job 按原始字节完整解码通过。

Linux DEB 和 AppImage 已完成真实产品拖选、像素检查、正常退出、同包重装和移除，并保留原配置与媒体字节；详见[安装证据边界](installation-validation.md)。AppImage 使用官方免 FUSE 路径，不能据此声明 FUSE 挂载或跨版本升级通过。Windows 材料校验遇到 Qt 官方多配置 SPDX 的同路径重复记录，NSIS 生成器另有 `/oname` 参数引号错误；该轮安装器与安装旅程未通过。两项已据原始产物和固定官方实现修正，仍待下一轮原生 CI。

该提交的[原生聚合运行](https://github.com/NAMEWTA/huhucapture/actions/runs/45637885748)和[保留的 Windows 产品回归](https://github.com/NAMEWTA/huhucapture/actions/runs/45637885799)通过。独立[视频验证](https://github.com/NAMEWTA/huhucapture/actions/runs/45637885690)中 Linux 两配置通过，Windows Debug 的视频/HEVC 检查通过，但真实录制尾时间线断言失败，Release 未执行；该失败仍需修复重验，不能被完整应用的另一场录制通过覆盖。

## 平台支持与验证口径

v0.0.5 的下载、单 EXE 与 Windows 10 1803 支持说明只适用于旧产品。Qt 6.12.1 迁移目标采用 Windows 10 **1809** 及以上 x64；新 Qt 包不能标为支持 1803，旧系统仍需使用旧版本。Linux 目标为 amd64，当前 Ubuntu 24.04 CI 已有受控安装旅程证据；最终发行基线、干净机器和桌面支持范围仍须经 P00 / P11 / P12 验证后公布。

`qt_image_contract` 验证真实 Qt 像素与导出边界，Windows 另链接保留的原生媒体后端；`qt_annotation_render` 写出完整 PNG 并检查字体和像素；`qt_settings_store` 操作真实文件系统；`windows_audio_format` 用保护页验证格式读取边界。Linux 的 Portal 故障测试使用真实 D-Bus 和不存在的服务，证明错误、取消、晚到回调与释放，不替代授权对话框或 PipeWire 帧采集。

Windows 的字体测试使用原生 `windows` Qt 平台插件；Linux 的离屏文字测试安装实际中文字体。编辑器组件测试创建真实 Qt 控件并直接驱动受控输入，不等同于物理键鼠或系统 IME。Windows `offscreen` 的占位字体引擎不能作为中文字体覆盖证据。PNG 只证明已执行的导出场景，不能证明中文输入法或跨屏拖动。

当前执行环境不能承担实体双屏、音源四组合、GNOME / Plasma 授权、长时录制和空白机器安装验收。这些项保持未验证，并在对应 PR / Issue 留出明确状态。所有必要发布门槛完成以前，不修改正式发布声明、不关闭实施 Issue、不生成声称已经交付双平台产品的正式 Release。

## 固定依赖与复用记录

实际已接入的验证版本集中在 [依赖锁](../build/dependencies.lock.json)，不覆盖仍服务于旧产品的工具链。Qt 使用匹配的 v143 和动态 CRT；C++ 组件分别验证 `/MD` 与 `/MDd`。固定 Rust 标准库使用 release CRT，所以聚合 ABI 目标只接受 Release / RelWithDebInfo 的 `/MD`，不通过屏蔽链接冲突伪装为 Debug 兼容。旧产品的 v145 / `/MT` 在 P13 前保留原验证入口。Qt SDK 资产与最终部署工具的完整哈希清单仍是 P00 待完成项。

LocalSend 源码来源、逐文件 blob、改动补丁与许可见 [LocalSend core](localsend-core.md)。libportal 的来源、补丁范围、构建与真实桌面 probe 见 [Linux Portal](linux-portal.md)。其他原生实现延续 [上游致谢](upstream.md) 中记录的归属。Qt、gifski、libportal、FFmpeg / x264 各有自己的许可条件；仓库根 MIT 许可不等于最终组合二进制只有 MIT 条款。
