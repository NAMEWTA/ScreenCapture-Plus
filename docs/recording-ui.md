# 共享录制界面

[`RecordingWindow`](../src/ui/qt/RecordingWindow.cpp) 是两平台唯一录制工具栏，使用现有 `ToolbarControls` 的 44 DIP 点击区域与 24 DIP 图标。窗口只发出开始、Save、Copy、Discard、关闭和输出重试意图；来源、编码、归档及真实输出回执由 [`RecordingController`](../src/app/RecordingController.cpp) 和 `RecordingSession` 持有。

默认 MP4 / 系统声开启 / 麦克风关闭。选择 GIF 清除两种音源；切回 MP4 不暗中恢复。开始点击只进入 Preparing，真正的 prepared 通知才启动单调时间显示。第一次成功停止意图固定，finalizing/archiving 不能被重复点击改成另一种结果。输出失败显示保留的完整路径并允许重试已归档媒体，不重录、不创建第二份归档；取消 Save As 保留原归档。

可见工具栏可以通过宿主激活并附带真实 Portal token。安全区域内的拖动和语言布局重排不能进入选区；工作时禁止独立 tooltip。若不能排除或安全放置，只有真实外部停止入口可用才隐藏工具栏，隐藏状态下的 activate 不将其重新展示。来源实际停止后恢复结果界面。细节与局限见 [控制窗口保护](recording-controls.md)。

`recording_window_tests` 驱动真实 QWidget、完整语言目录、格式/音源选择、第一次停止、输出重试、尺寸与安全区域重排。它不采集桌面；原生 GIF/MP4 probe 和实际 Wayland/X11/Windows 桌面验收分别核验真实捕获像素与完整媒体。
