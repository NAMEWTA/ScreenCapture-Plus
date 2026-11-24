简体中文 | [English](./docs/ReadMe.en-US.md) | [Русский](./docs/ReadMe.ru-RU.md) | [Bahasa Indonesia](./docs/ReadMe.id-ID.md)

![banner](./docs/banner.png)

**ScreenCapture-Plus** 是 [ScreenCapture](https://github.com/xland/ScreenCapture) 的长期维护分支，保留原生 Windows 截图工具的功能与交互，持续优化架构、性能和构建发布。

## 特性

- 截图、绘图标注、滚动截图（截长图）、录屏（GIF/MP4）、文字识别（OCR）、二维码识别。
- 取景框（拾色器），支持快捷键复制 RGB 颜色（`Ctrl+R`）、 HEX 颜色（`Ctrl+H`）与 CMYK 颜色（`Ctrl+K`）。
- 绘制椭圆、正圆（按住`Shift`）、矩形、正方形（按住`Shift`）、箭头、标号等。
- 绘制曲线、直线（按住`Shift`）、马赛克、橡皮擦、文本。
- 可以随时修改、删除已绘制的元素（鼠标移到元素上）。
- 撤销（`Ctrl+Z`）、重做（`Ctrl+Y`）、保存为文件（`Ctrl+S`）、保存到剪贴板（`Ctrl+C`或双击）。
- 运行速度快、内存占用低。
- 体积小、仅一个可执行文件，无需安装，不依赖任何动态链接库（文字识别除外）。
- 支持多种命令行参数直接启动指定的功能。
- 支持用完即走（进程不驻留在系统中）。
- 多语言支持。

## 下载

[下载本项目的 Release](https://github.com/NAMEWTA/ScreenCapture-Plus/releases/)。发布后 GitHub Actions 自动构建并附加 `ScreenCapture.exe`、便携 ZIP、符号和 SHA256 校验文件；文件大小以实际资产为准。

## 常用功能与问题

- 按住 `Ctrl键` 框选截图区域后，直接进入图像标记窗口（钉图窗口）
- 按住 `Ctrl键` 滚动鼠标滚轮可以放大、缩小图像标记窗口（钉图窗口）
- 长截图拼接不符合预期时，尝试调整截图区域往往能解决问题
- 如手动下载新版本，则必须退出老版本再启动新版本

## 支持的操作系统

- Windows 10 1803 or Later

## 编译

使用 Windows x64、Visual Studio 2026（C++ Desktop + ATL）、PowerShell 7、Git 和 Rustup。构建脚本自动恢复固定提交的 Ling/Yoga/gifski，统一静态链接；不再依赖开发者电脑上的绝对路径。

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/package.ps1
```

可执行文件位于 `artifacts/bin/x64/Release/ScreenCapture.exe`。也可在首次构建后打开 `ScreenCapture.slnx` 调试。

- [完整构建与 Release 发布指南](docs/BUILDING.md)
- [目录、架构、设计选择与性能说明](docs/ARCHITECTURE.md)
- [自动化覆盖与 Windows 功能验收](docs/VALIDATION.md)
- [开发约定](CONTRIBUTING.md)
- [第三方来源与许可](third_party/NOTICE.md)

上游历史稳定分支：[2.4.25（D2D）](https://github.com/xland/ScreenCapture/tree/2.4.25)、[2.3.3（Qt）](https://github.com/xland/ScreenCapture/tree/2.3.3_qt)。

## 命令行

```
// 截图完成后即退出进程。
> ScreenCapture.exe --auto-quit=true

// 框选完成后不显示工具条，直接进入指定功能：
// pin 钉图/图像标记
> ScreenCapture.exe --enter=pin
// long 长截图
> ScreenCapture.exe --enter=long
// video 屏幕录制
> ScreenCapture.exe --enter=video
// ocr 文字识别
> ScreenCapture.exe --enter=ocr
// qr 二维码识别
> ScreenCapture.exe --enter=qr
// tray 仅注册托盘图标，不执行任何操作
> ScreenCapture.exe --enter=tray

// 两个参数可以联合使用，比如：不注册托盘图标，截完长图后进程直接退出
> ScreenCapture.exe --enter=long --auto-quit=true
```

## 文字识别插件

下载最新版本的文字识别工具 [ImageReader.exe](https://github.com/xland/ImageReader/releases) (约25MB) 并把此文件放置到 `%appdata%\ScreenCapture\plugin`目录下，然后重启应用即可使用（或 `ScreenCapture.exe` 同目录下亦可）

## 便携能力

默认配置位于 `%appdata%\ScreenCapture\config.json`，额外语言位于 `%appdata%\ScreenCapture\Lang`，OCR 插件位于 `%appdata%\ScreenCapture\plugin`。

但用户可以在 `ScreenCapture.exe` 同目录下创建一个 `config.json` 的空文件，重启应用，`ScreenCapture` 即会在此文件中设置配置信息。

`ScreenCapture.exe` 同目录下创建一个 `Lang` 子目录，然后把语言文件放置到此目录下，`ScreenCapture` 即会读取此目录下的语言文件。

文字识别插件 `ImageReader.exe` 也可以放置在 `ScreenCapture.exe` 同目录下

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