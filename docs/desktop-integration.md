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
  发送真实虚拟键盘事件，再运行两个独立 Qt 进程。不伪造输入 serial、选择 offer
  或接收内容。wtype/Sway/Wayland 包版本和真实日志写入指定 artifact 目录。
- `desktop_clipboard_tests --reject-backend`：仅 Linux offscreen 负测，应明确失败
  为 `Unavailable`，不算系统剪贴板正向验证。

本地环境只有 C++ 工具链，尚无可运行的 Qt SDK、Windows 桌面或 Sway；本批已经
完成源码审查、脚本语法检查和补丁空白检查，新增服务的真实编译/运行待 CI 回执。
不得把这些测试入口或文件存在写成测试通过。X11/Wayland 普通接收程序、GNOME、
Plasma、物理混合 DPI、进程退出后的剪贴板管理器接管，以及整套应用 UI/升级流程
仍需各自的真实验收证据。

新增文本迁移在本地完成源码/API 检查，尚未在本地编译或执行 Qt/Win32 测试；
不能以此前图像和文件复制的 CI 结果代替新增文本路径验收。
