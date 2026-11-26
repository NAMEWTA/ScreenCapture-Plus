# 录制与滚动控制窗口的捕获保护

生产宿主为 `RecordingController` 和 `ScrollController`。两者都把 GUI 选择的真实 native surface 发布为 `Core::CaptureControls` 中的不可变快照，worker 只读取值，不接触 QWidget，不等待 GUI 每帧确认。窗口更换 ID 或销毁会把快照清空，关闭新的帧受理并保留已有结果。

Windows 优先使用明确支持 `WDA_EXCLUDEFROMCAPTURE` 的系统能力并读回 affinity。没有该能力时，只有仍然有效的逐显示器 source-pixel / DIP 绑定才能给出选区外候选位置；每帧读回前后还核对真实物理边框。不能把旧系统接受相同数值后产生的黑矩形叫作排除。

原生 X11 没有通用 display-affinity。`X11Capture::captureGuarded` 在短 server grab 内检查 owned window、viewable 状态、root-relative geometry、合法 frame extents 与实际 SHAPE 区域，再执行原有 SHM / GetImage 读回。grab 不跨编码、等待或 GUI 回调。GUI 的安全区域约束便于用户移动/缩放，实际 native guard 才决定是否受理像素。布局或快照变化会中止，而不是覆盖画面。

没有安全位置时，宿主必须先证实外部停止入口可用。控制窗口在整个采集期间保持隐藏，单纯 activate 不改变保存/丢弃意图、不让它重新进入画面；停止能力撤销会停止采集并保留结果。tooltip 是独立 native surface，开始前一并隐藏，录制工具栏工作时不创建 tooltip。

Wayland 不把授权 source 的像素推算为全局位置。选择器和控制窗口实际 unexpose 后，`ScreenCastSource::refresh()` 沿同一次授权重建数据连接，取得实际替代首帧。源 identity、格式、crop、transform 与选区尺寸必须保持一致；冻结新 generation 后，等借用 `refreshed` 信号返回才启动一次性消费者。该协议和 lease / producer 完成屏障不等于所有 compositor 的 presentation 保证，首帧像素仍需真实桌面 probe 验证。

保护逻辑复用来源、许可证及审阅的协议依据在 [`recording-controls/upstream.json`](../third_party/recording-controls/upstream.json)，X11 读回来源在 [`ffmpeg-xcbgrab/upstream.md`](../third_party/ffmpeg-xcbgrab/upstream.md)。组件编译、offscreen toolbar、真实 Xvfb SHAPE/geometry、Windows DWM 和 Wayland first-frame 像素各自是不同证据，不能互相替代。
