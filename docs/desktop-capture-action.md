# 桌面“停止并完成”入口

Issue #5 §8.4 要求没有托盘的 Wayland 桌面仍能完成手动长截图。捕获控件必须
先隐藏，不能一边把控件录入像素、一边将“隐藏后无入口”当作已经支持。
`DesktopCaptureAction` 为 GNOME 提供一条经过实际进程和 IPC 确认的 desktop
action 路由；停止和完成仍由现有 RecordingWorkspace、ScrollWorkspace 执行。

这条路由的系统查找、进程启动和命令转发可分别测试。它们不能代替真实 GNOME
菜单展示、操作可达性、授权采流以及最终拼接结果的验收。

## 控制动作与既有单实例

普通启动、`--auto-quit`、`--auto-start` 和 `--enter=...` 保留原有优先级。
新增 `--action=finish-capture` 是独立控制动作，不属于截图工具类型：

- 只连接同一用户、桌面会话和配置范围的既有 primary。
- 即使同时带 `--auto-quit`、后台启动或工具参数，也不建立一次性截图实例。
- 找不到 primary、primary 正在退出、动作被拒绝或确认丢失时明确失败。
- 不选主、不移除旧端点、不获取或恢复主实例锁，也不回退成普通截图。
- 空闲时由宿主拒绝完成动作；录制时调用既有 Save stop，长截图时调用既有
  stop/finish，保留各自真实 worker、编码收尾、最后完整图及归档合同。

独立 `--auto-quit` 一次性实例不是 primary，也不持有这条控制路由。它不能借用
另一 primary 的验证结果；若自己没有实际托盘或已注册快捷键等停止入口，仍须
保留明确的能力限制。

SingleInstance 继续使用原来的 24 字节受限报文。普通启动保留 v1；v2 用原保留
字节区分 Finish 和 Verify。原 16 字节 request UUID 同时承担 Verify 的一次性
关联值，ACK 仍核对同一个 UUID。没有新增本地 socket、脚本执行通道、任意文件
参数或第二套业务调度。验证字段、版本、动作类型、工具和后台标志组合均校验；
未知控制不能成为默认截图。

仍保留的 Windows 旧架构入口没有这套 Qt 控制 IPC。它在设置、托盘和捕获窗口
初始化前对非 Launch 动作退出并返回 2，避免新控制命令被旧参数路径误当截图。

## 何时可以隐藏控件

宿主只为 primary 创建服务，在控制窗仍可见且捕获尚未开始时调用 `verify()`。
一次确认按以下实际边界完成：

1. 当前必须是实际 XCB/Wayland 桌面，环境声明 GNOME，真实会话总线上的
   `org.gnome.Shell` 返回可识别的 ShellVersion。读取属性使用异步 QtDBus，
   不调用 Shell Eval、私有快捷键抓取或授权绕过。
2. 首次查找前在实际 Qt GLib 主上下文订阅 `GAppInfoMonitor::changed`，随后
   工作线程调用 GIO 查找最终生效的 `io.github.NAMEWTA.huhucapture.desktop`。
   必须可显示，包含 `FinishCapture` action，且不要求终端或 D-Bus activation。
   物理文件按现有 FileIdentity 读入，限制为 64 KiB 完整 UTF-8，并核对身份、
   修订和长度。
3. GLib 解析 action 的 Exec。目标必须是当前持久可执行文件以及准确的显式
   portable profile，不能把其他安装、任意 shell wrapper、文件 URI 字段展开
   或临时 AppImage 挂载目录解释成当前入口。
4. GIO 真正启动这个 action。一次性 nonce 只进入该次 `GAppLaunchContext`
   的子进程环境，不写桌面文件、不更改父进程环境。子进程入口读取并立即移除
   nonce，将 Finish 转为 Verify；畸形 nonce 必须失败，不能执行真正停止。
5. Verify 经既有 SingleInstance 回到当前 primary。服务只接受本次未消费且未
   超时的 nonce；取消、退出、未知值或重放均拒绝。这一步不调用任何 stop 方法。
   GIO 的 void 返回和进程启动本身都不构成成功。
6. 服务监视生效 desktop 文件、可执行文件及 XDG 搜索目录，并在安装监视后
   再由工作线程核对同一注册。GIO 的桌面 ID 映射有缓存，重复查找不保证同步
   重扫；如果更高优先级条目恰好出现在首次查找之后、Qt 监视之前，正式
   `GAppInfoMonitor` 通知也会撤销证明，避免仅重读旧文件产生持续的错误可用
   状态。只有这些检查与实际关联请求都完成后才发布可用状态。

整个验证有十五秒接收期限，Shell 属性调用另有三秒期限。取消立即关闭 nonce
准入；迟到子进程仍只能发送已经失效的 Verify，不能停止另一项捕获。正常
shutdown 等真实查询、工作线程和排队终态结束后通知完成，不在 GUI 线程等待
GIO 文件/进程操作。GIO 公共 launch API 的内部总线资源由框架管理；服务不将
共享会话总线的全局生命周期当作本对象的独占资源。

desktop entry、优先搜索目录或可执行文件发生变化，以及 GNOME Shell owner
或会话总线失效时撤回状态。目录变化采用保守撤回；不在捕获中悄悄重新启动一个
验证进程。宿主应按既有停止能力丢失规则保留结果并停止，等旅程结束后再验证。
primary shutdown 也必须调用服务 shutdown。

收到真实 Finish 时，宿主直接复用当前录制或长截图动作。它不能打开新的截图
框选、错误对话框或另一套控制窗口，再把这些界面混入正在采集的帧。

## desktop 文件与分发

生产 desktop 文件增加“停止并完成捕获”动作。普通启动继续保留原命令；DEB
打包时，这个无窗口控制动作的 Exec 指向真实的
`/opt/huhucapture/bin/huhucapture --action=finish-capture`，不解析或复制外层
`/usr/bin/huhucapture` 的 shell launcher。控制进程只协调 IPC，不初始化
PipeWire 采集服务；普通启动仍由既有 launcher 设置产品运行环境。

`StartupNotify=false` 遵循固定 Flameshot、ksnip 桌面入口的工具型应用行为。
后台/托盘或 Finish 动作没有新窗口，不应要求通过映射窗口完成启动指示；没有
为了此动作再引入 GDK 或私有启动通知协议。

AppImage 只接受系统中真正注册、指向持久外层 APPIMAGE 的 action。AppDir 内
存在一个 desktop 文件不等于已安装桌面入口。移动外层文件、缺少真实集成、
目标是另一份安装或 profile 不符时，服务保持不可用；复制命令字符串也不能
冒充全局快捷键已注册。没有可达入口的组合仍属于发布门槛。

## 来源与职责

复用顺序是本项目已有 Startup/SingleInstance/工作区 stop 动作，随后是 Issue
引用的 Flameshot/ksnip 行为，再使用官方 GIO、Qt 和 GNOME Shell 已公布接口。
没有复制第三方 desktop parser 或另建 D-Bus activation 框架。

来源、逐文件 blob、版本与许可记录在
[desktop capture action upstream](../third_party/desktop-capture-action/upstream.json)。
已阅读 GNOME Shell 46.0、50.0 的 AppMenu 和 D-Bus 接口、46.0 的 app-display /
native launcher，以及 GLib 2.80.0 的 desktop 查找、action 列表、Exec 扩展、
launch-context 环境、实际 spawn、目录缓存失效和 app-info monitor 主上下文路径。
GNOME 的 AppMenu 枚举 GDesktopAppInfo
actions，再调用 native launch_action；这一代码关系不等同于已经人工测试菜单。

## 验证边界

- `startup_tests` 保留旧参数矩阵并验证新增控制动作的优先级。
- `test-single-instance.py` 使用实际独立 Qt 进程，验证控制只转发、无 primary
  不选主/不写 IPC 文件、nonce 保真、v1/v2 和非法组合拒绝。
- `desktop_capture_action_tests` 使用实际 Qt 回调，检查 nonce 解析、无真实
  桌面时不声称可用、关闭 admission、排队结果顺序和重入销毁。
- `desktop_capture_action_probe` 默认读取真正的隔离 GIO application registry，
  检查目标/配置、隐藏条目、错误动作、取消和物理替换。它不会声称 GNOME 菜单
  已显示。`--gio-roundtrip` 必须在真正的 session bus 下运行，并实际启动另一个
  可执行进程；子进程收到 IPC ACK 后保存回执，父进程核对不同 PID、nonce
  准入一次、没有真实 Finish、重放被拒绝及事件循环心跳。
- `desktop_capture_action_probe --monitor-override` 在独立进程中先建立系统
  entry，再在首次 GIO lookup 后实际创建更高优先级的用户 entry。它要求真实
  GIO 通知、重新解析选中该条目、再删除后重新选中系统条目，并在 native 回调
  内释放监视器，检查再激活和回调销毁；没有模拟 Shell、文件事件或信号。

```sh
dbus-run-session -- ./desktop_capture_action_probe --gio-roundtrip
```

本地固定 Qt 6.12.1 / GCC 14 已严格编译此批源文件，参数矩阵、Qt 不可用/销毁
测试及真实 GIO 注册文件检查已运行通过；GLib 2.80.0 实际覆盖/移除测试收到两次
native 变更通知，正确重新解析，并通过回调内销毁。真实 Finish 在无 primary 时也已实际
返回失败且未创建锁、socket 或子目录。当前工作环境禁止 Unix socket bind；
跨进程正向测试在 QLocalServer listen 被拒绝，私有 dbus-daemon 也在 bind 被拒绝，
因此这两项须由 CI 实际裁决，不记为通过。

GNOME 46/50 的真实菜单显示、无托盘/无 GlobalShortcuts 时的用户操作、Overview
对源像素的影响、手动滚动到完成及所得长图，均需在各目标桌面另验。路由 ACK
不会替代这些 UI、像素和最终结果的验收，也不能使未达到 Issue 门槛的版本发布。
