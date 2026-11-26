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
| 原生窗口、输入、显示和设备验证 | [验证边界](docs/validation.md) |
| 图标与翻译 | [品牌资源](docs/brand.md)、[语言约定](docs/languages.md) |
| 代码代理协作 | [AGENTS.md](AGENTS.md) |

问题报告提供版本、Windows 版本、DPI/显示器布局、复现步骤与实际结果。截图含私密内容时，可以换成相同尺寸和布局的复现底图。设计提议说明用户场景、预期行为及失败时如何保留结果。

## 实施边界

优先复用已有能力和平台组件，保持原生单 EXE。纯规则在 `src/core`，平台资源在 `src/platform/windows`，编码、图像与持久化在服务层，窗口负责交互协调。新增文件登记到工程与 filters；依赖修改同步锁文件与合法许可。

明确句柄、缓冲、图形和线程的拥有关系，停止工作线程后再释放资源。复制、保存、编码和归档不能用模糊的成功状态掩盖失败。媒体清理只影响应用管理的范围；编辑器、媒体文件和 Windows 剪贴板各有自己的生命周期。

保留稳定编号、完整备注和原图像素。对默认值、快捷键、帧率、格式、输入焦点或文件规则的可见改动，应同步说明行为与验收方式。功能特定规则见上表的主题文档，避免在多个地方维护同一套约束。

## 文档与命名

维护文档集中在 `docs/`，Markdown 文件名全小写、单词用短横线，语言后缀也小写。根目录标准发现文件和第三方许可保留规范名称。新文档加入 [地图](docs/readme.md)，同时修复所有相对链接，不在语言、资源或源码目录另建重复指南。

正文重点记录设计理由、目录职责、扩展边界和失败语义。函数签名、字段枚举和固定依赖版本直接链接源码或锁文件。当前行为写入主题文档；版本变化写入更新记录和 `releases/<tag>.md`；具体测试通过记录归对应提交的执行证据。

## 验证与提交

Windows 标准入口为：

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

准备发布时，同步默认版本、显式声明和该版本说明；完整验证通过后由独立桥接发布入口使用同一组 EXE、符号、元数据和校验和。不要手动改写已生成 EXE 的版本或混合来自不同构建的发行文件。
