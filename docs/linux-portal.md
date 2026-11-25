# Linux Portal 控制面

本模块是 Issue #5 的 P00/P05 前置工作。它提供真实 Portal 能力查询、截图请求、ScreenCast 授权与 PipeWire remote 文件描述符交接；没有实现 PipeWire 视频帧消费、音频、Linux 编码器或正式 Qt 截图界面。编译通过、故障测试通过和桌面采集验收必须分别记录。GlobalShortcuts 的实际绑定及取消也仍是 P00 的独立未完成项。

## 模块边界

入口为 [PortalCapture](../src/platform/linux/portal/PortalCapture.h)，实现位于同目录。CMake 目标为 `huhucapture_portal`，仅 Linux 的 `HUHUCAPTURE_PORTAL_VALIDATION=ON` 启用，并要求 `HUHUCAPTURE_QT_VALIDATION=ON`。公共头只有 Qt/标准库类型，不向编辑模型暴露 `XdpPortal`、GObject、GVariant 或 PipeWire 类型。

- 直接链接 libportal 的 Screenshot、CreateSession、SelectSources、Start、Session.Close 和 Qt parent helper。仅用 GIO 查询公共 `Properties.GetAll` 能力，不另写一套 Portal 请求协议。
- Screenshot 成功返回 Portal 给出的有效本地文件 URI。它不代表图片已经解码、导入历史或复制到剪贴板；那些操作由共享媒体服务完成。
- ScreenCast 成功必须经过授权、非空 stream 元数据解析和真实 `OpenPipeWireRemote` FD 获取。`takePipeWireFd()` 只转移一次 FD，调用方负责关闭或将其所有权交给 PipeWire。
- `PortalStream` 的 position/size 是 compositor 坐标，不能当作实际视频像素或 DPI 比例。后续 PipeWire 层必须依据协商格式读取像素尺寸、stride、buffer 类型与时间戳。
- 不使用 X11 截图代替 Wayland 授权。Qt 的 xcb/Wayland 平台名仅作为环境记录；可用能力来自当前 Portal 的真实属性与请求结果。

## 线程、取消和资源所有权

控制器的构造、使用和销毁均在 QApplication 线程。运行时要求 Qt 的 GLib dispatcher，并检查 thread-default context 与全局 default context 一致。GLib async 回调、Qt parent 导出、取消、session close 均在此线程执行；不创建第二个 GMainLoop，不从其他线程迭代同一个 context。Qt 6.12.1 的 [dispatcher 实现](https://github.com/qt/qtbase/blob/v6.12.1/src/corelib/kernel/qeventdispatcher_glib.cpp) 是该约束的来源。

libportal 0.11.0 的 `xdp_portal_initable_new()` 内部仍调用同步 `g_bus_get_sync()`。控制器只在 GTask worker 上构造它，完成后把对象交给 Qt 线程。没有使用会在 session bus 缺失时 abort 的 `xdp_portal_new()` 或 `XdpQt::globalPortalObject()`。GUI 线程不等待 join 或同步 DBus 请求。

每次授权拥有自己的 cancellable、请求编号、parent 连接和回调状态。parent 在请求完成前销毁会取消该请求；迟到回调仍完成原请求并清理资源，不访问已销毁 QObject。控制器销毁后，异步回调保留清理状态但不再发 UI 信号。应用退出编排仍应先取消待决请求、等待终态回调，再结束事件循环；不能先停掉整个 dispatcher 后把异步资源清理算作完成。

成功的 ScreenCast session 由录制服务所属控制器继续持有，授权 parent 销毁不会撤销已经交付的录制会话。因此控制器应归应用服务或录制会话拥有，不能作为短命授权窗口的 QObject 子对象。后续录制服务应先停止 PipeWire/编码消费者，再调用 `closeScreenCast()`。外部 session 关闭或 Portal 服务消失会发出明确错误，并释放控制器仍持有的 FD。

取消与失败不依赖超时伪装成完成。实际 `Request.Close`、GTask 终态和回调资源释放在受控 libportal 补丁中完成。GUI 调用必须遵循上述单 context 约束；本补丁不承诺从任意线程同时调用 cancellation handler 的扩展语义。

## 结果和能力语义

`PortalFailure` 保留错误 domain、native code、remote DBus error 与消息。取消、parent 销毁、明确拒绝、服务不可用、能力不支持、忙碌及其他失败分别表示。Portal `Response=1` 本身表示取消，某些桌面将用户拒绝也表示为这一值；不能仅凭它声称区分“拒绝”与“取消”。只有明确的权限错误才映射为 Denied；`Response=2` 保留一般失败语义。

截图和 ScreenCast 的版本与失败分别记录，单个接口存在不代表所有接口可用。所选 source/cursor 模式必须被当前 Portal 广告支持；旧接口缺少 cursor 控制或 v4 persistence 时不会悄悄忽略调用方选项。失效 restore token 按真实错误返回，后续 UI 决定是否重新请求授权。

## 固定上游与受控补丁

依赖版本、源码 SHA 和补丁 SHA 以 [依赖锁](../build/dependencies.lock.json) 为唯一构建输入。目标基线为 libportal 0.11.0，提交 `fc4d62cfe131e8494c82f597f92b2234d584c367`；Qt parent 库必须与应用的 Qt 6.12.1 及其 private headers 同源构建。

实际阅读的上游实现包括：

| 源码 | 用途 |
| --- | --- |
| [screenshot.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/screenshot.c) | Screenshot/Response/取消/parent 导出 |
| [remote.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/remote.c) | CreateSession、SelectSources、Start、FD 与 stream 元数据 |
| [session.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/session.c) | session 注册、关闭、引用拥有关系 |
| [portal.c](https://github.com/flatpak/libportal/blob/fc4d62cfe131e8494c82f597f92b2234d584c367/libportal/portal.c) | session bus 初始化及失败行为 |
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

被修改的原文件版权归 Matthias Clasen；libportal Qt helper 还保留 Georges Basile Stavracas Neto、Jan Grulich、Neal Gompa 和 GNOME Foundation 等原始归属。libportal 和本补丁按 LGPL-3.0-only 使用，附带原始 [COPYING](../third_party/patches/libportal/COPYING) 与其所引用的 [GPL v3 文本](../third_party/patches/libportal/COPYING.GPL3)。后者来自 Qt 6.12.1 的 `LICENSES/GPL-3.0-only.txt`，原 blob `94a9ed024d3859793618152ea559a168bbcbb5e2`。不能将新增库重新标为 MIT；正式打包还必须提供匹配的动态库、源码、受控补丁和许可记录。

## 构建与检查入口

Ubuntu 24.04 需要 `build-essential meson ninja-build pkg-config libglib2.0-dev libglib2.0-dev-bin dbus`，GIO/GLib ≥2.80。同一 Qt prefix 必须提供 Core/Gui/Widgets 及版本匹配的 private headers；不要让 pkg-config/qmake 选择系统的其他 Qt。

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
cmake --build artifacts/portal --target portal_fault_tests portal_capture_probe
dbus-run-session -- artifacts/portal/portal_fault_tests
artifacts/portal/portal_fault_tests --no-bus
```

依赖脚本从固定 commit 的干净私有目录检出，核对补丁哈希和 `git apply --check`，再用 Meson 构建 Qt6 backend。默认关闭其他 UI backend、文档、introspection、vapi 和上游测试；这只是本模块的受控编译输入，不代表跳过后续产品或桌面验收。已有工作目录不会被脚本 reset/clean，重建时选择新的空 workspace。

`portal_fault_tests` 使用真实 session bus 和一个不存在的服务。它验证 unavailable、立即取消、预取消、错误后 cleanup cancellation、Qt/GLib 同线程分发，并等待 GObject weak-finalize 来证明迟到方法回调已释放引用。它没有模拟 Portal 成功响应，也不证明任何桌面支持截图或录屏。`--no-bus` 验证缺失总线时返回错误而非 abort。

真实桌面上可以运行以下探针，并保存输出、桌面/Portal 版本和人工操作记录：

```sh
artifacts/portal/portal_capture_probe
artifacts/portal/portal_capture_probe --screenshot
artifacts/portal/portal_capture_probe --screencast
artifacts/portal/portal_capture_probe --screencast --cancel-after-ms 500
artifacts/portal/portal_capture_probe --screenshot --destroy-parent-after-ms 500
```

探针返回的 `remoteFdValid` 只证明打开了真实 FD，输出始终明确 `framesDecoded=false`；没有用空帧或 Mock 代替 PipeWire 采流。权限等待的 120 秒探针截止值只负责让验证失败退出，不作为生产请求“完成”条件。

当前工作环境没有可声明的 GNOME/Plasma Wayland 桌面，因此接受/拒绝/取消授权、parent 销毁、session 外部关闭、restore token 失效、真实截图内容与多屏缩放均是 **not run**。GlobalShortcuts、PipeWire 帧/音频、FFmpeg、完整应用退出和发行包部署仍需后续实现与验收。具体编译和自动故障检查结果以对应提交的 CI 为准，不用本说明预先宣称通过。
