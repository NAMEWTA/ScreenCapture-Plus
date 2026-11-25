# 实际安装包的生命周期验证

`scripts/test-qt-installation.py` 消费 `package-qt.py` 已生成且未修改的真实平台包、
清单和 build-info，在原生 Windows 或 Ubuntu X11 桌面执行安装、同版本重装、
产品 GUI 启动和卸载。它复用现有打包元数据校验与系统安装器，不生成另一种
安装包，不修改产物的版本、源提交、dirty 或 development 标记。

这是 Issue #5 的 A11 / P11b 自动化接缝。实现接入流水线不等于检查已经通过；
每轮结果以同一提交下的 `installation-results.json`、进程日志和实际桌面 PNG
为准。正式发布仍需要完整 P12 验收。

## 四种实际生命周期

| 包 | 实际执行 | 数据保护 |
| --- | --- | --- |
| Windows NSIS | 原始安装器静默安装到含中文和空格的当前用户目录；原样重装；执行本次登记的真实卸载器 | 每阶段核对 UTF-16 配置、未知字段所在原始文件、独立子目录、PNG、MP4、实际 Documents 媒体；核对 HKCU 和当前用户开始菜单 |
| Windows ZIP | 核对完整 ZIP 的 CRC、目录清单和每个文件后解压；覆盖相同包的程序文件；按包的明确清单移除程序文件 | 未列入程序清单的配置和用户文件保留；不称为原生安装/卸载器 |
| Linux DEB | `apt-get install`、同包 `--reinstall`、真实 `dpkg --remove` / `--purge` | 核对实际包状态、架构、安装文件；保留用户配置、Documents 媒体和 `/opt/huhucapture` 中本轮专门创建的未知文件 |
| Linux AppImage | 复制原始 type-2 AppImage；使用官方 runtime 的免 FUSE 解包运行；替换相同包；删除该 AppImage 文件 | 显式便携配置位于可写目录，独立于临时解包目录；删除单个 AppImage 不删除配置或媒体 |

Windows 和 DEB 检查拒绝覆盖已有安装、注册表登记或不明安装路径。全局测试
媒体采用本轮独有名称；清理前再次核对原始 hash，只删除仍然匹配的本轮文件，
并先保留证据副本。不递归删除 Documents，不接管已有用户安装。建议只在
专用的原生 CI runner 或明确准备的验收机上运行。

## 独立的原生 GUI 探针

`installation_gui_probe` 是只在 `BUILD_TESTING=ON` 编译的独立程序，不安装到
产品、不改变产品入口。探针通过正式 `--auto-quit --portable-dir=...` 参数
启动安装后的原始可执行文件，然后：

1. 显示独立的已知像素图案，使用真实 Windows / X11 截图后端确认它已呈现。
2. 找到本次启动进程所拥有的真实截图选区窗口，核对实际运行文件的 SHA256。
   AppImage 按实际子进程关系定位应用，不把解包 launcher 当作 GUI 程序。
3. 检查子进程实际加载的 Qt、平台插件及 Windows CRT 来自包内目录。
   产品子进程的开发 Qt、库路径和 PipeWire 环境被清除；探针自身可使用测试 SDK。
4. 仅向该窗口拥有的坐标发送原生拖选，重新截取桌面，要求选区内为原始像素、
   选区外出现真实遮罩；不能用一个标题或错误对话框代替可用产品。
5. 发送系统窗口关闭请求，等待正常退出、工作排空和返回码 0；超时或遗留
   后代进程使检查失败。保留启动前、截图窗口、拖选后画面和输出日志。

这条旅程使用真实安装包及真实桌面输入/读回；不向产品加入测试窗口、Mock
采集源或跳过初始化的命令行分支。

## 与完整应用 CI 相同的入口

Linux 在已有真实包、共享视频 fixture 和已编译探针之后运行：

```sh
dbus-run-session -- xvfb-run -a env QT_QPA_PLATFORM=xcb \
  python scripts/test-qt-installation.py \
  --packages artifacts/packages/linux \
  --probe artifacts/cmake/application-linux/installation_gui_probe \
  --video artifacts/video-fixtures/valid.mp4 \
  --artifacts artifacts/cmake/application-linux/installation
```

Windows 在原生桌面运行：

```powershell
python scripts/test-qt-installation.py `
  --packages artifacts/packages/windows `
  --probe artifacts/cmake/application-windows/RelWithDebInfo/installation_gui_probe.exe `
  --video artifacts/video-fixtures/valid.mp4 `
  --artifacts artifacts/cmake/application-windows/installation
```

输出目录必须为空。输入 MP4 来自固定编码器生成、两平台共享的实际 fixture。
两端都校验清单绑定的包和可执行文件，不会重写元数据来接受另一版本。
即使此前组件检查失败，完整应用流水线仍可收集成功生成的包的安装证据；
先前失败不会因此变绿或被忽略。

## 仍需单独验收的内容

同一包的重装不是跨版本升级，静默安装不是安装向导的物理交互验收，Xvfb
不是实体多屏。报告明确记录这些边界，不把缺项隐去。另需实际执行旧版本到
桥接版再到 Qt 完整包、跳过桥接的手动迁移、运行中安装提示、卸载后重新
安装、最低 Windows 版本、GNOME / Plasma Wayland、FUSE 挂载，以及完整
物理 DPI、IME、GPU、音频和长期资源验证。

清除开发环境变量与核对实际加载路径证明对应包内模块被使用；当前 runner
仍然装有构建工具，不能据此声称已经在完全没有开发工具的干净机器通过。
