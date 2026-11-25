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

同一独立探针还先启动四个真实已安装控制进程：没有匹配主实例的 Finish、
动作前后两种顺序的缺失/空便携目录，以及畸形验证环境。每个都必须在期限内
正常返回 2，且从未观察到其原生产品窗口；错误对话框、选主后开始截图、崩溃
或超时均失败。`controlRejections` 保留各自 PID、返回码和诊断，然后才继续
原本的截图拖选与关闭旅程。此检查不代替成功的主实例控制或 GNOME 菜单验收。

探针读取系统的原生窗口标题。产品显式设置 `applicationDisplayName` 后，固定
Qt 6.12.1 的 [`QPlatformWindow::formatWindowTitle`](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/gui/kernel/qplatformwindow.cpp)
附加产品名；XCB 使用 ` — `，Windows 使用 ` - `。因此逻辑 QWidget 标题
`Screen capture` 不是实际系统标题。探针精确匹配平台格式，并继续核对进程归属、
原始可执行文件 hash、包内模块和拖选后的像素；不以标题前缀接受任何产品窗口。

提交 `659cc9d7f882e3fd1d92cdb6f2360ed2c31ab735` 的
[完整应用 CI](https://github.com/NAMEWTA/huhucapture/actions/runs/45649036685)
已在 Ubuntu X11 runner 完成两种 Linux 包的上述旅程：DEB 安装、同包重装、
移除与 purge，AppImage 免 FUSE 运行、同文件替换与移除；每种包首次和重装后
均通过真实截图拖选、像素核对和正常退出。四条 GUI 旅程共执行 16 个实际
控制拒绝进程，全部返回 2 且未观察到产品窗口。DEB 六个、AppImage 五个
数据检查点保留原配置、未知字段及 PNG / MP4 字节。证据归档为该次运行的
`qt-application-linux`，其中 `installation-results.json` 保留全部实际路径和 hash。
CI 的合并提交身份为 `7bf1f08908af6664fae81e04486b3eeee6ae7fcd`，源码树与
上述分支提交相同；没有改写产物中的真实构建身份。

同轮 Windows Server 2022 runner 已生成真实 NSIS 安装包和便携 ZIP。
实际目录冲突检查非零退出并保留用户文件；正常安装仍返回 2，未完成安装登记。
原 artifact 内有 760 个与包清单完整相同的程序文件，下一文件是完整安装路径
长达 263 字符的 Rust 标准库通知，从该文件起共 417 个文件未安装。这与固定 NSIS 的
普通 Win32 路径限制一致；它不表示程序已成功安装或运行。
[工具链通知](rust-toolchain-materials.md)已改用保留来源映射的紧凑布局，完整
许可原文保持不变。修正后的 NSIS 安装、DLL 占用下的重试/卸载和便携 ZIP
旅程仍待新原生 CI，不通过缩短测试目录或开启全机长路径策略规避检查。

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

## Windows 原生失败与重试

同一 Windows 旅程还执行三种真实系统失败，不使用替身安装器或预设退出码：

- 先在所需的一个 payload 子目录位置创建普通用户文件。静默安装必须有界非零
  退出，不发布成功安装登记，并保留该文件和所有配置、媒体的原始字节。
- 正常安装并运行产品后，以 `CreateFileW` 打开实际清单中的 DLL，只允许共享
  读取，保持真正的写入/删除共享冲突。此时同包重装必须非零退出，并保留
  卸载器、安装登记、被锁文件及用户数据。
- 保持同一文件锁，再运行本次真实卸载器，要求同样的失败和可重试状态。
  释放锁后，正常同包重装必须恢复完整 payload、通过产品 GUI 旅程，再完成卸载。

卸载探针把已登记卸载器的原始字节复制到本轮独占目录，以 NSIS 的末尾
`_?=安装目录` 参数运行。这样等待的是实际卸载进程，安装目录内的卸载器也
不再因自身运行而被占用。原始 NSIS launcher 的提前返回不算卸载完成。

删除失败前可能已移除较早的受管程序文件；报告明确保留这一事实。可重试
登记、卸载器和数据保护不构成整个文件系统及注册表的事务回滚承诺。
这些场景只有在原生 CI 实际产生相应日志后才算通过；本地 Python 语法检查
和模板审查均不能代替 Windows 执行。

## 仍需单独验收的内容

同一包的重装不是跨版本升级，静默安装不是安装向导的物理交互验收，Xvfb
不是实体多屏。报告明确记录这些边界，不把缺项隐去。另需实际执行旧版本到
桥接版再到 Qt 完整包、跳过桥接的手动迁移、运行中安装提示、卸载后重新
安装、最低 Windows 版本、GNOME / Plasma Wayland、FUSE 挂载，以及完整
物理 DPI、IME、GPU、音频和长期资源验证。

清除开发环境变量与核对实际加载路径证明对应包内模块被使用；当前 runner
仍然装有构建工具，不能据此声称已经在完全没有开发工具的干净机器通过。
