# 编辑工具设置的兼容与所有权

共享 Qt 编辑器使用原有 `Setting` / `ToolSub` 的 `toolPin` 配置，不创建第二份配置文件。应用在 `MediaWorkspace` 创建后、打开任何编辑窗口前调用 `setEditorSettings(SettingsStore&)`，传入已经读取的同一个配置实例。该实例必须比媒体工作区活得更久；重复绑定同一实例是幂等操作，更换实例或在已有编辑窗口后首次绑定会被拒绝。

`src/app/EditorPreferences.cpp` 是旧 JSON 键与现有 `EditorToolStyle` 的薄适配。字号、线宽、半径范围及调色板继续来自 `src/editor/ToolSettings.h`，形状、几何、文档历史及文字排版规则不在配置适配层复制。旧 Windows `ToolSub` 仍使用同一张范围和颜色表。

## 保留的键和单位

数值均为逻辑像素。形状创建时仍由现有编辑器转换为原图像素；不能把窗口缩放后的尺寸写回配置。`colorIndex` 是九色调色板的下标，不是颜色整数。

| `toolPin` 分组 | 数值键与范围（默认） | 布尔键（默认） | 调色板 |
| --- | --- | --- | --- |
| `rect` | `width` 1–26（2） | `fill` false | `colorIndex` |
| `ellipse` | `width` 1–26（2） | `fill` false | `colorIndex` |
| `arrow` | `width` 1–16（3） | `fill` true | `colorIndex` |
| `number` | `radius` 6–86（16） | `fill` true | `colorIndex` |
| `line` | `width` 1–60（12） | `semiTransparent` false | `colorIndex` |
| `text` | `fontSize` 10–60（20） | `bold` false、`italic` false | `colorIndex` |
| `mosaic` | `width` 18–68（28） | `rect` false | 不修改原有颜色字段 |
| `eraser` | `width` 18–68（28） | `rect` false | 不修改原有颜色字段 |

范围批注工具没有原生 `ToolSub` 样式设置，新编辑器不为它生成配置字段；已有未知的 `annotation` 分组仍原样保留。普通文字只读取 `text.fontSize`。旧交互中可能出现的跨工具 2 px 线宽污染不会被恢复；明确保存的字号按原有 10–60 范围读取，无该字段时使用 20，而不是借用其他工具的线宽。

读取时保留有效的小数尺寸，不因整数滑块的显示而重写。越界数值按共享范围夹取；错误类型使用既定默认值。调色板下标保留旧版本有效范围内的 float 转换和向零取整，但先验证范围，避免将非法数值转换为整数。所有这些读取都只改变视图的有效值，不自动修正磁盘上的原始内容。

## 写入、多个窗口与失败

新窗口加载全部已保存工具设置；切换工具时再次读取该工具的最新值，与旧 `ToolSub::beginTool` 一致。因此一个贴图窗口的调整会在另一窗口下次选择同一工具时可见。已经画出的形状不会随偏好变化而重新设置样式；当前正在输入的文字仍按现有编辑器交互立即应用字号、颜色或粗斜体。

用户操作真实工具控件、或现有形状滚轮更新工具尺寸时，`toolStyleChanged` 通知同一媒体工作区。适配层比较该窗口上一次有效样式，只修改实际变化的已知属性；一次操作中的多个属性通过同一次 `SettingsStore::setValue` 提交。其他工具、未知根字段、未知工具字段及未调整的非规范值均保留。已有 `toolPin` 或工具分组如果不是对象，会拒绝写入并保留原值，不以空对象覆盖。

配置文件锁、外部修改检测、首次迁移的逐字节备份和文件提交仍全部由 `SettingsStore` 执行。视图不会自行打开配置文件，也不会另建配置实例绕过写入失败。失败时保留本次编辑状态和已画内容，通知用户该偏好未保存；下一次重新读取以实际提交的配置为准。工具事件内不直接调用宿主错误处理：失败在下一 GUI 轮合并通知，并计入 `MediaWorkspace::busy()`，防止一次性模式在通知送达前退出。

## 验证边界

`tests/qt/editor_preferences_tests.cpp` 使用真实 UTF-16 LE 原配置、`SettingsStore`、`MediaWorkspace` 与 `EditorWindow`。覆盖恢复全部工具值、实际工具按钮/滑块调整、跨窗口读取与独立字段更新、关闭重开、独立磁盘重载、新配置首次写入、原文件精确保留备份、外部写入冲突和错误父字段保留；同时比较现有形状输出，验证偏好变化不修改已画内容。保存失败的宿主回调同步销毁工作区和编辑器，也有实际控件事件覆盖。

本批在本地固定 Qt 6.12.1 与 GCC 14 下完成新增适配器、媒体工作区和测试的 C++20 `-Wall -Wextra -Wpedantic -Werror` 编译，三组新增实际文件/Qt 控件测试通过；相关 `qt_editor_components` 和 `qt_media_workspace` 回归 2/2 通过。组件运行使用 `offscreen`，没有将其解释为物理鼠标、混合 DPI、多显示器或窗口管理器测试；Windows 编译与桌面运行仍以对应 CI/实际环境结果为准。
