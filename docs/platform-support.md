# 平台支持

| 平台 | 构建与安装目标 | 采集入口 |
| --- | --- | --- |
| Windows x64 | Windows 10 1809 及以上，安装器或完整便携 ZIP | Windows 原生屏幕采集，Media Foundation 视频编码与 WASAPI 音源 |
| Linux amd64，X11 | Ubuntu 24.04 构建基线，DEB 或 AppImage | 真实 X11 根窗口采集，在应用中选择区域或窗口 |
| Linux amd64，Wayland | 同一 Linux 软件包，使用当前桌面会话的 Portal 与 PipeWire | 由桌面显示授权与来源选择，应用使用获准的截图和帧流 |

Windows 10 1803 可以继续使用 [Windows v0.0.6](https://github.com/NAMEWTA/huhucapture/releases/tag/v0.0.6)。安装、便携目录与旧配置迁移见 [下载与运行](../README.md#下载与运行)。

## Wayland 桌面能力

应用根据实际 Portal 能力启用截图、录制和全局快捷键。全局快捷键未获授权时，可以从托盘或应用启动入口发起截图。长截图可在授权的来源中手动滚动。录制控件隐藏时，只有成功注册的停止操作才会作为可用停止入口。窗口位置、来源选择和输入控制由当前桌面负责。

拒绝或取消授权会结束请求。Portal 或 PipeWire 不可用时，界面报告失败并释放请求。涉及应用内采集状态与 Portal 兼容的工程边界见 [Linux Portal](linux-portal.md) 和 [录制控件](recording-controls.md)。

## 验证范围

自动化运行包含 Windows 采集与编码、Ubuntu X11 桌面，以及受控 Sway 的授权和帧流场景。具体运行范围以对应提交的测试产物为准。实体混合 DPI 双屏、真实音源、系统输入法、GNOME 与 Plasma 的桌面组合及长时录制需要独立运行记录。见 [验证边界](validation.md)。
