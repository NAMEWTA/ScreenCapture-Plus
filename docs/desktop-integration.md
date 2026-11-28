# 系统剪贴板、回收与文件定位

本模块实现 Issue #5 P06/F11 的桌面服务边界。编辑器、贴图和历史窗口共享
`services/desktop/`，宿主应用负责工作线程调度、归档顺序、语言文案与视图生命期。
接口存在不表示应用装配或 P06 验收已经完成；实际运行证据由 CI 与
[验证边界](validation.md) 维护。

## 所有权与线程

| 接口 | 调用位置 | 所有权与实际完成条件 |
| --- | --- | --- |
| `prepareImage` | 工作线程 | 持有独立像素，编码真实 PNG；Windows 同时准备 DIBV5、DIB |
| `prepareFile` | 工作线程 | 接收历史读租约；PNG 复用 `inspectPng(..., decoded)` 一次完整校验和解码，GIF/MP4 准备真实文件路径 |
| `prepareText` | 工作线程 | 持有独立的完整 Unicode 文本；Windows 准备 `CF_UNICODETEXT`，Linux 准备 UTF-8 MIME 内容 |
| `SystemClipboard::publish` | GUI 线程 | 最后一次文件身份核对仍由工作线程执行；只在平台实际提交或确认后完成 |
| `recycle` | 工作线程 | 按已确认 `Entry` 取得独占删除租约并再次检查 `unchanged()` |
| `permanentlyRemove` | 工作线程 | 只接受此前真实回收失败所生成、经第二次用户确认的 permit |
| `FileRevealer` | GUI 线程入口 | 文件检查、目录创建、Windows Shell 调用在工作线程；Linux DBus/GIO 使用异步完成回调 |

`SystemClipboard` 和 `FileRevealer` 属于应用服务，不能作为临时窗口的子对象。
关闭历史、编辑器或贴图只解除该视图的订阅。所有非零请求 token 在服务仍存活时
只有一个终态通知；旧请求、取消后的晚回调不能完成新的请求。

`PreparedClipboard` 是一次性对象。发布失败、取消或完成后主动释放其准备内存
和历史读租约，即使调用者仍持有该对象。Qt 接管的图像数据继续有效；文件 URI
不携带历史租约，因此持续保留一条剪贴板 URI 不会阻止原有的历史清理。
正在进行最后核对的工作线程会在核对结束时释放自己短暂持有的租约。

所有文件准入复用 `MediaArchiveService` 和 `FileIdentity`。读租约协调应用内部
的复制、发送和清理，不声称隔离另一个具有同等文件权限、持续交换路径的进程。
PNG 入口使用不会跟随最终链接或阻塞于 FIFO 的现有安全文件打开接口。复制完整
GIF/MP4 文件不另写一套解码器；需要严格媒体准入的宿主先调用共享
`validatedLease()`。

## 剪贴板确认

### Windows

`platform/windows/DesktopClipboard` 从既有 `ImageExport` 提取 WIC 编码、内存
布局和文件投递实现。旧 Windows `ImageExport` 和 Qt 服务调用同一个实现，不让
旧产品链接 Qt，也不重复维护两套编码逻辑。

图片一次准备三种实际格式：PNG、带 alpha mask 的顶向下 BGRA DIBV5、兼容旧应用
的 24 位底向上 DIB。GIF/MP4 继续使用含完整 UTF-16 路径的 `CF_HDROP`。
取色、坐标和 QR 文本使用同一 payload/发布通道的 `CF_UNICODETEXT`，不调用一个
无法报告结果的额外复制入口。按照 Windows 格式规范，裸 LF 在原生输出中转换为
CRLF，已有 CRLF 不重复插入；Unicode 与非 BMP 字符保持完整。
每个 `HGLOBAL` 仅在其 `SetClipboardData()` 真正成功后移交系统，其余内存由 RAII
释放。`OpenClipboard()` 被其他进程占用时直接返回 `Busy`，不先清空原内容。

原实现把任一种格式成功当作整体成功。共享适配明确要求三种图片格式均成功，
否则返回 `PartialPublication` 或 `ClipboardRejected`，保留已归档文件并允许
用户重试。这是对先前误报成功的修正：部分格式已提交时不宣称回滚了系统剪贴板，
结果同时包含实际提交的格式。

### Linux X11 与 Wayland

Linux 使用 Qt 的 `QMimeData` 与系统剪贴板后端。图片显式提供 `image/png` 和
Qt 图像数据，GIF/MP4 提供 `text/uri-list`；没有自写像素传输或 Wayland 管道。
文本同时提供 `text/plain` 与 `text/plain;charset=utf-8`，保留输入换行；与图像
使用相同的真实 owner/offer 确认和生命周期。来自 `QString::fromRawData()` 的内容
也在准备结束前成为独立内存，不借用随后可能复用的外部字符缓冲。
剪贴板内容由持续运行的 Qt 应用拥有，不依附发起操作的窗口。

固定 Qt 的 Wayland 接收端会把 `text/plain;charset=utf-8` 在
`QMimeData::formats()` 中规范化为 `text/plain`。独立接收验证以 `hasText()`
判断就绪，然后同时比较显式 UTF-8 内容和 `QClipboard::text()` 的完整文本；
不等待被 Qt 隐藏的格式名称，也不省略中文、非 BMP 字符、换行及制表符校验。

固定 Qt 6.12.1 会为本地文件 URL 额外提供
`application/vnd.portal.filetransfer`；另一个 Qt Wayland 进程读取文件时优先
通过会话总线的 `org.freedesktop.portal.Documents` 服务执行 FileTransfer。
因此文件复制的完整支持环境必须包含实际运行的 `xdg-document-portal` 和
`xdg-permission-store`（Ubuntu 由 `xdg-desktop-portal` 包提供），Linux 安装依赖
及 AppImage 宿主要求需包含此项。单纯发布 `text/uri-list` 不能证明 Qt 接收端
已取到文件：固定 Qt 在 Portal 返回空传输 key 时没有恢复到原始 URI。
文件发布先以 QtDBus 异步调用实际 `FileTransfer.version` 属性，最多三秒；
缺失或不支持的服务返回 `Unavailable`，不替换之前的剪贴板内容。检查通过后
才执行既有 worker 磁盘身份校验和 Qt 发布。取消或失败均释放本次读租约。
断开的 D-Bus 连接还会立即返回已经结束的调用。固定 Qt 的
`QDBusPendingCallWatcher` 在这一无内部 helper 的分支不发 `finished`；服务
检查 `isFinished()` 并在下一轮事件中处理相同结果。两条路径核对同一 watcher
身份，只完成一次，不在 GUI 线程使用 `waitForFinished()`，也不把缺少通知
留成永远忙碌的文件复制请求。实现依据是固定 Qt 的
[pending-call 源码](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/dbus/qdbuspendingcall.cpp)。
图像、纯文本和原始 URI 消费端不因此被当成文件 Portal 的验收证据。
预检是当时的能力证明；服务在成功发布后中断或文件随后被用户清理时，不能
保证未来取回仍成功。发布回执与之后接收端实际完成文件取回仍是不同事件。

X11 路径依据固定 Qt 源码，同时检查拥有权和 `mimeData()` 返回的实际选择对象；
后者会查询 X server 的 selection owner。`offscreen`、`minimal` 等非桌面后端
返回 `Unavailable`，不能作为真实剪贴板成功。

固定 Qt Wayland 的 `ownsClipboard()` 只检查本地 source 指针，不能证明合成器
接受了复制。本模块通过 Qt 的公开 `QWaylandClientExtension` 绑定标准
`wl_data_device_manager`，为当前真实输入 seat 增加一个只观察选择通知的 data device。每次提交有唯一 MIME 标记；只有合成器发回含此标记的选择 offer，且 Qt
仍拥有本次内容，才通知成功。观察者不请求内容、不接管 Qt 数据源、不读取数据管道，
也不执行 `wl_display_roundtrip()` 或 GUI 线程等待。

Wayland 复制必须来自可见且有焦点的窗口或其临时子窗口，并具有 Qt 记录的真实
输入 serial/seat。没有用户交互时返回 `RequiresInteraction`；选择确认超过三秒
返回 `Timeout`。取消是停止等待本次确认，不能声称撤销合成器已经收到的复制。

像素预算默认 512 MiB，编码输出也有限额。Windows 的三种兼容格式会各自占用
系统剪贴板内存；该预算是源图像预算，并非整个进程峰值内存承诺。完整性能验收仍需
测量目标分辨率、多屏和长截图负载。

文字默认上限为 16 MiB，分别检查 UTF-16 输入与平台编码后的完整 payload，
Windows 的终止符及新增 CR 字符也计入限额。它是 payload 限额而非进程峰值承诺。
Qt 的 UTF-16 验证负责 Unicode 准入；含内嵌 NUL 的值无法完整表示为原生纯文本，
因此返回 `Encoding`，不截断后报成功。QR 服务仍持有原始 payload，宿主在复制失败
时应保留结果窗口。空文本可以表示为合法的空纯文本。

## 回收与第二次确认

`recycle(archive, confirmedEntry)` 不重新枚举目录，只处理先前确认的具体快照。
读/发送租约冲突返回 `Busy`；身份或修订变化返回 `ChangedOnDisk`。两种情况都不
生成永久删除许可。

Windows 复用原有 `IFileOperation`，在 `PreDeleteItem` 中再次检查快照并要求
`TSF_DELETE_RECYCLE_IF_POSSIBLE`，拒绝 Shell 的永久删除分支。旧产品入口使用
保留系统二次提示的原策略，Qt 共享服务使用严格回收策略。没有用 Qt 的统一框架
替换成熟的 Win32 文件操作。

Linux 调用 Qt 官方 `QFile::moveToTrash()`，使用其 FreeDesktop Trash 机制。
固定 Qt Unix 实现的一个边界是：创建垃圾箱硬链接后，原文件 unlink 失败仍可能
返回 true。因此共享服务还检查原身份是否确实离开原路径。原文件保留时不得报
“已回收”；若硬链接操作改变原文件修订，则要求刷新和重新确认，不沿用旧快照
生成永久删除许可。实际垃圾箱路径保留在结果中，便于诊断和恢复。

只有真实回收失败且原文件仍与确认快照一致时，才生成无法由外部构造的
`PermanentRemovalPermit`。宿主控制器持有该许可，展示第二次、默认取消的确认。
明确确认后，`permanentlyRemove()` 再次核对同一身份，仅删除这一个文件；许可
不可重放。取消、未确认或许可为空都不能触发删除，绝不静默降级。

## 定位结果

Windows 保留 `SHOpenFolderAndSelectItems`；打开目录复用 `ShellExecuteExW`。
Shell 工作在有正确 COM apartment 的工作线程，GUI 不等待系统文件管理器。
已经交给 Shell 的打开操作无法撤销，取消不得伪造一个与实际 Shell 结果相反的终态。

Linux 先异步调用 `org.freedesktop.FileManager1.ShowItems`，打开目录则用
`ShowFolders`。方法失败后调用 GIO 的
`g_app_info_launch_default_for_uri_async()` 打开实际父目录，必须读取对应 finish
结果，不能用“已经排队”当成功。回退成功独立返回 `OpenedParent`，界面可说明
“已打开所在文件夹”；不能显示为已选中文件。整个 Linux 请求有十五秒期限，
DBus 单次请求两点五秒；与 Portal 一样复用 Qt 的 GLib 主上下文。

这些结果表示真实系统 API 的完成/启动确认。仍需目标桌面的实际人工验收来检查
窗口呈现、文件是否高亮及窗口管理器的焦点行为。没有把后端受理等同于物理桌面
已逐项验收。`Failure.detail` 用于诊断，产品提示由宿主根据错误类型本地化。

## Linux 自启动路径与实际启动

自启动仍先提交本应用的用户级 `.desktop` 项，再保存共享配置；配置保存失败时
恢复提交前的准确字节或缺失状态。如果期间另一个写入者改变了该项，回滚不覆盖
外部内容。设置页和独立服务使用相同的 `AutoStart` 实现。

含字面 `%` 的应用路径有一个实际 GLib 兼容边界：`GDesktopAppInfo` 建立对象时先
验证 `Exec` 的首个 token，启动时才展开 `%%`。因此直接将已按规范转义的路径放在
首位，会在启动前被错误地当成不存在的双百分号文件名。仅对这一情况，使用固定
绝对 `/usr/bin/env --` 把应用路径作为参数传递；全部参数继续由共享
`desktopExecCommand` 转义，`TryExec` 保持真实应用路径。普通路径直接启动，
该分支不调用 shell，也不通过 `PATH` 查找应用或转发程序。正常 GLib 校验仍完整
执行，系统缺少转发程序时不会报告启用成功。依据为
[GLib 2.80.0 的加载与启动实现](https://github.com/GNOME/glib/blob/2.80.0/gio/gdesktopappinfo.c)
及 [Desktop Entry Exec 规则](https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html)。

`qt_settings_controller --startup-only` 直接运行原有完整自启动回归，使用隔离的
真实 XDG 目录、真实配置保存冲突和 GLib 启动的独立子进程。路径仍包含 Unicode、
空格、`$` 和 `%`，子进程必须实际收到 `--auto-start`。该入口可以使用 offscreen
运行，不依赖 X11 热键前序；默认的原生设置测试继续运行完整热键、自启动和控制器
流程。它验证真实启动调用和回滚，不代表已测试用户登录时的自动启动。

## 复用来源

完整来源与文件 SHA 记录在
[desktop upstream](../third_party/desktop/upstream.json)。没有复制 Qt、GIO 或 ksnip
的私有类到生产源码。

- 项目现有 `ImageExport.cpp`、`MediaHistory.cpp`：基线
  `0efaf45aae1062436da8238814bba17c325fe4d3`，保留仓库 MIT 许可和 NAMEWTA/ThorVG
  归属；Windows 实现提取后由旧入口直接复用。
- Qt 6.12.1，固定 `qt/qtbase` commit
  `5af1a37e31a869cd6eb8a73d82e2c71a450e6634`：阅读了 QFile、Unix/Windows filesystem
  engine、XCB/Wayland clipboard/data source/data device、公开 native/extension API
  和 Unix desktop services 源码。使用公开 API，Qt 按统一依赖与许可证清单分发。
- 文本路径另阅读固定 Ling `src/Util.cpp` 的既有 `CF_UNICODETEXT` 实现
  （MIT，2025 liulun）、Qt `qmimedata.cpp` 的文本编码与
  `qwindowsmimeregistry.cpp` 的换行处理。采用已有服务的 RAII 和真实返回结果，
  不复制 Qt 私有 MIME converter。原生换行依据
  [Windows 标准剪贴板格式](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats)。
- GLib/GIO 2.80.0，固定 commit `763cc3b238398614c20069fd67642730e3a6519b`：阅读
  `gio/gappinfo.c` 完整异步默认应用查询、启动、Portal 回退及 finish 路径。
  LGPL-2.1-or-later；Red Hat, Inc.，原作者 Alexander Larsson。运行时使用系统
  GIO >= 2.80，实际发行版本由构建清单记录。
- Issue R05 的 ksnip 固定 commit `059f022cb9c4bdd30d3e8311eb9199ca92fed4f3`：阅读
  `src/gui/clipboard/ClipboardAdapter.cpp`；GPL-2.0-or-later，Damir Porobic，2020。
  作为 Qt 应用持有剪贴板的行为参考，未复制其代码。
- [FreeDesktop FileManager1](https://www.freedesktop.org/wiki/Specifications/file-manager-interface/)：
  采用其公开 ShowItems/ShowFolders 接口；无复制规范中的示例实现。

## 验证入口与当前边界

- `desktop_clipboard_tests`：真实独立接收进程读取 PNG/URI/Unicode 文本；Windows 还逐字节
  检查 DIBV5 alpha、DIB 行方向和 CF_HDROP，并让另一个进程实际占用剪贴板。
  覆盖取消、一次性内存移交、发布前文件变化、读租约释放及窗口关闭后内容有效。
  文本用例包含中文、非 BMP、LF/CRLF、制表符、外部缓冲独立所有权；Windows
  实际竞争进程占用时也必须返回文本发布 `Busy` 并保留先前图像。
- `desktop_files_tests`：真实文件回收、读租约冲突、变化快照、取消、独立永久
  确认许可；Windows 用真实文件共享锁触发失败，Linux 用真实不可用垃圾箱和
  无写权限目录触发失败。Linux 权限测试必须由普通用户运行。
- `file_revealer_tests`：实际文件检查与变化保护。`--real-desktop` 才调用真实
  Explorer/FileManager1/GIO；默认模式明确不宣称已测试打开文件管理器。
- `scripts/test-wayland-clipboard.py`：创建独占的 headless Sway，用系统 wtype
  发送真实虚拟键盘事件，再运行两个独立 Qt 进程；同时拥有独立的 session bus、
  permission store 和 Documents portal，以实际 FileTransfer 调用验证 Unicode
  文件 URI。停止服务并检查本次运行的 FUSE 挂载清理后再移除私有目录。
  不伪造输入 serial、选择 offer 或接收内容，也不删除 Portal MIME 来绕过 Qt
  接收路径。wtype/Sway/Wayland/Portal 包版本及真实日志写入指定 artifact 目录；
  URI 不符时还记录实际格式、编码字节和接收 URL。
  另外启动会话总线不可用的独立真实 Wayland 客户端，要求文件发布返回
  `Unavailable`、释放历史租约，并仍能读取此前拥有者的完整 PNG。
- `desktop_clipboard_tests --reject-backend`：仅 Linux offscreen 负测，应明确失败
  为 `Unavailable`，不算系统剪贴板正向验证。

本地环境只有 C++ 工具链，尚无可运行的 Qt SDK、Windows 桌面或 Sway；本批已经
完成源码审查、脚本语法检查和补丁空白检查，新增服务的真实编译/运行待 CI 回执。
不得把这些测试入口或文件存在写成测试通过。X11/Wayland 普通接收程序、GNOME、
Plasma、物理混合 DPI、进程退出后的剪贴板管理器接管，以及整套应用 UI/升级流程
仍需各自的真实验收证据。

新增文本迁移在本地完成源码/API 检查，尚未在本地编译或执行 Qt/Win32 测试；
不能以此前图像和文件复制的 CI 结果代替新增文本路径验收。
