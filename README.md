简体中文 | [English](docs/ReadMe.en-US.md) | [Русский](docs/ReadMe.ru-RU.md) | [Bahasa Indonesia](docs/ReadMe.id-ID.md)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/brand/huhucapture-logo-dark.svg">
  <img src="assets/brand/huhucapture-logo-light.svg" alt="huhucapture" width="560">
</picture>

# huhucapture

**huhucapture** 是面向 Windows 的原生截图、标记与录屏工具，由 [NAMEWTA](https://github.com/NAMEWTA) 维护。它将矩形范围、彩色编号和右侧说明整理到同一张截图中，也提供钉图、长截图、GIF / MP4 录屏与二维码识别。

项目属于 [huhu](https://github.com/NAMEWTA/huhu) 品牌家族，采用虎虎形象与截图范围角标。产品名称、程序文件、配置目录和发布资产统一使用 `huhucapture`。品牌资产及使用约定见 [品牌说明](docs/BRAND.md)。

[下载 v0.0.1](https://github.com/NAMEWTA/huhucapture/releases/tag/v0.0.1) · [全部版本](https://github.com/NAMEWTA/huhucapture/releases) · [提交问题](https://github.com/NAMEWTA/huhucapture/issues) · [更新记录](CHANGELOG.md)

## 截图与说明，一起保存

![huhucapture 的范围批注合成示例](docs/banner.png)

上图展示实际编辑器复制出的合成图片。每个矩形、左上角编号和右侧条目使用同一颜色；删除某项后，其他编号与颜色保持不变，因此编号可以不连续。

### 范围批注

1. 框选截图后点击图像标记按钮，或按住 `Ctrl` 完成框选，进入编辑窗口。
2. 选择普通 **序号** 旁的 **批注** 工具，在图片上按住左键拖出需要说明的矩形。
3. 松开鼠标后，在右侧输入说明，支持多行。边框和左上角的小编号自动采用相同主题色，无需选择颜色。
4. 点击已有编号、边框或右侧条目编辑说明；拖动编号或边框移动整块范围，拖动边角控制点调整大小。
5. 保存或复制时，原图、所有标记和完整说明栏合成一张图片。

批注矩形可向任意方向拖出；单击与过小的误拖不会留下空框。矩形内部保持透明，可继续添加标记或嵌套批注。收起说明栏只影响编辑界面，已有说明仍会完整导出。

导出先自动换行、调整字号与间距，必要时增加画布高度或右侧列数，保留原图像素。超过设备尺寸或内存预算时会提示导出失败并保留编辑内容；不会静默截断说明。完整规则见 [批注使用与导出](docs/ANNOTATIONS.md)。

## 功能

| 场景 | 能力 |
| --- | --- |
| 截图与钉图 | 区域截图、拾色器、图像编辑、钉图预览与缩放 |
| 长内容 | 滚动长截图 |
| 录屏 | GIF / MP4；MP4 可选择系统声音与麦克风 |
| 绘图 | 矩形、椭圆、箭头、普通序号、线条、文字、马赛克、橡皮擦 |
| 范围说明 | 矩形批注、自动配色与稳定编号、右侧多行说明、合图保存与复制 |
| 二维码 | 识别截图中的二维码并复制内容 |
| 日常使用 | 托盘、用完即走、便携配置、多语言 |

工具条采用 **44 × 44 逻辑像素点击区域、24 逻辑像素图标**，随显示器 DPI 调整，空间不足时自动换行。普通序号和范围批注是独立工具。项目不提供 OCR 功能。

## 下载与运行

支持 **Windows 10 1803 及以上版本，x64**。从 [GitHub Releases](https://github.com/NAMEWTA/huhucapture/releases) 下载 `huhucapture.exe` 后即可运行，无需安装。应用及静态依赖不要求另行安装 Visual C++ 运行库。

| 发布文件 | 用途 |
| --- | --- |
| `huhucapture.exe` | 单文件程序入口 |
| `huhucapture-v0.0.1-windows-x64.zip` | 便携程序、额外语言、许可与构建信息 |
| `huhucapture-v0.0.1-windows-x64-symbols.zip` | 调试符号，供问题定位使用 |
| `build-info.json` | 源提交、版本、架构、工具链、依赖与 EXE 哈希 |
| `SHA256SUMS.txt` | 下载文件校验 |

手动更新前先退出正在运行的进程，再替换并启动新文件。程序内的更新检查使用本仓库的稳定 Release，并在安装前请求确认。

### 从此前开发构建迁移

本次版本序列从 `v0.0.1` 开始。使用此前开发构建的用户需要手动下载替换：先在旧程序中关闭开机自启动，再完全退出，随后运行新的 `huhucapture.exe`。配置不会自动迁移；如需沿用设置，可将原 `config.json` 复制到 `%appdata%\huhucapture\config.json`，或放在新 EXE 同目录启用便携配置。

### 常用操作

| 操作 | 快捷方式 |
| --- | --- |
| 保存完整图片 | `Ctrl+S` |
| 复制完整图片 | 非文字编辑状态下 `Ctrl+C`、`Enter` 或双击 |
| 图形撤销 / 重做 | 非文字编辑状态下 `Ctrl+Z` / `Ctrl+Y` |
| 约束图形 | 绘制时按住 `Shift`，适用于普通矩形、椭圆和直线 |
| 钉图预览缩放 | `Ctrl` + 鼠标滚轮 |
| 复制颜色 | 截图拾色时 `Ctrl+R`（RGB）、`Ctrl+H`（HEX）、`Ctrl+K`（CMYK） |

文字或批注输入框拥有焦点时，文字输入、选择、删除与 `Ctrl+C` 由输入框处理；`Ctrl+S` 仍可保存整图。按 `Esc` 结束输入后再执行整图复制。当前批注输入框没有独立的文字撤销栈，输入时不会将 `Ctrl+Z` / `Ctrl+Y` 转成整条批注的撤销或重做。

## 命令行

```powershell
# 完成当前截图任务后退出
huhucapture.exe --auto-quit=true

# 框选后进入对应功能
huhucapture.exe --enter=pin
huhucapture.exe --enter=long
huhucapture.exe --enter=video
huhucapture.exe --enter=qr

# 只启动托盘；参数也可组合
huhucapture.exe --enter=tray
huhucapture.exe --enter=long --auto-quit=true
```

`--enter=qrcode` 是 `--enter=qr` 的别名。上述参数控制桌面交互入口。

## 配置与语言

默认配置文件是 `%appdata%\huhucapture\config.json`。如果 EXE 同目录已有 `config.json`，程序优先使用它；可以预先创建空文件启用便携配置。

额外语言优先从 EXE 同目录的 `Lang` 文件夹加载；该文件夹不存在时，使用 `%appdata%\huhucapture\Lang`。两处目录不会合并。简体中文与英语内置，其他语言的安装、编码与翻译约定见 [语言文件说明](Lang/ReadMe.md)。

## 开发与维护

使用 Windows x64、Visual Studio 2026（C++ 桌面开发、MSVC v145、ATL 与 Windows SDK）、PowerShell 7、Git 和 Rustup。在仓库根目录运行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

程序输出到 `artifacts/bin/x64/Release/huhucapture.exe`。首次恢复依赖后可打开 `huhucapture.slnx` 调试。CI 共用这些脚本，只有完整验证通过后才会发布声明的版本。脚本覆盖范围、桌面手工验收和实际证据应分别记录，不能相互替代。

| 文档 | 内容 |
| --- | --- |
| [批注说明](docs/ANNOTATIONS.md) | 交互、编号、布局、导出边界 |
| [架构与扩展](docs/ARCHITECTURE.md) | 目录职责、资源所有权、数据流、性能约束 |
| [构建与发布](docs/BUILDING.md) | 固定依赖、本地构建、Actions、版本与资产 |
| [验证指南](docs/VALIDATION.md) | 自动测试范围、证据路径、手工验收 |
| [贡献指南](CONTRIBUTING.md) | 开发流程、改动边界与提交要求 |
| [自动化协作约定](AGENTS.md) | 面向代码代理的仓库规范 |
| [品牌说明](docs/BRAND.md) | 虎虎标志、SVG / PNG / ICO 与使用方式 |

## 许可

代码遵循 [MIT 许可](LICENSE)。第三方依赖、来源与许可见 [third_party/NOTICE.md](third_party/NOTICE.md)。分发时应保留相应许可与版权声明。
