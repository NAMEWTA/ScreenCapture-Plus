# 剪贴板取消、实际排空与应用退出

本页补充 [系统桌面服务](desktop-integration.md) 的退出边界。它说明请求完成和
真实后台任务完成为何需要分别追踪，不重新定义格式、归档或平台接收协议。

## 取消回执不等于工作线程已经结束

`SystemClipboard` 在真正发布历史文件前，用 QtConcurrent 再核对一次读租约对应
的文件身份。取消可以立即把该请求标记为 `Cancelled`，但不能中止一个已经运行
或仍在 Qt 线程池排队的文件操作。移除 `QFutureWatcher` 或清零 UI token 并不会
让该工作线程结束，也不能让它提前释放所持的历史读租约。

服务保留 validation watcher，直到真实 `finished` 回调。取消后 `busy()` 仍为
真；此时的新发布返回有且仅有一次 `Busy` 回执，不再堆叠新的校验任务。原任务
完成后，释放实际租约并通过 `busyChanged(false)` 通知宿主。这个状态事件很必要：
取消回执可能已经送达，实际排空时不会再给旧 token 发第二次 `finished`。

Wayland 文件发布的异步 Documents Portal 能力查询同样保留至真实回调，受既有
三秒期限约束。取消后不继续身份检查或发布，不将等待能力查询解释为成功。
平台发布过程中可能同步触发 Qt 通知和应用代码；服务也追踪这层仍在执行的
原生发布，直到栈上保留的读租约与内容真正退出作用域。

## 正常退出屏障

应用先停止可能产生新复制请求的截图、录制和媒体任务，再在 GUI 线程调用
`SystemClipboard::shutdown()`，等待 `shutdownFinished` 后才销毁该服务或离开
Qt 事件循环。该接口停止新请求准入，并取消当前发布请求；它不清空系统剪贴板。

准入边界由短临界区保护。此前已经接受但尚未送达 GUI 的错误线程请求仍有非零
token，必须送达其真实 `WrongThread` 回执。之后的新调用返回零，并消费它自己的
未使用准备对象，不虚构一个成功或取消通知。

每个已接受 token 从准入开始，直到 `finished` 的所有直接监听者返回，始终占有
一个待决回执。监听者在处理回执时调用 shutdown，甚至进入重入事件循环，也不能
提前得到 `shutdownFinished`。最终通知只在以下条件同时满足后排入事件循环：

- 没有活动发布、真实 validation future 或待决 Portal 查询。
- 没有仍在栈上执行的原生发布。
- 所有已接受 token 的唯一终态回执均已送达并返回。

正常路径不使用 GUI 线程上的 `waitForFinished`、`waitForDone` 或阻塞等待来模拟
排空。窗口关闭不销毁应用拥有的服务。异常销毁对象时，独立 worker 仍只访问它
自己持有的租约，不访问已经销毁的窗口；这不是正常退出屏障的替代方式。

## Linux 剪贴板持有与进程退出

`shutdownFinished` 仅证明本服务的工作与回执排空。它不证明另一个进程已经保存
复制内容，也不证明应用退出后剪贴板仍可用。Linux 的 Qt selection/data source
仍由 QApplication 持有；停止该服务不会主动删除它。

已阅读的固定 [Qt XCB 实现](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/xcb/qxcbclipboard.cpp)
在析构时可尝试向实际剪贴板管理器请求 `SAVE_TARGETS`，但公共
`QClipboard` API 没有提供这次交接的成功结果。Wayland 的普通数据源也没有
通用的永久保存确认，具体所有权见固定
[Qt Wayland clipboard](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/wayland/qwaylandclipboard.cpp)
与 [data device](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/plugins/platforms/wayland/qwaylanddatadevice.cpp)。
因此宿主不能把当前 selection 确认、服务排空或普通
`ownsClipboard()` 当作“退出后永久保存成功”。在无法确认交接时，宿主需要给出
同一套 Qt 可见窗口，让用户保持窗口以便粘贴，或明确选择退出。相关正式入口和
各平台真实验证仍以应用集成结果为准。

## 实际验证范围

[并发生命周期测试](../tests/qt/system_clipboard_shutdown_tests.cpp) 使用真实 Qt
线程池、生产服务、完整 PNG 编解码、实际归档文件与读/删除租约。它在一个真实
QtConcurrent 任务上设置有界调度屏障，让生产校验 future 确实处于未完成状态，
验证取消后仍忙碌、重试不产生无界任务、实际完成后可再次使用，以及 shutdown
等待所有任务与回执。另验证跨线程准入、重入回执和每个 token 只有一个终态。
测试没有替换文件操作或构造虚假的剪贴板成功结果。

该程序可用 `offscreen` 检查异步生命周期，因为上述分支在原生发布前取消，
不宣称系统接收端已收到内容。本地 Qt 6.12.1 / Linux amd64 已实际构建、运行
这一程序，并运行既有无桌面后端的明确拒绝测试。Windows、实际 X11/Wayland
接收端、剪贴板管理器接管以及完整产品退出仍需各自的真实 CI 或桌面验收。
