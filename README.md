简体中文 | [English](docs/ReadMe.en-US.md) | [Русский](docs/ReadMe.ru-RU.md) | [Bahasa Indonesia](docs/ReadMe.id-ID.md)

![ScreenCapture-Plus](docs/banner.png)

正式批注工具实际复制得到的合成 PNG（GUI 自动化验证示例；删除 #1 后保留 #2、#3 的编号与配色）。

# ScreenCapture-Plus

**ScreenCapture-Plus v0.0.2** 是 [xland/ScreenCapture](https://github.com/xland/ScreenCapture) 的 Windows 原生截图工具维护分支。支持截图、绘图、长截图、GIF / MP4 录屏与二维码识别，并提供可随图片一起导出的矩形范围批注。

## v0.0.2 的范围批注

- 截图、编辑、录屏、长截图和样式工具条采用 **44 × 44 逻辑像素点击区域、24 逻辑像素图标**。尺寸跟随显示器 DPI；窄屏自动换行，按钮保持可点击大小。
- 原有 **序号** 工具继续用于绘制独立编号图形；旁边新增 **批注** 工具，以编号气泡图标区分。
- 选择 **批注** 后，在图片上拖出矩形范围。边框与编号自动使用同一主题色，较小的编号徽章固定在矩形左上角内侧，右侧列出对应说明。
- 拖动边框或编号移动整个范围；拖动边角控制点调整范围。编号、颜色与文字保持对应；撤销、重做或删除其他条目不会重新编号。
- 保存或复制时，图片、标记和右侧说明合成一张图片。文字自动换行并按空间调整字号，必要时增加输出高度或右侧列数；导出使用原图像素，不受预览缩放影响。
- 批注太多、文字太长或图片超过输出资源限制时，会提示无法完成导出并保留编辑内容，不会静默丢弃说明。

### 如何使用批注

1. 框选截图区域后点击图像标记按钮，或按住 `Ctrl` 完成框选，进入编辑窗口。
2. 点击 **序号** 旁的 **批注** 气泡按钮。
3. 在图片上按住鼠标左键，拖出需要说明的矩形范围。松开后自动进入右侧说明输入框，支持多行。矩形可向任意方向拖出；单击和过小的误拖不会留下空批注。
4. 点击已有编号、边框或右侧条目可以继续修改说明；拖动编号或边框移动整个区域，拖动边角控制点调整大小。范围保持在原图内，矩形内部仍可添加其他标记或嵌套范围。
5. 点击工具条上的保存或复制按钮，导出包含说明栏的整图。`Ctrl+S` 在批注输入时也可保存；`Ctrl+C` 在输入框内复制文字，先按 `Esc` 结束输入后再复制整图。

右侧列表可以滚动。收起说明栏不会删除批注，也不会从导出结果中移除说明。详细交互、编号规则与输出布局见 [批注使用说明](docs/ANNOTATIONS.md)。

## 其他功能

- 截图、滚动长截图，GIF / MP4 录屏；MP4 支持系统声音与麦克风选项。
- 二维码识别，支持复制识别内容。
- 取景框与拾色器：`Ctrl+R` 复制 RGB、`Ctrl+H` 复制 HEX、`Ctrl+K` 复制 CMYK。
- 矩形、椭圆、箭头、序号、线条、文本、马赛克、橡皮擦；按住 `Shift` 可约束圆形、正方形或直线。
- 鼠标移到绘图元素上可调整或删除；`Ctrl+Z` 撤销、`Ctrl+Y` 重做。
- `Ctrl+S` 保存、`Ctrl+C` 或双击复制；文字输入时按键由输入框处理。
- `Ctrl` 加鼠标滚轮缩放钉图预览；多语言、托盘与用完即走模式。
- 单个 `ScreenCapture.exe` 可直接运行，无需安装；应用依赖采用静态链接，无需单独安装 Visual C++ 运行库。

## 下载与运行

前往 [v0.0.2 Release](https://github.com/NAMEWTA/ScreenCapture-Plus/releases/tag/v0.0.2) 或 [全部 Releases](https://github.com/NAMEWTA/ScreenCapture-Plus/releases)。

发布资产包含 `ScreenCapture.exe`、Windows x64 便携 ZIP、调试符号 ZIP、`build-info.json` 和 `SHA256SUMS.txt`。便携 ZIP 附带语言文件与第三方许可；文件大小以实际发布资产为准。首次发布及后续 Release 的构建、测试和打包流程见 [构建与发布](docs/BUILDING.md)。

支持 **Windows 10 1803 及以上版本，x64**。手动更新前请先退出旧进程，再替换并启动新版本。

## 构建与验证

使用 Windows x64、Visual Studio 2026（Desktop development with C++ + ATL）、PowerShell 7、Git 和 Rustup。脚本恢复固定提交的 Ling / Yoga / gifski，避免依赖开发者电脑上的绝对路径。

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

可执行文件位于 `artifacts/bin/x64/Release/ScreenCapture.exe`；首次构建后可打开 `ScreenCapture.slnx` 调试。核心测试、原生渲染 / PNG 检查和启动检查各有覆盖范围；完整桌面交互、混合 DPI、输入法与音视频验证按 [验证清单](docs/VALIDATION.md) 执行。

- [批注交互与导出](docs/ANNOTATIONS.md)
- [完整构建与发布指南](docs/BUILDING.md)
- [目录、架构、扩展与性能说明](docs/ARCHITECTURE.md)
- [开发约定](CONTRIBUTING.md)
- [语言文件](Lang/ReadMe.md)
- [第三方来源与许可](third_party/NOTICE.md)

## 命令行

```powershell
# 完成截图任务后退出进程
ScreenCapture.exe --auto-quit=true

# 框选后直接进入图像标记 / 钉图窗口
ScreenCapture.exe --enter=pin
# 框选后进入长截图
ScreenCapture.exe --enter=long
# 框选后进入屏幕录制
ScreenCapture.exe --enter=video
# 框选后识别二维码（也接受 --enter=qrcode）
ScreenCapture.exe --enter=qr
# 仅注册托盘图标
ScreenCapture.exe --enter=tray

# 参数可以组合
ScreenCapture.exe --enter=long --auto-quit=true
```

## 便携配置与语言

默认配置位于 `%appdata%\ScreenCapture\config.json`。若 `ScreenCapture.exe` 同目录已存在 `config.json`，则优先使用该文件；可以先建立一个空文件再启动程序。

额外语言优先从 EXE 同目录的 `Lang` 子目录加载；不存在该目录时，使用 `%appdata%\ScreenCapture\Lang`。简体中文和英语内置于 EXE，其他语言的安装及更新方式见 [语言说明](Lang/ReadMe.md)。

## 上游作者与赞助


保留原作者的署名、MIT 许可与赞助信息。下列联系方式与二维码属于上游作者。

<table>
  <tr>
    <td align="center">
      <img alt="支付宝赞助" src="./docs/alipay.jpg" width="160" height="160">
      <p>支付宝赞助</p>
    </td>
    <td align="center">
      <img alt="微信赞助" src="./docs/wechat.png" width="160" height="160">
      <p>微信赞助</p>
    </td>
    <td align="center">
      <img alt="作者微信" src="./docs/author.jpg" width="160" height="160">
      <p>作者微信</p>
    </td>
    <td align="center">
      <img alt="公众号二维码" src="./docs/gongzhonghao.jpg" width="160" height="160">
      <p>公众号：桌面软件</p>
    </td>
  </tr>
</table>
