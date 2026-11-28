# Qt 迁移的代码与验证范围

[Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5) 定义统一迁移计划，[Issue #4](https://github.com/NAMEWTA/huhucapture/issues/4) 补充 Linux 和 LocalSend。当前生产定义使用 CMake，在 Windows 和 Linux 装配同一个 Qt 应用。

Windows 保留 Win32 采集及 Media Foundation / WASAPI，Linux 使用 X11 或 Portal / PipeWire。两端共用编辑器、媒体、配置、更新和 LocalSend 界面，通过本平台适配请求系统能力。旧 Ling 单 EXE 工程保留在 [v0.0.6 源码标签](https://github.com/NAMEWTA/huhucapture/tree/v0.0.6)。

## 已接入的公共边界

| 路径 | 当前职责 | 需要继续完成的部分 |
| --- | --- | --- |
| `src/core/` | 无 Qt / Win32 的像素、批注排版、活动历史、归档命名、录制结果和时间合同 | 会话、能力与跨屏坐标的完整产品合同 |
| `src/app/QtMain.cpp`、`DesktopApplication.*`、`SingleInstance.*` 与 `src/core/Startup.h` | 唯一 QApplication、托盘和会话装配；既有 CLI 语义、非阻塞本地动作转发、活锁/崩溃/不确定投递边界；旧产品复用启动规则 | 实际桌面启动、无托盘及协调退出的完整用户旅程 |
| `src/editor/` 的模型与 `EditorDocument.h` | 共用几何、拖动、文字、编号、批注、工具参数与唯一活动历史，已接入截图及贴图；原 Windows 工具直接复用 | 实体桌面交互、来源变换和混合 DPI 验收 |
| `src/ui/qt/Editor*` | 共用九工具画布、单一文字输入、完整批注面板、原图马赛克及带令牌的异步输出请求，连接共享归档 / 剪贴板 / 贴图服务 | 真实 IME、混合 DPI 和完整编辑输出旅程 |
| `src/services/rendering/` | 编辑器共用 Qt 字体测量、完整备注排版和离屏合图，保留原图像素与失败时旧输出 | 真实桌面字体、长备注与 IME 验收 |
| `src/services/config/` | UTF-8 / UTF-16 JSON、未知字段保留、首次迁移备份、路径选择与原子替换；设置控制器接入运行时效果、自启和快捷键回滚 | 干净机器与升级后的实际配置迁移、系统操作及授权流程 |
| `src/services/localization/` | 六份原语言、同一 BOM 解析器、英语回退、目录优先级和缓存刷新，已接入窗口语言切换 | 真实窗口长译文、字体和 IME 验收 |
| `src/services/media/` | 受管命名、PNG / GIF / MP4 校验、流式导入、身份租约、缩略图缓存及持久化结果，接入历史与共享输出 | 真实媒体和桌面故障旅程、恢复流程及性能验收 |
| `src/services/desktop/` | 统一宿主接入剪贴板准备/发布、回收/第二次永久删除许可和异步文件定位；Windows 旧入口共用原生实现 | 双平台真实桌面复制、回收和文件管理器交互验收 |
| `src/services/transfer/` 与 `src/ui/qt/Transfer*` / `ReceivePrompt` | 有界 Qt actor、独立接收 worker、完整验证归档、证书确认与进度 UI；LanController 装配持久身份、网卡和发现，任务状态不依赖窗口寿命 | 身份持久化与故障流程持续验证、物理双端互传及性能门槛 |
| `src/platform/FileCommitter.h` | 图片、录制及接收导入共用提交结果、受控暂存所有权和无覆盖发布合同 | 真实文件系统故障、设备移除、持久化状态未知与恢复旅程 |
| `src/platform/windows/` | Qt 应用接入原生文件发布、GDI 采集、MF/WASAPI 录制和桌面边界；原 Windows 归档使用共享提交合同 | 真实显卡 / 音源、录制全生命周期和物理窗口交互验收 |
| `src/platform/linux/portal/` | libportal 截图 / ScreenCast / GlobalShortcuts 控制面、取消、绑定替换及 FD 所有权，真实总线断开处理；KWindowSystem 异步父窗导出和 ScreenCast 采流已通过受控 Sway 检查 | 真实 GNOME / Plasma 授权与物理桌面验收 |
| `src/core/MediaFrame.*` 与 `src/platform/linux/pipewire/` | 无 Qt 的有界帧池、跨线程只读 lease、旧 MemFd 描述符兼容映射；Debug / Release 已通过真实 Sway 采流、resize、daemon-loss 和停止后帧保留检查 | GNOME / Plasma、物理显示器、实际音源及长时录制验收 |
| `third_party/localsend/` 与 `src/platform/rust/` | 固定上游 core 与 gifski 的单一 Rust 静态库、C ABI、真实 TLS / 文件流与受控关闭，接入 Qt 设备 / 任务服务和媒体完整校验 | 物理两端与官方客户端互操作；聚合 ABI 和一致锁已通过指定双平台 CI |

同一工具的交互规则不能因为平台迁移而被“统一”成不同语义。例如普通矩形越过对边时可以翻转，批注范围则限制在原图内；普通序号按活动最大值加一，批注高水位编号永不复用。移动、文字与样式修改仍遵循现有历史语义，不借模型抽取改变撤销行为。

## 配置与媒体保护

`SettingsStore` 读取旧格式时保留未知字段。UTF-16 LE / BE BOM、UTF-8 BOM 和严格 UTF-8 使用一个解码入口；坏编码、坏 JSON、非对象根和未知未来 schema 不会被默认配置覆盖。原有空便携文件仍可启用配置，第一次写入前也保存原始字节备份。旧默认 `Ctrl+Alt+A` 仅在旧 schema 迁移时转为 F1，新 schema 中明确设置的同一按键保留。

Windows 已存在的 EXE 旁配置优先；Linux 默认使用调用方提供的实际用户配置目录，便携目录必须明确指定，不把 AppImage 的只读挂载目录作为配置目录。规范化后的物理路径同时用于 `QLockFile` 和 `QSaveFile`，避免两个路径别名使用不同写锁。写前比较读取时的原始字节，其他进程已修改或删除目标时返回冲突，不覆盖它。

显式导入只读取用户选定的文件，不扫描其他磁盘。先备份源文件和旧目标，再原子替换目标；同一内容重复导入保持用户后续修改，仍检查目标是否被外部改变。导入自身、硬链接或符号链接别名不改写源文件。

普通配置写入使用 `QSaveFile` 的原子替换，关闭 direct-write fallback。这里不把原子替换宣传为断电情况下的严格持久化保证：Qt 的该接口没有完整提供媒体提交所需的文件与目录同步结果。配置迁移备份则使用 `FileCommitter`，只有备份明确成功后才继续覆盖配置。

备份写入复用平台的空文件身份检查和受控文件句柄，不以路径重新打开并截断文件。写入、flush 和关闭均须成功，发布前再次核对文件身份与长度；失败清理只针对仍有相同身份的暂存文件。保存配置在备份及新内容写入后、原子替换前再次比较原始快照，检测这段时间内的外部修改，并保留已经成功的备份。该检查与替换之间不是跨进程原子比较交换；受支持的并发应用写入仍由同一物理路径的 `QLockFile` 协调。

媒体提交明确区分未提交、已提交、以及已发布但持久化状态未知。发布之后的清理错误不能把最终文件当临时文件删除。Linux 使用同文件系统的无覆盖硬链接发布并同步文件与目录，文件系统不支持时返回失败；Windows 使用独占暂存和无覆盖移动。该合同只接受受控暂存命名空间，不声称能隔离同一用户任意修改应用私有目录的攻击。接收端的类型和完整媒体解码检查仍属于更上层的导入服务。

## 验证与交付

产品验证覆盖共享契约、实际平台进程、媒体、受控桌面和完整安装树。正式包执行同源构建、四种包安装测试、来源闭合和旧桥下载。旧桥的原生 About 探针从 v0.0.6 固定源码构建，使用当前聚合包。

实体设备、混合 DPI、系统输入法、物理音源、长时录制及干净机器跨版本升级的人工验证状态应单独记录。具体入口见 [构建与发布](building.md) 和 [验证指南](validation.md)。

## 平台支持与验证口径

v0.0.6 及更早版本提供 Windows 10 1803 的原生单 EXE。Qt 6.12.1 迁移采用 Windows 10 **1809** 及以上 x64；新 Qt 包不能标为支持 1803，旧系统仍需使用旧版本。Linux 目标为 amd64，当前 Ubuntu 24.04 CI 已有受控安装旅程证据；最终发行基线、干净机器和桌面支持范围仍须经 P00 / P11 / P12 验证后公布。

`qt_image_contract` 验证真实 Qt 像素与导出边界，Windows 另链接保留的原生媒体后端；`qt_annotation_render` 写出完整 PNG 并检查字体和像素；`qt_settings_store` 操作真实文件系统；`windows_audio_format` 用保护页验证格式读取边界。Linux 的 Portal 故障测试使用真实 D-Bus 和不存在的服务，证明错误、取消、晚到回调与释放，不替代授权对话框或 PipeWire 帧采集。

Windows 的字体测试使用原生 `windows` Qt 平台插件；Linux 的离屏文字测试安装实际中文字体。编辑器组件测试创建真实 Qt 控件并直接驱动受控输入，不等同于物理键鼠或系统 IME。Windows `offscreen` 的占位字体引擎不能作为中文字体覆盖证据。PNG 只证明已执行的导出场景，不能证明中文输入法或跨屏拖动。

当前执行环境不能承担实体双屏、音源四组合、GNOME / Plasma 授权、长时录制和空白机器安装验收。这些项保持未验证，并在对应 PR / Issue 留出明确状态。发行结果绑定具体提交和完整资产，人工项目按实际执行范围记录。

## 固定依赖与复用记录

实际已接入的验证版本集中在 [依赖锁](../build/dependencies.lock.json)，旧工具链记录在 v0.0.6 标签。Qt 使用匹配的 v143 和动态 CRT；C++ 组件分别验证 `/MD` 与 `/MDd`。固定 Rust 标准库使用 release CRT，所以聚合 ABI 目标只接受 Release / RelWithDebInfo 的 `/MD`，不通过屏蔽链接冲突伪装为 Debug 兼容。发行清单记录原始包、对应源码和部署证据。

LocalSend 源码来源、逐文件 blob、改动补丁与许可见 [LocalSend core](localsend-core.md)。libportal 的来源、补丁范围、构建与真实桌面 probe 见 [Linux Portal](linux-portal.md)。其他原生实现延续 [上游致谢](upstream.md) 中记录的归属。Qt、gifski、libportal、FFmpeg / x264 各有自己的许可条件；仓库根 MIT 许可不等于最终组合二进制只有 MIT 条款。
