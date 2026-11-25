# 参与 huhucapture 开发

huhucapture 在 `main` 维护。修改围绕一个可观察的问题，说明改后的行为、数据保护边界和真实验证结果。先从 [README](README.md) 理解产品，再从 [文档地图](docs/readme.md) 选择所涉及的设计约定。

## 开始前确认什么

| 修改范围 | 主要约定 |
| --- | --- |
| 新模块、资源、线程或性能 | [架构与扩展](docs/architecture.md) |
| 文件、目录、编码和文档组织 | [工程约定](docs/conventions.md) |
| 保存、复制、录屏文件与历史清理 | [媒体历史](docs/media-history.md) |
| 图形、批注范围、备注和完整导出 | [批注设计](docs/annotations.md) |
| 设置、列表、字体、控件和交互 | [界面设计](docs/design.md) |
| 依赖、版本、构建和发行文件 | [构建与发布](docs/building.md) |
| Qt 完整应用、安装树和新架构更新 | [Qt 应用](docs/qt-application.md)、[Qt 打包](docs/qt-packaging.md)、[Qt 更新](docs/qt-updates.md) |
| 原生窗口、输入、显示和设备验证 | [验证边界](docs/validation.md) |
| 图标与翻译 | [品牌资源](docs/brand.md)、[语言约定](docs/languages.md) |
| 代码代理协作 | [AGENTS.md](AGENTS.md) |

问题报告提供程序版本、操作系统与桌面会话（Linux 还需说明 X11/Wayland、桌面环境）、DPI/显示器布局、复现步骤与实际结果。截图含私密内容时，可以换成相同尺寸和布局的复现底图。设计提议说明用户场景、预期行为及失败时如何保留结果。

## 实施边界

跨平台实施以 [Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5) 的工作包和依赖为准，[迁移状态](docs/qt-migration.md) 说明当前代码边界。目标是同一套 C++20 / Qt 6 界面、编辑器和服务，只有系统能力进入 Windows / Linux 适配。不要新增第二套平台业务或复制编辑器规则。

优先复用当前成熟实现，其次固定 Issue 引用的上游源码，再选择成熟库。纯规则在 `src/core` 和无平台头的编辑模型，系统资源在 `src/platform`，编码、图像与持久化在服务层，窗口负责交互协调。新增目标登记到 CMake；仍由现有 Windows 产品调用的文件也登记到其工程与 filters。依赖修改同步锁文件、源码归属、可重放补丁与许可。

现有 Windows 单 EXE 是迁移回归基线，P12 未验收前保留其构建；P13 才清除旧生产架构。Qt 的动态库部署与 Windows / Linux 完整安装包须走新的验收门槛，不能把旧单 EXE 的运行要求套用到新架构。

明确句柄、缓冲、图形和线程的拥有关系，停止工作线程后再释放资源。复制、保存、编码和归档不能用模糊的成功状态掩盖失败。媒体清理只影响应用管理的范围；编辑器、媒体文件和 Windows 剪贴板各有自己的生命周期。

保留稳定编号、完整备注和原图像素。对默认值、快捷键、帧率、格式、输入焦点或文件规则的可见改动，应同步说明行为与验收方式。功能特定规则见上表的主题文档，避免在多个地方维护同一套约束。

## 文档与命名

维护文档集中在 `docs/`，Markdown 文件名全小写、单词用短横线，语言后缀也小写。根目录标准发现文件和第三方许可保留规范名称。新文档加入 [地图](docs/readme.md)，同时修复所有相对链接，不在语言、资源或源码目录另建重复指南。

正文重点记录设计理由、目录职责、扩展边界和失败语义。函数签名、字段枚举和固定依赖版本直接链接源码或锁文件。当前行为写入主题文档；版本变化写入更新记录和 `releases/<tag>.md`；具体测试通过记录归对应提交的执行证据。

## 验证与提交

共享核心和 Qt / Portal 的 CMake 入口见 [构建与发布](docs/building.md)；LocalSend 的真实 TLS 测试见 [core 适配说明](docs/localsend-core.md)。固定工具链和配置必须与 CI 一致，不以本机另一个 Qt 版本代替私有 ABI 验证。

完整 Qt 应用使用 `qt-application-linux` / `qt-application-windows` 预设和相同的 `HUHUCAPTURE_BUILD_APP=ON` 生产目标。固定依赖恢复完成后，构建、测试、安装按 [Qt 打包](docs/qt-packaging.md) 执行。关闭 `BUILD_TESTING` 必须仍能构建、安装全部生产服务；只在测试模块发现 Python、Xvfb 和 D-Bus 测试启动器。跨平台正式产物须来自同一源提交，依次通过组件、完整应用、安装树、包内运行库、实际安装升级和 Issue 必要验收。

仍在维护的 Windows 回归入口为：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/test-windows.ps1 -Update
pwsh ./scripts/package.ps1
```

依赖与构建设置变化还运行 Debug。其他平台的 C++20 核心测试仅证明纯规则，不能报告 Windows 窗口、IME、DPI、GPU 或音频已通过。

提交说明包含问题、用户可观察的变化、实际命令和结果、未执行的边界。性能结论附同机同场景测量，无法测量时只描述减少的具体工作量。不要提交 `.deps`、`artifacts`、个人配置、IDE 状态或临时日志。

准备发布时，同步唯一版本源、显式声明和该版本说明。旧 Windows 基线继续使用原发行门槛；Qt 首版还须完成桥接、对应源码及四种完整包的同源聚合与实际验收。开发包和组件测试不能授权正式发布。不要手动改写二进制版本或混合来自不同构建的发行文件。

原生采流测试的桌面也必须可复现：[受控 Wayland 桌面](docs/validation-desktop.md)
固定 compositor、Portal backend 与构建输入，各自保留来源和许可证。不要把
它的私有库放入生产包，或让应用 PipeWire 插件进入系统 daemon。旧更新桥的
HTTP 传输样本、真实完整包下载与安装升级分别记录，不能互相替代；同样，
编译通过、离屏控件通过和实际原生桌面通过必须分别报告。
