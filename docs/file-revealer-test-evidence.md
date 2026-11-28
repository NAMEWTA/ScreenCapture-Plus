# 文件定位测试的 Shell 生命周期

生产 `FileRevealer` 的 Windows 结果表示 `ShellExecuteExW` 或
`SHOpenFolderAndSelectItems` 已经受理真实请求。这个结果不保证 Explorer 已经
完成导航，也不能作为测试删除刚打开目录的时点。生产服务保持原有异步契约。

## 已观察到的跨测试遮挡

开发提交 `1f516bf5799551af1776839f56a21a16e652a4c1` 的 Windows Debug Qt
任务记录 63/66 通过。`file_revealer_windows_desktop` 在 1.62 秒处通过，但当时
只检查了系统 API 结果；函数退出会立即销毁 `QTemporaryDir`。

同一任务随后保存的 `scroll-initial-final-desktop.png` 显示真实 Explorer
“Location is not available” 对话框，其路径正是先前文件定位测试的临时目录。
`scroll-initial-presentation.json` 中，实际与期望选区均为 220×180，DPR 为 1，
原生 DPI 为 96。首末读回中不匹配区域均为 `[135,119,85,61]`，共 5185 像素；
右下取点的窗口归属为该对话框的 `DirectUIHWND`。完整桌面截图与这个矩形一致，
因此本次长截图的初始像素失败由真实窗口遮挡解释，并非缩放或采集坐标偏差。

原始材料来自 GitHub artifact `15026275252`，ZIP SHA-256 为
`3e9756cab2e12fc5fcf8648c34bb76cce2ee634ed55cf57c109c78c8e8a46307`。
保留的相对路径是
`artifacts/cmake/qt-validation/scroll-render/Debug/scroll-initial-*.png`
与同目录的 `scroll-initial-presentation.json`。同轮截图测试也报告可见像素
不符，但没有相同初始帧证据，不能把推断当作它的独立根因证明；工作区崩溃另行审查。

## 测试完成条件

仅 Windows `file_revealer_tests --real-desktop` 使用以下真实 Shell 观察。
默认文件校验模式和生产服务没有新增自动关窗行为。

1. 在实际打开请求之前，记录已存在的 Shell 窗口，并关闭临时目录的自动删除。
2. 通过实际 `IShellWindows`、`IShellBrowser` 和当前 `IFolderView2` 获取已显示
   目录的 PIDL，再解析其完整文件系统路径；不根据标题或 `LocationURL` 推定导航完成。
3. 要求目标窗口及其 Shell view 均实际可见。定位文件还必须通过
   `IFolderView2::GetSelection(FALSE)` 观察到唯一选中项，完整路径与已确认媒体一致。
4. 每次打开都独立完成观察及退出，再提交下一次请求。只对测试开始后出现、
   当前仍显示精确测试目录的 `IWebBrowser2` 对象调用 `Quit()`；已有窗口或在已有
   窗口中新开的标签页不会被当成可关闭的测试窗口。
5. 等待该真实 HWND 消失且从 Shell 窗口集合注销，并最终核对所有已提交请求均已观察、所有测试目录均不再
   有对应的 Shell view，才恢复目录自动删除。关闭也不是以 API 接受即判完成。

单个观察有 20 秒上限，并持续分派真实 Qt/Windows 事件。CTest 的外层限时需要
容纳两次打开、选中观察和真实关闭；不以固定等待时间当作成功条件。失败时记录
观察到的 HWND、目录和选择，并尝试关闭精确匹配的测试对象。未确认完成的请求
保留临时目录，以免它在测试结束后才到达 Explorer 并引发新的错误对话框。
没有结束 Explorer 进程、关闭任意前景窗口、遮盖错误窗口，或放宽截图的逐像素断言。

## 公开 API 来源与验证范围

本测试沿用项目 `platform/windows/ComApartment.h` 和 WRL 所有权封装，参考
Microsoft 的 [Explorer 信息查询示例](https://devblogs.microsoft.com/oldnewthing/20040720-00/?p=38393)
确认公开接口链，没有复制 Shell 实现。具体契约见
[IShellWindows::Item](https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nf-exdisp-ishellwindows-item)、
[QueryActiveShellView](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellbrowser-queryactiveshellview)、
[GetSelection](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifolderview2-getselection)
和 [IWebBrowser2::Quit](https://learn.microsoft.com/en-us/previous-versions/aa752140(v=vs.85))。

本次在固定 Qt 6.12.1/GCC 14 环境完成 Linux C++20 严格编译
（`-Wall -Wextra -Wpedantic -Werror`），默认文件校验测试实际通过。
新增 Windows 分支尚待 MSVC 编译及原生 Explorer CI；Linux 编译会排除它，
因此不构成 Windows Shell 生命周期通过的证据。
