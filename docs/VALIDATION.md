# huhucapture 验证与桌面验收

## 如何读取验证结果

本文定义验证范围、执行方式与证据位置。某项检查列在本文中，不代表某次构建已经执行或通过。huhucapture v0.0.1 的构建结果以对应提交的 [Windows Actions](https://github.com/NAMEWTA/huhucapture/actions/workflows/windows.yml) 为准，发行文件以 [Release](https://github.com/NAMEWTA/huhucapture/releases/tag/v0.0.1) 的 `build-info.json` 和 SHA256 为准。

核心测试、离屏渲染、启动检查、脚本 GUI 和人工桌面验收分别记录。报告至少包含提交 SHA、命令、环境、结果与证据；未执行的场景明确写为未执行。更换产品身份或资源后，应重新验证对应提交，不能把先前生成的截图当作本次品牌、版本和程序启动的通过记录。

## 自动化入口

| 命令 | 验证层次 | 主要输出 |
| --- | --- | --- |
| `pwsh ./scripts/build.ps1 -Configuration Release` | 正式 x64 编译和链接 | `artifacts/bin/x64/Release/huhucapture.exe` |
| `pwsh ./scripts/test.ps1` | 八个纯核心程序及 PowerShell 语法 | 测试退出码与日志 |
| `pwsh ./scripts/test-windows.ps1` | 真实 DWrite / D2D / WIC 离屏合成 | `artifacts/tests/windows` |
| `pwsh ./scripts/smoke.ps1` | 独立 EXE / 便携配置启动 | 启动与退出日志 |
| `pwsh ./scripts/test-windows.ps1 -Gui` | 正式编辑器对象、窗口和剪贴板 | `artifacts/tests/windows/gui` |
| `pwsh ./scripts/package.ps1` | 版本、PE 依赖、包内容和校验和 | 五项发布资产 |
| `pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore` | Debug 编译 / 链接，构建设置一致性 | Debug 产物 |

Release 先于 GUI 测试，依赖 / 构建设置变化时还需 Debug。完整 CI 顺序、artifact 和发布门槛见 [BUILDING.md](BUILDING.md)。

## 纯核心回归

顶层 `tests/*.cpp` 各自有独立 `main`，不依赖显示器、账号或第三方测试框架。失败通过非零退出码表达，不因 Release 的 `NDEBUG` 而失效。

| 程序 | 覆盖 |
| --- | --- |
| `PixelBufferTests.cpp` | 尺寸乘法、长度、奇数宽、多行、BGRA → RGB、alpha 与越界哨兵 |
| `history_tests.cpp` | 空历史、精确删除、撤销边界、redo 清理、借用解除、延迟票据、地址复用和大量记录扩容 |
| `scroll_stitch_tests.cpp` | 匹配 / 拼接像素、可见尾部预览、极窄选区、空白条带、并列选择、灰度权重和非法输入 |
| `release_version_tests.cpp` | 稳定标签、可选 v、WORD 上限、非法 / 预发布拒绝和数值比较 |
| `annotation_tests.cpp` | 稳定 ID / 颜色、历史分支、空备注、紧凑布局、完整行、溢出和预算 |
| `annotation_region_tests.cpp` | 四向创建、退化误拖、保尺寸移动、八方向调整、边界、内部穿透、徽章和非整数缩放 |
| `annotation_panel_layout_tests.cpp` | 小图、100%–300% DPI、列表 / 删除区 / 编辑框 / 页脚分隔及增高布局 |
| `toolbar_layout_tests.cpp` | 大屏 / 窄屏换行、不同控件宽度、精确边界和 125%–300% DPI 取整 |

有 C++20 编译器的其他平台也可运行这些纯核心程序，例如：

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror -pedantic -Isrc tests/annotation_region_tests.cpp -o /tmp/annotation_region_tests
/tmp/annotation_region_tests
```

这类结果只能证明核心逻辑，不包含 Windows 编译、字体、窗口和设备行为。

## Windows 原生导出

`test-windows.ps1` 编译独立 `AnnotationRenderTests.exe`，直接使用正式 `src/services/AnnotationRenderer.cpp`，通过 D3D11 WARP 软件设备执行 Direct2D / DirectWrite，再用 WIC 编码和解码 PNG。

它验证完整文字和 ID / 配色、真实换行与字形外伸、紧凑徽章、原图位置和逐字节像素 / alpha、96-DPI 输出坐标，以及调用后的渲染目标、DPI、变换、单位、抗锯齿和混合模式恢复。非法输入、过大尺寸或预算不足应整体失败，不覆盖先前输出。

| PNG | 场景 |
| --- | --- |
| `01-no-annotations.png` | 无批注，尺寸和源像素不变 |
| `02-cjk-and-empty-note.png` | 中英文、空备注、不连续 ID 及配色 |
| `03-long-multiline-notes.png` | 长文与显式换行，扩高后全文保留 |
| `04-many-notes-multiple-columns.png` | 45 条说明在受限边长下完整分列 |
| `05-unbroken-url.png` | 无空格长 URL 的实际换行和字形边界 |
| `06-compact-short-notes.png` | 5 条短说明收紧后容纳于 280px 高度 |

所有 PNG 都回读检查尺寸及像素。测试程序、符号、报告和图片位于 `artifacts/tests/windows`，CI 归入 `windows-render-checks`。测试图左侧是合成底图，右侧由正式导出器渲染；它们是导出证据，不代表已经操作完整桌面编辑器。

## 真实窗口与剪贴板

`test-windows.ps1 -Gui` 将已有 Release 对象（排除 `main.obj`）、资源和依赖链接到独立测试程序，启动正式窗口类。它使用隔离配置并关闭测试会话的更新器，通过真实 HWND 消息、同步光标 / 按键状态和 `WM_CHAR` 驱动 UI；产品 EXE 不增加测试参数。

覆盖的受控场景包括：

- 选择真实批注工具，正 / 反方向拖框，检查小编号在左上角且同色；完成后直接输入中文、英文和多行说明。
- 列表切换、编辑与删除，检查文字归属；撤销 / 重做和继续新建时保持 ID、范围和颜色一致。
- 编辑后继续拖出新范围；单击、零宽 / 零高与 1px 误拖不留下空条目。
- 控制点调整、编号移动以及四边越界时的保尺寸限制。
- 丢失捕获时丢弃未完成新框、保留已有框最后有效位置，返回图片后不继续拖动。
- 收起面板、移动到显示器边缘后拖动已有范围；面板重开不会造成手势坐标跳变。
- 调用正式复制命令，从系统剪贴板取得并解码 PNG，检查原图与说明同时存在，边框、图内编号和右侧编号同色，区域内部透明。

证据位于 `artifacts/tests/windows/gui`：

| 文件 | 内容 |
| --- | --- |
| `test-report.txt` | 场景和断言结果 |
| `screens/01-editor-two-notes.png` | 实际编辑窗口与两条说明 |
| `screens/02-editor-after-drag.png` | 拖动后的窗口状态 |
| `screens/03-real-editor-clipboard.png` | 正式复制产生的合图 PNG |

GUI 进程有 45 秒硬超时，只终止测试启动的进程。硬件 D3D11 或输入桌面不可用时返回独立错误码 77，CI 视为失败，不将环境不足标记为成功。早于 GUI 生成的构建、原生渲染和打包证据仍可用于诊断，发布继续被阻止。

真实窗口消息覆盖的是列出的受控场景。`WM_CHAR` 无法证明 IME 组词与候选窗正常，窗口截图也不能证明多显示器、物理鼠标、硬件录屏和音频设备已完成验收。

## 启动、打包和发布检查

`smoke.ps1` 把 Release EXE 复制到隔离临时目录，放入空 `config.json`，以 `--enter=tray` 启动；确认三秒内没有提前退出，再清理测试进程与目录。它覆盖基本启动、便携配置和内置资源，不代表截图或录屏已运行。

`package.ps1` 核对文件版本、PE 依赖和四项身份资源：`ProductName` / `FileDescription` 为 `huhucapture`、`OriginalFilename` 为 `huhucapture.exe`、`CompanyName` 为 `NAMEWTA`。它生成 EXE、便携包、符号包、构建信息与 SHA256。声明发布还检查仓库、当前 main、构建 SHA、版本、架构和资产全集；全部门槛通过后才创建新标签及发行版，已有正式发行版不被声明入口修改。

品牌及版本变更需核对 `huhucapture.exe` 文件属性、窗口 / 托盘 / About 标识、图标、默认配置路径、更新资产名和 ZIP 内容。文件成功编译不能替代对这些可见入口的检查。

## Windows 人工桌面矩阵

未附环境、操作和结果的条目视为未执行。记录提交、Windows 版本、显卡与驱动、显示器位置 / 分辨率 / DPI、输入法和音源，至少覆盖一个混合 DPI 双屏环境。

| 场景 | 验收重点 |
| --- | --- |
| 品牌和程序身份 | EXE 名、文件属性、任务栏 / 托盘 / 窗口 / About 图标与文案一致；仓库和更新链接正确；默认配置写入 huhucapture 目录 |
| 工具条与 DPI | 各工具条 44px 点击区、24px 图标；100%、125%、150%、200%、300% 缩放，窄屏换行，边缘不裁切，跨混合 DPI 无累计偏移 |
| 范围创建与调整 | 四向拖框、同色边框 / 小编号、完成后输入、误拖清理；整体移动、八点调整、边界和小范围徽章 |
| 文字与 IME | 点击编号 / 边框 / 列表切换；中文组词与候选窗、英文、多行、长 URL、空文本；说明不串写、快捷键不打断输入 |
| 面板与焦点 | 长列表滚动、编辑框滚动、收起 / 重开；Alt+Tab、捕获中断、被动布局不抢焦点，面板区域不触发图形 |
| 历史与编号 | 删除中间项不改其他 ID / 配色；创建撤销 / 重做同步恢复；新建不复用已分配 ID；删除最后一项后无空说明栏 |
| 保存和复制 | PNG / 剪贴板含全部说明；中文、换行、长 URL、紧凑文本和多列；失败 / 取消保存不丢编辑内容 |
| 缩放与极端尺寸 | 小选区、1px 图像边界、长图、大图缩小预览、新建编号可读；合图仍按原图物理像素读取 |
| 输入快捷键 | 文字输入的选择 / 删除 / 复制不影响整条批注；Ctrl+S 保存；Esc 后图形撤销 / 重做 / 复制恢复 |
| 启动和常驻 | 默认截图、托盘、重复启动、全局热键和退出；无残留窗口 / 线程 |
| 选区与原绘图工具 | 主屏、负坐标副屏、跨屏、调整、Esc、取色；各图形、Shift 约束、修改 / 删除、普通序号独立 |
| 钉图与剪贴板兼容 | 多窗口、拖动、缩放、双击；复制到浏览器、Electron、Office 和旧式 DIB 程序 |
| 长截图 | 平滑滚动、固定 / 纯色顶部、底部兜底、滚到底、极窄区域和连续多段；结束后仍可编辑和保存 |
| GIF | 奇数宽、指针、立即停止、中文路径、长录制、重复开始 / 结束；文件可打开 |
| MP4 与音频 | 主 / 副屏、混合显卡、HEVC 回退；系统声 / 麦克风 / 两者 / 无音源，拔出或无包时可停止且文件可播放 |
| 二维码和功能入口 | QR 内容复制、镜像码与多码；界面和设置不出现 OCR 按钮或插件安装入口 |
| 语言和配置 | 空 / 缺键配置、语言缺键 / 格式错误、中英文切换、未知字段；EXE 旁配置优先、两处语言目录不合并 |
| 更新与命令行 | 稳定 Release、缺资产、取消、目录不可写、请求中退出；只使用同 Release EXE；所有当前 --enter 和 --auto-quit 参数 |

## 性能测量

使用同一台机器、相同系统 / 电源 / 显卡模式、截图区域、内容、帧率与音源，对比较版本各执行至少三次，报告中位数及异常。记录版本和输入数据，让他人能重现实验。

| 负载 | 记录内容 |
| --- | --- |
| 启动到可框选 | 时延、初始私有内存、GDI / USER 句柄 |
| 1、10、50 条批注 | 输入 / 列表 / 拖动响应、修改后布局时间、输出尺寸、峰值内存；包含中文长文与 URL |
| 同一段 60 秒 4K GIF | CPU、内存、帧数 / 时长、文件大小，区分捕获和编码 |
| 连续长图 | 单次拼接时间、累计尺寸、峰值内存及最终像素 |
| 1,000 个图形与大量撤销 | 悬停 / 拖动和渲染时间，区分历史遍历与绘制成本 |
| MP4 整屏和小选区 | 编码负载、停止延迟、音画同步、无音频包时 CPU |
| 50 次截图关闭、20 次录屏停止 | 句柄、线程、内存是否持续增长 |

未测量的数据不填写推算的百分比。源代码可以证明某个副本或分配路径被消除，但端到端性能、设备稳定性和资源峰值需要对应测量。原生导出 PNG 主要证明正确性，不能替代性能实验。
