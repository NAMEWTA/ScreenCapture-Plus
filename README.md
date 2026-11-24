简体中文 | [English](docs/readme.en-us.md) | [Русский](docs/readme.ru-ru.md) | [Bahasa Indonesia](docs/readme.id-id.md)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/brand/huhucapture-logo-dark.svg">
  <img src="assets/brand/huhucapture-logo-light.svg" alt="huhucapture" width="560">
</picture>

# huhucapture

**huhucapture** 是面向 Windows 的原生截图、标记与录屏工具，由 [NAMEWTA](https://github.com/NAMEWTA) 维护。它将矩形范围、彩色编号和右侧说明整理到同一张截图中，也提供钉图、长截图、GIF / MP4 录屏与二维码识别。

本项目基于 [xland / ScreenCapture](https://github.com/xland/ScreenCapture) 衍生开发。感谢原作者与上游贡献者，以及 [Ling](https://github.com/xland/Ling) 等开源组件的维护者，为原生截图、编辑和录屏能力提供基础。继承起点、独立维护范围与版权归属见 [上游致谢](docs/upstream.md)。

项目属于 [huhu](https://github.com/NAMEWTA/huhu) 品牌家族，采用虎虎形象与截图范围角标。产品名称、程序文件、配置目录和发布资产统一使用 `huhucapture`。品牌资产及使用约定见 [品牌说明](docs/brand.md)。

[下载 v0.0.4](https://github.com/NAMEWTA/huhucapture/releases/tag/v0.0.4) · [全部版本](https://github.com/NAMEWTA/huhucapture/releases) · [提交问题](https://github.com/NAMEWTA/huhucapture/issues) · [更新记录](CHANGELOG.md)

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

导出先自动换行、调整字号与间距，必要时增加画布高度或右侧列数，保留原图像素。超过设备尺寸或内存预算时会提示导出失败并保留编辑内容；不会静默截断说明。完整规则见 [批注使用与导出](docs/annotations.md)。

## 功能

| 场景 | 能力 |
| --- | --- |
| 截图与钉图 | 区域截图、拾色器、图像编辑、钉图预览与缩放 |
| 长内容 | 滚动长截图 |
| 录屏 | GIF / MP4；MP4 可选择系统声音与麦克风 |
| 绘图 | 矩形、椭圆、箭头、普通序号、线条、文字、马赛克、橡皮擦 |
| 范围说明 | 矩形批注、自动配色与稳定编号、右侧多行说明、合图保存与复制 |
| 二维码 | 识别截图中的二维码并复制内容 |
| 媒体历史 | 保存/复制自动归档、截图缩略图、视频封面、再次复制、打开目录、回收站清理 |
| 日常使用 | 默认 F1 截图、托盘、用完即走、便携配置、多语言、重新设计的设置页 |

工具条采用 **44 × 44 逻辑像素点击区域、24 逻辑像素图标**，随显示器 DPI 调整，空间不足时自动换行。普通序号和范围批注是独立工具。项目不提供 OCR 功能。

## 剪切复制板：随时找回截图与录屏

点击保存或工具条上的完成复制按钮后，截图、批注合图、长截图和录屏都会自动归档到 **系统“文档”目录下的 `huhucapture` 文件夹**。程序通过 Windows 的文档已知文件夹定位，文档移动到 OneDrive 或其他磁盘后仍使用实际位置。

文件按本地时间命名为 `YYYYMMDD-HHMMSS.png`、`.gif` 或 `.mp4`。同一秒保存同类型文件时追加 `-001`、`-002`，保留每一次结果。另存为默认从该目录开始；选择其他位置后，归档副本仍保留。

截图工具条的 **二维码识别右侧** 新增“剪切复制板”图标。打开后以列表查看历史截图与视频封面，可以逐项复制、打开所在文件夹，也可以一键清除。图片复制为图像，视频复制为可粘贴的媒体文件。文件跨程序重启保留；已完成录屏即使取消另存为，也可在这里重新找到。

历史窗口采用紧凑列表：预览、文件信息和操作分别对齐，复制、目录与翻页按钮保持统一的桌面尺寸，只有多页内容才显示分页按钮。缩略图保留媒体比例，长文件名按可用空间省略，窗口受当前工作区约束；列表滚动与操作命中使用同一套位置规则。

也可以从托盘右键菜单直接打开“剪切复制板”。一键清除会先确认，默认把列表中的归档媒体送入系统回收站；如果 Windows 无法回收，会再次提示确认。它只管理归档目录顶层符合命名规则的 PNG / GIF / MP4，不递归处理其他文件，也不删除另存到其他位置的副本。失败或取消后保留剩余项并刷新列表。完整边界见 [媒体历史设计](docs/media-history.md)。

### 更协调的桌面设置

设置页沿用指定的 [Apple 设计语言](https://github.com/VoltAgent/awesome-design-md/blob/main/design-md/apple/DESIGN.md)，采用紧凑的桌面比例：720 × 500 逻辑像素窗口、居中的内容列、14px 正文与 32px 控件，中性浅灰画布搭配白色分组和单一蓝色交互。通用、快捷键和关于页面共用字体、对齐与间距规则。

语言下拉框与展开列表使用一致的字号和样式，当前语言有勾选提示。方向键移动高亮，Enter 确认；Esc 或点击外部取消。归档目录可以直接打开，较长路径保留完整操作目标。比例、弹层和 DPI 约定见 [设计文档](docs/design.md)。

## 下载与运行

支持 **Windows 10 1803 及以上版本，x64**。从 [GitHub Releases](https://github.com/NAMEWTA/huhucapture/releases) 下载 `huhucapture.exe` 后即可运行，无需安装。应用及静态依赖不要求另行安装 Visual C++ 运行库。

**只想使用软件：下载 `huhucapture.exe` 即可；需要额外语言和随包许可时，选择便携 ZIP。** 其余文件供故障定位、源码开发或核验下载使用。

| 发布文件 | 包含什么 | 什么时候需要 |
| --- | --- | --- |
| `huhucapture.exe` | 可直接运行的 Windows x64 单文件，内置简体中文和英语；也是更新器使用的固定文件名 | 日常使用，下载后运行 |
| `huhucapture-v0.0.4-windows-x64.zip` | 同一份 EXE、`Lang` 额外语言、依赖许可/声明和 `build-info.json` | 想完整保留分发内容，解压后运行其中的 EXE |
| `huhucapture-v0.0.4-windows-x64-symbols.zip` | 与本次 Release 构建配套的 PDB 调试符号 | 开发者分析崩溃、调用栈或断点时使用；普通运行无需下载 |
| `build-info.json` | 仓库与源提交 SHA、标签、Windows 版本、架构、工具链、固定依赖和 EXE 的 SHA256 | 提交问题或核对一个文件究竟来自哪次构建 |
| `SHA256SUMS.txt` | 上述 EXE、两种 ZIP 和构建信息的 SHA256 校验值 | 核对下载是否完整、文件是否与该 Release 一致 |
| GitHub 自动提供的 `Source code (zip / tar.gz)` | 对应标签的源码快照 | 自行编译或阅读源码；其中不包含已构建的可运行 EXE |

#### 为什么保留 symbols.zip

PDB 能让调试器把程序地址对应到函数、源码位置和变量信息，帮助定位普通错误提示无法解释的崩溃。它不是运行依赖，也不会让程序启动或截图更快，因此与便携程序分开下载。说明依据微软的 [符号文件文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/symbols) 和 [PDB 匹配规则](https://learn.microsoft.com/en-us/visualstudio/debugger/specify-symbol-dot-pdb-and-source-files-in-the-visual-studio-debugger)。

需要调试时，从**同一 Release、同一次构建**下载 EXE、symbols ZIP 和 `build-info.json`，解压 PDB，在 Visual Studio 或 WinDbg 的符号搜索路径中加入解压目录，再打开对应 EXE 或崩溃转储。要查看源码行，检出 `build-info.json` 记录的提交。不要把其他版本或自行重新编译产生的 PDB 与发行 EXE 混用；版本号相同也不代表符号能够匹配。符号包本身不包含用户截图或故障时的内存转储。

在 PowerShell 中可核验下载：

```powershell
Get-FileHash .\huhucapture.exe -Algorithm SHA256
```

把输出与同一 Release 的 `SHA256SUMS.txt` 对应文件行比较即可；其余文件同样核验。

### 更新与默认快捷键

手动更新前先退出正在运行的进程，再替换并启动新文件。程序内的更新检查使用本仓库的稳定 Release，并在安装前请求确认。

默认全局截图快捷键改为 **F1**。升级时，旧默认 `Ctrl+Alt+A` 迁移为 F1，其他自定义快捷键保留；仍可在设置的快捷键页面修改。已有 `%appdata%\huhucapture\config.json` 或 EXE 旁的便携配置继续使用。

### 常用操作

| 操作 | 快捷方式 |
| --- | --- |
| 开始截图（默认全局快捷键） | `F1` |
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

额外语言优先从 EXE 同目录的 `Lang` 文件夹加载；该文件夹不存在时，使用 `%appdata%\huhucapture\Lang`。两处目录不会合并。简体中文与英语内置，其他语言的安装、编码与翻译约定见 [语言文件说明](docs/languages.md)。

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
| [文档地图](docs/readme.md) | 按问题查找设计、工程和验证约定 |
| [上游致谢](docs/upstream.md) | ScreenCapture 与 Ling 来源、继承起点和维护归属 |
| [工程约定](docs/conventions.md) | 目录、编码、资源、错误处理与文档规范 |
| [媒体历史](docs/media-history.md) | 归档目录、命名、保存失败和清理边界 |
| [界面设计](docs/design.md) | 设置与媒体历史的字体、颜色、布局及交互 |
| [批注说明](docs/annotations.md) | 交互、稳定身份、完整导出和失败语义 |
| [架构与扩展](docs/architecture.md) | 目录职责、资源所有权、数据流、性能约束 |
| [构建与发布](docs/building.md) | 固定依赖、本地构建、Actions、版本与资产 |
| [验证指南](docs/validation.md) | 自动测试范围、证据路径、手工验收 |
| [贡献指南](CONTRIBUTING.md) | 开发流程、改动边界与提交要求 |
| [自动化协作约定](AGENTS.md) | 面向代码代理的仓库规范 |
| [品牌说明](docs/brand.md) | 虎虎标志、SVG / PNG / ICO 与使用方式 |

## 许可

代码遵循 [MIT 许可](LICENSE)。第三方依赖、来源与许可见 [third_party/NOTICE.md](third_party/NOTICE.md)。分发时应保留相应许可与版权声明。
