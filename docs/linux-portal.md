# Linux Portal 控制面

本模块是 Issue #5 的 P00/P05 前置工作。它提供真实 Portal 能力查询、截图请求、ScreenCast 授权与 PipeWire remote 文件描述符交接，以及 GlobalShortcuts 会话、绑定和触发控制。视频帧消费在独立的 [PipeWire 数据面](linux-pipewire.md)，音频或正式 Qt 截图界面仍需后续集成。编译通过、故障测试通过和桌面采集/快捷键验收必须分别记录。

## 模块边界

入口为 [PortalCapture](../src/platform/linux/portal/PortalCapture.h) 和 [PortalShortcuts](../src/platform/linux/portal/PortalShortcuts.h)，实现位于同目录。两者共用 PortalTypes 的错误合同及 PortalSupport 的初始化、错误转换和 context 检查。CMake 目标为 `huhucapture_portal`，仅 Linux 的 `HUHUCAPTURE_PORTAL_VALIDATION=ON` 启用，并要求 `HUHUCAPTURE_QT_VALIDATION=ON`。公共头只有 Qt/标准库类型，不向编辑模型暴露 `XdpPortal`、GObject、GVariant 或 PipeWire 类型。

- 直接链接 libportal 的 Screenshot、CreateSession、SelectSources、Start、Session.Close。X11 直接用其 Qt parent helper；Wayland 使用成熟异步导出后再交给 libportal 的 parent 抽象。仅用 GIO 查询公共 `Properties.GetAll` 能力，不另写一套 Portal 请求协议。
- Screenshot 成功返回 Portal 给出的有效本地文件 URI。它不代表图片已经解码、导入历史或复制到剪贴板；那些操作由共享媒体服务完成。
- ScreenCast 成功必须经过授权、非空 stream 元数据解析和真实 `OpenPipeWireRemote` FD 获取。`takePipeWireFd()` 只转移一次 FD，调用方负责关闭或将其所有权交给 PipeWire。
- 存在 v6 `pipewire-serial` 时保留并校验其真实 uint64 值；数据面优先按 serial 连接同一授权 remote。旧 node ID 不能当成 serial，也不能据此回连到无关的全局源。
- `PortalStream` 的 position/size 是 compositor 坐标，不能当作实际视频像素或 DPI 比例。后续 PipeWire 层必须依据协商格式读取像素尺寸、stride、buffer 类型与时间戳。
- 不使用 X11 截图代替 Wayland 授权。Qt 的 xcb/Wayland 平台名仅作为环境记录；可用能力来自当前 Portal 的真实属性与请求结果。

## 线程、取消和资源所有权

控制器的构造、使用和销毁均在 QApplication 线程。运行时要求 Qt 的 GLib dispatcher，并检查 thread-default context 与全局 default context 一致。GLib async 回调、Qt parent 导出、取消、session close 均在此线程执行；不创建第二个 GMainLoop，不从其他线程迭代同一个 context。Qt 6.12.1 的 [dispatcher 实现](https://github.com/qt/qtbase/blob/v6.12.1/src/corelib/kernel/qeventdispatcher_glib.cpp) 是该约束的来源。

`openPipeWireRemote()` 只为仍然 ACTIVE 的同一 ScreenCast session 打开另一个授权数据连接。前一个 FD 必须已由调用方领取，不能与其他未完成 Portal 请求并行；它复用既有异步 GIO/libportal 接缝，不重新 SelectSources/Start、不阻塞 GUI。取消、关闭会话或迟到结果会关闭实际返回的 FD；成功结果保留 close-on-exec 并经 `pipeWireRemoteOpened` 交付一次。共享实时来源仅在旧 producer/preview/lease 全部退出后使用此接口，具体顺序及其不等同于 compositor presentation 的边界见 [Wayland 刷新合同](wayland-capture-refresh.md)。本地真实无授权请求明确失败且完全 drain，没有伪造 session 或成功 FD。

libportal 0.11.0 的 `xdp_portal_initable_new()` 内部仍调用同步 `g_bus_get_sync()`。控制器只在 GTask worker 上构造它，完成后把对象交给 Qt 线程。没有使用会在 session bus 缺失时 abort 的 `xdp_portal_new()` 或 `XdpQt::globalPortalObject()`。GUI 线程不等待 join 或同步 DBus 请求。共享 GIO bus 连接显式设置 `exit-on-close=FALSE`，否则 [GIO 的默认行为](https://docs.gtk.org/gio/func.bus_get_sync.html) 可能在运行中断线时直接结束进程。`closed` 信号交给同一 Qt/GLib context 处理，取消待决请求并释放活动会话；不绕过文件或媒体清理。

固定 libportal 的 Qt helper 调用 `QDesktopUnixServices::portalWindowIdentifier()`；Qt 6.12.1 在首次 Wayland foreign handle 导出时执行同步 `wl_display_roundtrip_queue()`。源码依据为 Qt 的 [qwaylandplatformservices.cpp](https://github.com/qt/qtbase/blob/v6.12.1/src/plugins/platforms/wayland/qwaylandplatformservices.cpp) 和 [qwaylandxdgshell.cpp](https://github.com/qt/qtbase/blob/v6.12.1/src/plugins/platforms/wayland/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp)。因此 Wayland 走独立的 [PortalParent](../src/platform/linux/portal/PortalParent.h) 平台接缝：复用 KWindowSystem 6.32.0 的两个异步导出类，通过官方 QWaylandClientExtensionTemplate、标准 xdg-foreign-v2 生成绑定及 handleReceived 取得真实句柄，再包装成 libportal parent。没有将 QWindow 移到 worker，也不调用阻塞 roundtrip、独立 dispatch 或嵌套事件循环。具体源码、逐文件 blob、作者和许可见 [upstream 记录](../third_party/kwayland-parent/upstream.md)。

导出对象活到 Portal 请求终态。取消、父窗 surface/role 销毁、compositor 移除导出协议都会释放导出对象并报告原因；迟到 Qt 信号因 QObject context 或终态检查被丢弃。只有收到非空原生 handle 才创建 `wayland:` 标识。原生 handle 导出若 5 秒仍未收到响应，明确失败并释放 proxy；该截止值只用于 compositor 元数据请求，不套在人工 Portal 授权对话框上。parent 导出成功后发生失效会取消仍待决的 Portal 请求。ScreenCast 授权完成后独立持有 session，释放 parent 不会关闭活动录制。

每次授权拥有自己的 cancellable、请求编号、parent 连接和回调状态。parent 在请求完成前销毁会取消该请求；迟到回调仍完成原请求并清理资源，不访问已销毁 QObject。控制器销毁后，异步回调保留清理状态但不再发 UI 信号。应用退出编排仍应先取消待决请求、等待终态回调，再结束事件循环；不能先停掉整个 dispatcher 后把异步资源清理算作完成。

`PortalCapture::shutdown()` 立即关闭新请求入口，但 `shutdownFinished` 还必须等待初始化、授权、重新打开 remote、已排队的拒绝通知和正在执行的终态回调真实退出。公开取消/关闭方法、parent 终态和原生 service/session 信号也计入同一门槛，允许接收器在回调内销毁 public QObject 或分发事件而不提前报告排空。固定 libportal 的 Screenshot/Create/Start 取消会先完成公共 GTask，迟到 D-Bus 方法回调稍后才释放 task/session/portal 引用，因此 C++ 回调计数归零本身不够。控制器复用 GlobalShortcuts 的独占 XdpPortal 最后引用通知；初始化重试替换旧实例时也保留该释放门槛，不能只检查最后一次实例。

GObject 的 weak notification 在 `finalize` 之前执行，见固定 [GLib 2.80.0 的 g_object_unref](https://github.com/GNOME/glib/blob/2.80.0/gobject/gobject.c)（blob `7ec5902b6e4591b6036a63aef2dcce87ac7850ef`）。它证明原生引用已经进入最终销毁路径，不能在该回调栈内直接宣布清理完成。因此实际 ack 排到下一 Qt 事件轮，再次核对所有门槛；被接纳的 Portal 对象及其异步操作仍严格遵守同一 GUI/GLib context。这里不等待整个共享 GDBus connection 被全局销毁，也不把 weak owner 清空或取消订阅当作原生引用释放。

成功的 ScreenCast session 由录制服务所属控制器继续持有，授权 parent 销毁不会撤销已经交付的录制会话。因此控制器应归应用服务或录制会话拥有，不能作为短命授权窗口的 QObject 子对象。后续录制服务应先停止 PipeWire/编码消费者，再调用 `closeScreenCast()`。外部 session 关闭或 Portal 服务消失会发出明确错误，并释放控制器仍持有的 FD。

取消与失败不依赖超时伪装成完成。实际 `Request.Close`、GTask 终态和回调资源释放在受控 libportal 补丁中完成。GUI 调用必须遵循上述单 context 约束；本补丁不承诺从任意线程同时调用 cancellation handler 的扩展语义。

## 结果和能力语义

`PortalFailure` 保留错误 domain、native code、remote DBus error 与消息。取消、parent 销毁、明确拒绝、服务不可用、能力不支持、忙碌及其他失败分别表示。Portal `Response=1` 本身表示取消，某些桌面将用户拒绝也表示为这一值；不能仅凭它声称区分“拒绝”与“取消”。只有明确的权限错误才映射为 Denied；`Response=2` 保留一般失败语义。

截图和 ScreenCast 的版本与失败分别记录，单个接口存在不代表所有接口可用。所选 source/cursor 模式必须被当前 Portal 广告支持；旧接口缺少 cursor 控制或 v4 persistence 时不会悄悄忽略调用方选项。失效 restore token 按真实错误返回，后续 UI 决定是否重新请求授权。

## GlobalShortcuts 会话与重绑

GlobalShortcuts 的能力单独查询。控制器直接调用 libportal 的 CreateSession、BindShortcuts、ListShortcuts 和 Close；没有自行重写授权 D-Bus 协议。偏好触发键采用 XDG shortcuts 规范字符串，不能直接传入 Qt 的本地化按键展示文本。实际展示文本取 Portal 返回的 `trigger_description`，不能把偏好键当作已经获准的键。

[接口规范](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.GlobalShortcuts.xml) 限制每个 session 只能 BindShortcuts 一次，并允许返回部分绑定。控制器据此创建候选 session；只有返回全部请求 ID、有效 trigger 描述且 session 仍存活，才交付新绑定并关闭旧 session。失败、取消和待决 parent 销毁只清理候选 session。旧 session 在请求期间保持连接。不同桌面对同应用多个 session 的限制及重绑冲突必须实机验证，不能把保留旧 session 的代码行为当作旧快捷键实际仍触发的测试证据。

活动会话不依赖已经完成授权的窗口寿命。激活/释放事件只转发当前活动会话，带有 Portal 原始毫秒时间戳和可选 activation token；token 不持久化、不写入探针日志。收到 ShortcutsChanged 后用真实 ListShortcuts 刷新完整绑定，合并重复刷新请求并按会话代次忽略旧回调。刷新失败显式报告，不覆盖当前已知列表。服务或 bus 消失、外部 session close 会关闭活动绑定并报告不可用。

共享 [设置控制器](settings-controller.md) 可传入候选提交回调：完整授权之后、关闭旧 session 之前保存实际配置；返回 false 时只清理候选。回调持有 shared state / operation，并在返回后重新检查 owner、取消、parent 和 candidate 寿命，防止 UI 重入使旧会话被错误关闭。此回调不得进入嵌套事件循环。typed 保存失败由上层回调结果表达，不解析底层英语消息。

普通 `bindShortcuts` 继续要求真实可见的 QWindow parent。只有显式 `restoreShortcuts` 请求使用规范允许的空 parent 字符串，复用原 libportal BindShortcuts API；它不调用要求非空 parent 的包装，也不构造虚假的 Wayland 句柄。桌面仍可能要求授权，不能把恢复请求视作已获得许可。`cancelInitialization` 取消真实 GTask 初始化，终态仍通过原 initialized 回调返回，供应用退出编排等待。

`closeBindings()` 只关闭当前会话并允许以后重绑；完整退出使用 `shutdown()`，它立即关闭新请求入口。`shutdownFinished` 必须等初始化、候选绑定、ListShortcuts 刷新和已排队的拒绝通知真实结束。Call/Refresh 先释放各自的 operation/session，再减少回调计数，下一事件轮重新核对后才通知；不是调用 cancel、清空 weak owner 或取消订阅就宣布完成。parent cancellation 可以同步发出终态，因此公共关闭方法也保持 State 并计入活动投递，允许终态接收器销毁 public QObject 而不留下后续访问。

固定 libportal 的 ParentBind 在取消时可以先返回外层 GTask，其内层回调稍后才释放剩余 task/session 引用。因此自身回调排空之后，控制器还释放本次独占的 XdpPortal，并等待其真实 GObject 最后引用通知，随后在下一事件轮确认，遵循上文的 finalize 顺序。这里复用既有缺失服务测试的实际对象释放证据；不要求整个共享 GDBus connection 被全局销毁，也不把 public QObject 的 weak pointer 清空当作原生资源完成。

共享 GlobalShortcut 还等待自己的 queued bind/reject 投递，然后设置控制器才发出原有 `shutdownFinished`。Windows/X11 仍实际释放 HWND 热键或私有 XCB 连接，再经过相同的共享完成门槛；应用不需要第二套退出协议。具体修复和已执行范围见 [生命周期审查](qt-lifetime-review.md)。

## 固定上游与受控补丁

依赖版本、源码 SHA 和补丁 SHA 以 [依赖锁](../build/dependencies.lock.json) 为唯一构建输入。目标基线为 libportal 0.11.0，提交 `fc4d62cfe131e8494c82f597f92b2234d584c367`；Qt parent 库必须与应用的 Qt 6.12.1 及其 private headers 同源构建。

实际阅读的上游实现包括：

| 源码 | 用途 |
| --- | --- |
| [screenshot.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/screenshot.c) | Screenshot/Response/取消/parent 导出 |
| [remote.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/remote.c) | CreateSession、SelectSources、Start、FD 与 stream 元数据 |
| [session.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/session.c) | session 注册、关闭、引用拥有关系 |
| [portal.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/portal.c) | session bus 初始化及失败行为 |
| [globalshortcuts.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/globalshortcuts.c) | 会话、绑定、激活、实际列表与 g_rc_box 生命周期 |
| [portal-qt6.cpp](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/portal-qt6.cpp) | 直接复用 Qt parent；Qt ≥6.9 使用 QDesktopUnixServices |
| [libportal/meson.build](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/meson.build) | GIO ≥2.80、Qt private ABI 和 backend 选项 |

[受控补丁](../third_party/patches/libportal/0001-capture-request-lifetime-and-async-fd.patch) 针对这些具体缺口：

1. Screenshot 取消可能先释放 call，而初始 DBus 方法仍有迟到失败；补丁给未返回的方法保留独立引用，并在终态先断开 handler。
2. CreateSession 的取消使用旧 request path，SelectSources/SelectDevices 阶段没有更新；补丁更新当前 handle，并在失败/取消时关闭请求与未交付 session。
3. Start 没有处理 DBus 方法失败，只发 Close 也不能保证收到 Response；补丁完整返回错误、处理主动取消和提前 session close。
4. parent 导出失败、预先取消和未知 response 不得留下永不完成的 GTask。
5. session 的 portal 注册引用与 session→portal 引用需要在 close 时解环，关闭信号期间保留对象；stream variant 的额外引用随局部结果释放。
6. 新增异步 `xdp_session_open_pipewire_remote_async/finish`，直接使用 GIO 的 FD/cancellable API，保留错误和 FD 所有权；不在 GUI 调用原有同步接口。

检索上游后续提交时，main 比固定版本仅多两个提交，Screenshot/remote/session 未获得对应修复。已复核既有 [b7be557](https://github.com/flatpak/libportal/commit/b7be557098e57cf8c65c176984d9a4e8df2907b8)、[b8c57df](https://github.com/flatpak/libportal/commit/b8c57df1ea6d72984c96f6c34428626dc74325e9)、[0977a830](https://github.com/flatpak/libportal/commit/0977a83082979c221eb00743e86cab1f5dc5b685) 的处理依据：Close 后由调用者完成任务、返回终态前断开 cancellable handler、取消必须返回错误。补丁沿用这些规则，不把旧修复误报为当前问题已解决。

[第二个补丁](../third_party/patches/libportal/0002-globalshortcuts-lifetime-and-qt-parent.patch) 独立修改 globalshortcuts.c/.h，保留第一个补丁的固定哈希。它复用原来的 g_rc_box Call，修复预取消、Close 后不完成 GTask、终态 handler 断开次序、迟到方法回调与失败临时 session 清理，并增加 `bind_shortcuts_with_parent()` 窄包装。该包装调用原 BindShortcuts API 和现有 parent exporter，为导出回调与请求回调各保留独立引用，直到真正回调结束才释放 parent。它不增加新的授权协议，也不改变原 API。GlobalShortcuts 上游实现起点为 [280c128](https://github.com/flatpak/libportal/commit/280c1281e0d1710574d3409869097df0de61c366)；固定版本之后未找到对应生命周期修复可直接移植。原始 globalshortcuts.c 和 .h blob 分别为 `59b35c2462ada102554628b80cc23147275973ae`、`70a6920ef2374c2b575cf450afe366052bb0d010`。

[第三个补丁](../third_party/patches/libportal/0003-preexported-parent-handle.patch) 只扩展 parent.c/.h 的构造入口，允许调用方传入已经真实导出的合法窗口标识。parent 的复制/释放沿用 libportal，只持有自己的字符串；原生 export 的生命周期仍由 PortalParent 管理。原文件作者为 Georges Basile Stavracas Neto，blob 分别为 `7c9a2df201360c65b47c1d5e7714e699b7bd8cc0`、`8bced8d64d3accdade72f387dd9b6951bd41f7a0`。前两个补丁不变，第三个补丁也没有改写 Portal 协议或更换采集后端。

被修改的原文件版权归 Matthias Clasen；libportal Qt helper 还保留 Georges Basile Stavracas Neto、Jan Grulich、Neal Gompa 和 GNOME Foundation 等原始归属。libportal 和本补丁按 LGPL-3.0-only 使用，附带原始 [COPYING](../third_party/patches/libportal/COPYING) 与其所引用的 [GPL v3 文本](../third_party/patches/libportal/COPYING.GPL3)。后者来自 Qt 6.12.1 的 `LICENSES/GPL-3.0-only.txt`，原 blob `94a9ed024d3859793618152ea559a168bbcbb5e2`。不能将新增库重新标为 MIT；正式打包还必须提供匹配的动态库、源码、受控补丁和许可记录。

## 构建与检查入口

Ubuntu 24.04 需要 `build-essential meson ninja-build pkg-config libglib2.0-dev libglib2.0-dev-bin dbus libwayland-dev libwayland-bin sway`，GIO/GLib ≥2.80。同一 Qt prefix 必须提供 Core/Gui/Widgets/WaylandClient、qtwaylandscanner 及版本匹配的 Gui private headers；不要让 pkg-config/qmake 选择系统的其他 Qt。只链接 Qt 与 Wayland 的所需模块，不安装整个 KWindowSystem 运行时。

```sh
export HUHUCAPTURE_QT_ROOT=/absolute/path/to/Qt/6.12.1/gcc_64
export PATH="$HUHUCAPTURE_QT_ROOT/bin:$PATH"
export CMAKE_PREFIX_PATH="$HUHUCAPTURE_QT_ROOT"
export PKG_CONFIG_PATH="$HUHUCAPTURE_QT_ROOT/lib/pkgconfig"
python3 scripts/build-portal.py
export PKG_CONFIG_PATH="$PWD/.deps/portal-validation/install/lib/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$PWD/.deps/portal-validation/install/lib:$HUHUCAPTURE_QT_ROOT/lib"
cmake -S . -B artifacts/portal -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DHUHUCAPTURE_QT_VALIDATION=ON \
  -DHUHUCAPTURE_PORTAL_VALIDATION=ON
cmake --build artifacts/portal --target portal_fault_tests portal_capture_probe \
  portal_shortcuts_fault_tests portal_shortcuts_probe portal_parent_probe
dbus-run-session -- artifacts/portal/portal_fault_tests
artifacts/portal/portal_fault_tests --no-bus
dbus-run-session -- artifacts/portal/portal_shortcuts_fault_tests
artifacts/portal/portal_shortcuts_fault_tests --no-bus
artifacts/portal/portal_shortcuts_fault_tests --disconnect-bus
dbus-run-session -- artifacts/portal/portal_shortcuts_fault_tests --shutdown
artifacts/portal/portal_shortcuts_fault_tests --shutdown --no-bus
python3 scripts/test-wayland-parent.py artifacts/portal/portal_parent_probe
```

依赖脚本从固定 commit 的干净私有目录检出，核对补丁哈希和 `git apply --check`，再用 Meson 构建 Qt6 backend。默认关闭其他 UI backend、文档、introspection、vapi 和上游测试；这只是本模块的受控编译输入，不代表跳过后续产品或桌面验收。已有工作目录不会被脚本 reset/clean，重建时选择新的空 workspace。

`portal_fault_tests` 使用真实 session bus 和一个不存在的服务。它验证 unavailable、立即取消、预取消、错误后 cleanup cancellation、Qt/GLib 同线程分发，并等待 GObject 最后引用通知及下一事件轮来证明迟到方法回调已释放引用、最终销毁栈已经返回。初始化与未授权 remote 的终态回调内实际分发 Qt 事件，检查 `shutdownFinished` 不会重入尚未退出的完成回调；未授权 remote 还在拒绝通知尚未投递时立即关闭控制器。它没有模拟 Portal 成功响应，也不证明任何桌面支持截图或录屏。`--no-bus` 验证缺失总线时返回错误而非 abort；这些负向测试不能替代实际 Screenshot/ScreenCast 授权中途取消、remote 刷新与活动会话排空的原生桌面验证。

`portal_shortcuts_fault_tests` 对真实缺失服务执行 GlobalShortcuts CreateSession 的未取消、立即取消、预取消及引用释放检查。`--disconnect-bus` 自行启动一个隔离的真实 dbus-daemon，在两个控制器获得连接之后实际终止它，要求进程继续运行、closed 在 Qt 线程分发、新请求返回 Unavailable。该测试没有构造假的 Portal 服务，不能证明活动桌面会话的清理或按键触发成功。

`--shutdown` 另验证真实初始化期间取消、初始化失败回调内关闭、排队拒绝通知和关闭后拒绝新请求。完成回调内实际嵌套分发 Qt 事件，要求 drain 不能早于当前回调返回；`--shutdown --no-bus` 在不可连接的真实总线地址验证同一顺序。固定 SDK 本地后者已通过；前者因环境禁止 dbus-daemon 创建 socket 尚未本地执行，仍需原生 CI。使用独立 SDK 时，dbus-run-session/dbus-daemon 必须保留宿主运行库环境，仅客户端加载固定 Qt/GLib 依赖，不能把应用的 LD_LIBRARY_PATH 注入宿主工具。

`test-wayland-parent.py` 启动自己拥有的 headless Sway/pixman compositor，使用私有 runtime 目录和最小配置，不接入用户的桌面或 bus。Ubuntu 24.04 的 [Sway 1.9](https://packages.ubuntu.com/noble/sway) 与 [wlroots 0.17.1](https://packages.ubuntu.com/noble/libwlroots12t64) 可提供该环境；Sway 固定源码 [`1a3cfc50`](https://github.com/swaywm/sway/blob/1a3cfc50c15124c2be18c025e609112ee15de4aa/sway/server.c) 明确创建 xdg-foreign-v2 全局对象。测试记录实际安装版本，不把包版本假定为运行证据。

探针先在真实 compositor 上导出正常句柄，然后协调器暂停自己的 compositor 子进程，检查取消、待决 parent 销毁、owner 销毁和原生元数据超时期间 Qt 的 10 毫秒 heartbeat 仍在运行。恢复后用异步 `wl_display_sync` 作为实际服务端屏障，检查迟到事件处理和 owner 释放。另一个模式验证成功导出后 parent 销毁的失效通知。外部 watchdog 在 probe GUI 卡住时仍会恢复 compositor 并结束测试；finally 清理及父进程死亡信号确保不会留下协调器拥有的暂停进程。probe 本身不接受 PID，也不会向任何进程发信号。结果、心跳、Sway 日志写入 `artifacts/tests/wayland-parent/`。这些检查只证明原生窗口导出接缝，不替代 Portal 授权、GNOME/Plasma、多屏硬件或 PipeWire 验收。

真实桌面上可以运行以下探针，并保存输出、桌面/Portal 版本和人工操作记录：

```sh
artifacts/portal/portal_capture_probe
artifacts/portal/portal_capture_probe --screenshot
artifacts/portal/portal_capture_probe --screencast
artifacts/portal/portal_capture_probe --screencast --cancel-after-ms 500
artifacts/portal/portal_capture_probe --screenshot --destroy-parent-after-ms 500
artifacts/portal/portal_shortcuts_probe
artifacts/portal/portal_shortcuts_probe --bind
artifacts/portal/portal_shortcuts_probe --bind --cancel-after-ms 500
artifacts/portal/portal_shortcuts_probe --bind --destroy-parent-after-ms 500
artifacts/portal/portal_shortcuts_probe --bind --close-parent-after-bind
artifacts/portal/portal_shortcuts_probe --bind --rebind-trigger CTRL+SHIFT+F12
artifacts/portal/portal_shortcuts_probe --bind --rebind-trigger CTRL+SHIFT+F12 --cancel-rebind-after-ms 500
artifacts/portal/portal_shortcuts_probe --bind --shutdown-after-bind
artifacts/portal/portal_shortcuts_probe --bind --shutdown-on-refresh
```

探针返回的 `remoteFdValid` 只证明打开了真实 FD，输出始终明确 `framesDecoded=false`；没有用空帧或 Mock 代替 PipeWire 采流。权限等待的 120 秒探针截止值只负责让验证失败退出，不作为生产请求“完成”条件。

Shortcuts 探针的 `--bind` 必须观察到真实 Activated 信号才通过，默认等待 15 秒，可用 `--hold-ms` 调整人工观察时间。成功重绑或取消重绑后会重新统计激活；取消重绑还检查旧列表与 session 保留。未收到真实激活不会用能力版本或绑定返回值替代通过结果。

两个明确的 shutdown 模式专门验证资源退出，不声称完成按键激活验收。`--shutdown-after-bind` 在实际授予绑定的回调内退出；`--shutdown-on-refresh` 要求用户随后在真实桌面设置中改变分配，等实际 ShortcutsChanged/ListShortcuts 回调再退出。它们都在回调内分发 Qt 事件确认尚未过早发出 shutdownFinished，并等待真实 drain。没有假的 Portal、注入的 ShortcutsChanged 或人为成功响应。所有探针模式在成功或失败终态后均等待 shutdownFinished；15 秒清理 watchdog 只报告失败。两个 active 模式当前仅已编译，真实 GNOME/Plasma 执行仍为 **not run**。

当前工作环境没有可声明的 GNOME/Plasma Wayland 桌面，因此接受/拒绝/取消授权、Portal 对话框 parent 销毁、session 外部关闭、restore token 失效、真实截图内容、多屏缩放、快捷键实际绑定/触发/重绑均是 **not run**。异步 parent 的真实 Sway 检查与实际桌面 Portal 验收分开记录；PipeWire 数据面另行记录其真实采流验证，音频、完整应用退出和发行包部署仍需后续实现与验收。具体编译和自动故障检查结果以对应提交的 CI 为准，不用本说明预先宣称通过。
