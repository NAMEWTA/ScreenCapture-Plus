# 上游致谢与来源

huhucapture 基于 [xland / ScreenCapture](https://github.com/xland/ScreenCapture) 衍生开发。感谢 [xland](https://github.com/xland) 与上游贡献者提供原生 Windows 截图工具的基础实现，也感谢 [Ling](https://github.com/xland/Ling) 及相关开源组件的维护者。截图、钉图与绘图编辑、滚动截图、GIF / MP4 录屏、二维码识别、托盘、命令行入口和多语言能力，都建立在这些已有工作之上。

## 可以追溯的起点

本项目记录的继承快照为 ScreenCapture 提交 [`5e4713e2fbe333873abff911424a212923489ccd`](https://github.com/xland/ScreenCapture/commit/5e4713e2fbe333873abff911424a212923489ccd)，其提交说明为 `2.5.15`。可从该快照的 [ReadMe.md](https://github.com/xland/ScreenCapture/blob/5e4713e2fbe333873abff911424a212923489ccd/ReadMe.md) 了解原项目的功能与使用方式。固定链接用于追溯继承内容；后续维护应同时查阅上游当前代码，并记录真正引入的修复与版本。

Ling 为 v0.0.6 及更早 Windows 版本提供了界面基础；当前应用使用 Qt。实际构建采用的版本由 [依赖锁文件](../build/dependencies.lock.json) 统一管理，其他依赖及继承的 C 源码来源由 [第三方声明](../third_party/NOTICE.md) 记录。没有可靠记录的第三方原始版本，不通过猜测补写。

## 本项目的维护范围

huhucapture 由 [NAMEWTA](https://github.com/NAMEWTA) 独立维护，使用自己的产品名称、品牌资源、问题入口和发行版本。本项目增加或调整的范围包括矩形批注与说明合成、媒体历史归档、桌面设置与列表交互、工程模块边界，以及固定依赖和发布验证。产品当前行为以本仓库的 [README](../README.md) 为准；来源说明不意味着上游维护者参与或为本项目的后续修改背书。

维护时优先理解并复用已有实现。引入上游修复或其他项目代码，应记录源仓库、具体提交、适配理由和适用许可；只参考产品行为或设计规则时，也应说明参考范围，避免把设计借鉴写成源码移植。媒体历史的行为参考与设置页的视觉参考分别记录在 [媒体历史](media-history.md) 和 [界面设计](design.md) 中。

## 归属与分发

Qt 迁移继续直接复用本仓库的 `HistoryStack`、批注几何 / 排版、`ScrollStitch`、Windows GDI / DXGI / Media Foundation / WASAPI、quirc 和 gifski。模型抽取保留来源与行为，原 Windows 适配仍直接调用共享模型；不把换用 Qt 表述为重写或原创这些能力。

新增的 LocalSend core 固定到 `44f48f18a5503c43873570ff489d27ff1983e198`，只引入 Rust core，不引入 Flutter / Dart 运行时。原始文件 blob、Apache-2.0 许可和受控改动补丁随源码保存，详见 [LocalSend core](localsend-core.md)。Linux Portal 复用 libportal 0.11.0 与对应 Qt 后端，补丁只针对已检查的生命周期和异步接缝；精确提交、上游文件和 LGPL / GPL 许可见 [Linux Portal](linux-portal.md)。

Wayland 异步父窗口导出适配 KWindowSystem 6.32.0 固定提交 `870fed584663656db8b5e3d4ec1a7048b5eff25f` 中的两类导出对象，保留 Kai Uwe Broulik 的 LGPL-2.0-or-later 归属；未引入整套 KDE Frameworks。原始源码 blob、Qt 私有 ABI 边界及协议 XML 来源见 [来源清单](../third_party/kwayland-parent/upstream.md)。

窗口激活使用固定 Qt 6.12.1 自有 Wayland shell surface 的窄平台适配：只附上桌面给出的有效令牌，再调用 Qt 的正常激活请求，不额外引入 KWindowSystem 激活管理器或协议连接。实现与未授予焦点的边界见 [窗口激活](window-activation.md)。

Windows 已认领文件句柄到 Media Foundation 的桥接适配 Microsoft Corporation 的 Windows classic samples 提交 `434f6002bdf9cf9829406c3ff2b33387982d6168` 中 `XmlLiteReaderWriter` 的 `FileStream`，保留原 MIT 许可。它持有同一个文件对象，不按路径重新打开或截断；录制与编码仍复用本项目原后端。精确源码、改动和官方 MF 接口见 [句柄流来源](../third_party/windows-sdk-stream/upstream.json)。

共享设置继续复用旧配置、语言目录、按键名称及 Windows RegisterHotKey / Run 注册表行为。X11 被动抓取适配 QHotkey 固定提交 `4e3a244d87f1f7e741e1395f2ffe825f3a8ada45` 的实际源码，保留 Felix Barz 的 BSD-3-Clause 许可；用 checked XCB 接缝承接共享设置事务，不引入第二套 UI 或 QHotkey 运行时。原始文件校验、改动范围与既有 Ling 来源见 [QHotkey 记录](../third_party/qhotkey/upstream.md)，真实能力、恢复失败和自启动限制见 [设置控制器](settings-controller.md)。

Linux 软件媒体验证直接构建固定 FFmpeg 9.0.2 和 x264 API 164，并复用上游 mux / decode 示例的 API 流程。Fabrice Bellard、Stefano Sabatini 的 MIT 示例归属与依赖本身的 GPL 条款分别保留，详见 [软件编解码](software-codecs.md)。

Qt 按各模块的实际许可部署；现有 gifski 的 AGPL、libportal / KWindowSystem 的 LGPL 和软件编码组合的 GPL 条款都必须独立保留。新的 Qt 组合二进制与对应源码 / 构建说明尚未完成发行审查，不能根据仓库根 MIT 声明直接声称所有依赖都采用 MIT。

上游致谢是源码与分发说明的一部分，不随本项目名称、分支或贡献统计的展示方式改变。

- ScreenCapture 继承快照的 [应用资源](https://github.com/xland/ScreenCapture/blob/5e4713e2fbe333873abff911424a212923489ccd/Src/Res/Resource.rc) 声明 `Copyright (C) LiuXiaoLun 2023-2026`，本项目在第三方声明中保留该归属。
- 上游 [MIT 许可文本](https://github.com/xland/ScreenCapture/blob/5e4713e2fbe333873abff911424a212923489ccd/LICENSE) 中已有的 ThorVG 版权行保留在本项目根 [LICENSE](../LICENSE)；本项目新增工作的 NAMEWTA 声明与其并列。
- 各依赖保留各自的许可与版权。仓库根许可不替代第三方条款，便携发行包包含对应文本，具体来源与用途以 [NOTICE](../third_party/NOTICE.md) 为准。

对上游的感谢、当前项目的维护责任和实际代码归属应同时保持清楚。整理品牌、文档或仓库展示时，不删除继承代码的来源和许可说明，也不把上游工作表述为完全从零开发。

## GitHub Contributors 的展示边界

GitHub Contributors 按默认分支中的提交生成贡献统计；它与本项目实际继承的源码来源、版权及许可声明分别维护。统计页面出现或不出现某位作者，都不改变本页记录的上游归属。

GitHub 官方说明，强制推送、重写历史或删除提交后，贡献者展示和统计可能需要约 24 小时刷新；默认分支的正常推送会重置按提交 SHA 缓存的仓库统计。维护时应核对实际提交作者，并留出平台重新计算的时间，不为改变页面展示而伪造作者或使用错误的身份映射。刷新机制及排查入口见 [Contributors 文档](https://docs.github.com/en/repositories/viewing-activity-and-data-for-your-repository/viewing-a-projects-contributors) 和 [仓库统计 API 文档](https://docs.github.com/en/rest/metrics/statistics)。
