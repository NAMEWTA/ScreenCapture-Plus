# 原生长截图探针的呈现就绪与证据

`scroll_capture_tests` 的正向路径使用真实 Qt 窗口绘制独立像素页，经唯一的 `DesktopCapture` 读取，再由生产 SendInput/XTest 路径滚动。选区和最终整张长图均逐像素比较；测试不以模拟帧替换原生输入。

## 已观察到的差异

在 [c71 对应 Windows Qt 验证任务](https://github.com/NAMEWTA/huhucapture/actions/runs/45612551089/job/139199548891) 中，64 个测试有 62 个通过；长截图在 0.30 秒处的初始页面像素检查失败，还没有开始滚动。另一项窗口激活测试记录了前景窗口被其他进程接管。前景变化是否同时影响了长截图，不能由这两条日志直接推定，因为原探针尚未保存初始失败帧。

同轮已下载的 Linux Debug `LastTest.log` 实际记录 72/72 通过，其中 `scroll_capture_native` 通过时间为 1.31 秒，命令为真实 Xvfb/xcb、1024×768 桌面。这份证据不覆盖 Windows、Wayland 或物理混合 DPI。

源码对照发现了测试就绪条件的不一致：已有 `tests/qt/screen_capture_tests.cpp` 用有界真实读回观察页面实际像素，而首批长截图探针只等待 `paintEvent` 计数不为零。该计数甚至可能包含内容初始化前的空窗口绘制。新探针分别记录总绘制次数和实际像素内容的绘制次数。

## 为什么不能只等待绘制事件

已完整阅读固定 Qt 6.12.1 的 [QWindowsBackingStore 源码](https://github.com/qt/qtbase/blob/v6.12.1/src/plugins/platforms/windows/qwindowsbackingstore.cpp)，对应 Git blob `384743a855478213117a4f7874e546d84ff5051f`。普通窗口 `flush()` 使用 GDI `BitBlt` 与窗口 DC；透明无原生边框窗口使用 `UpdateLayeredWindowIndirect`。一个 Qt 绘制或 expose 事件不构成整个桌面已完成合成的回执。

Microsoft 对 [DwmFlush](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush) 的说明也仅涉及调用方已排队的 DirectX 更新，并明确不刷新整个会话的渲染批次。因此本修复没有向正式 GUI 或采集服务加入一个未经验证的“全桌面完成”假设。

## 测试调整

初始化仍先要求窗口真实 expose、真实内容已经绘制。随后复用既有截图探针的 5 秒呈现就绪上限，每轮都调用同一个生产 `DesktopCapture` 获取实际桌面帧，核对最新来源身份、实际屏幕绑定及缩放，再重新计算选区。

只有整个选区与独立页面期望完全相等时，才将这一张实际快照交给最终 `ScrollController`。这里没有像素容差、固定延时、固定丢帧数或替换像素。达到时限仍不相等就失败。最终长图与独立页面的逐像素断言、真实滚轮事件要求、取消保存后的图像保留和退出顺序均保持。输出通过真实 `MediaWorkspace`，第一次显示实际共享 `QFileDialog` 后取消，第二次重新保存；成功后分别读取持久归档和用户导出的 PNG，逐像素比对同一独立页面。没有保留第二套 ScrollWorkspace 或 ScrollCaptureSession 来绕过最终控制器。

## 失败前保留的材料

只要成功取得实际原生帧，就在初始像素断言之前保留下列文件，故该断言失败时仍可查看原始证据：

- `scroll-initial-first-desktop.png`、`scroll-initial-first-actual.png`、`scroll-initial-first-expected.png`：第一次读回的完整桌面、真实选区及独立期望。
- `scroll-initial-final-desktop.png`、`scroll-initial-final-actual.png`、`scroll-initial-final-expected.png`：最后一次读回的相同材料。
- `scroll-initial-presentation.json`：首末读回的 Qt 几何、页面原点、来源 ID、屏幕绑定、DPR、原生桌面原点、图像格式、全部不匹配像素数、首个不匹配值及其包围范围。

Windows 同时记录请求前后实际前景窗口与线程焦点、进程 ID、窗口类、窗口和客户区范围、客户区物理原点、DPI、DWM 扩展边框和 cloak 状态，以及选区三点当时的真实窗口归属。DWM 时序查询只作为观察证据，不用作像素来源时间戳或呈现放行条件。请求前后观察也不是采集 worker 内部同一瞬间的证明。

每轮保留的摘要最多 64 项，超出时保留前 63 项和实际最后一项，并单独记录真实尝试总数。完整原生帧只存首末两份，避免诊断本身无限积累。

当前整合探针已经严格编译，但仍需在整合提交上重新运行原生 CI；上述旧轮结果不替代本次控制器与输出路径的验证。新增 Windows 诊断使用 `user32` 和 `dwmapi`，沿用现有 Windows 采集目标的真实系统链接项。
