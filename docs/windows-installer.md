# Windows 安装器的失败与保留边界

Windows 安装包继续使用 [打包锁](../build/package/dependencies.lock.json) 中的
NSIS 3.11。实现入口为 [用户级模板](../build/package/user-installer.nsi.in)
及 [打包器](../scripts/package-qt.py) 的 `windows_installer`。完整产物和版本
流程见 [Qt 打包](qt-packaging.md)，原生验证入口见 [安装验证](installation-validation.md)。

## 安装失败

固定 NSIS 编译器 `Source/script.cpp` 默认允许忽略文件错误，把静默安装的
默认回答设为 `IDIGNORE`。运行时 `EW_EXTRACTFILE` 创建文件失败收到 Ignore
后，只设置错误标志并继续。`SetOutPath` 创建或切换目录失败、`Delete` 删除
文件失败也不自动中止。成功生成安装包不能证明目标系统上的这些操作成功。

模板使用 `AllowSkipFiles off`，每个受管文件的 `SetOutPath`、`File`、`Delete`
都有相邻的错误检查。删除后再检查准确路径是否仍存在，避免把目录占据文件名
当作成功。自行显示的消息均有 `/SD` 默认值；文件占用时的静默操作直接失败，
不等待无人可见的 Retry/Cancel。两个执行段进入时设置退出码 2，全部必要
步骤结束才改成 0，也覆盖 `File` 在 NSIS 运行时内部直接中止的情况。

程序文件、卸载器、快捷方式及 Windows 应用登记成功后，才写入
`Installation\Location`。失败不显示成功，也不自动启动应用。

## 同路径重装

只对当前用户登记的相同安装路径调用旧卸载器。普通 NSIS 卸载器会启动临时
子进程，启动进程退出不代表卸载完成；直接以 `_?=` 在安装目录运行又使其
自身 EXE 保持占用。模板将真实旧卸载器复制到本次 NSIS 所有的 `$PLUGINSDIR`，
检查复制结果，再用 `/S _?=$INSTDIR` 和 `ExecWait` 检查启动错误及实际卸载
退出码。这既等待完成，也允许删除原安装目录的卸载器，不安排重启后删除。

这不是事务升级。旧卸载可能先删除若干受管文件，再遇到锁定的 DLL；新安装
此时中止，剩余文件、卸载器和登记保留，可关闭占用进程后重试。固定
`util.c::myDelete` 在普通 `Delete` 无匹配时不会设置错误，因此重试能经过
此前已删除的文件。若旧卸载已成功，新文件随后写入失败，可能留下不完整的
新程序目录，须修复权限或空间后重装，不承诺恢复整套旧程序。

若登记路径相同但旧卸载器已经缺失，保持原有写入新清单的行为，不能证明
旧清单独有文件全部退役。更早发布版本的卸载行为取决于那个真实版本；本次
源码审查与同版本重装都不构成旧发行版的跨版本升级验收。

## 卸载

仅处理该包的明确文件清单。受管文件、快捷方式或安装目录内卸载器删不掉
时，立即停止并保留安装登记；受管文件删除失败发生在删除卸载器之前，仍
可通过它重试。此前成功删除的受管文件不会伪装成仍存在，也不复制回去。

目录仅用不带 `/r` 的 `RMDir` 清理。用户放入的未知文件使目录保留，不构成
删除这些文件的授权，也不使成功移除程序文件变成失败。配置、媒体、Documents
及其他未列出的路径均不递归清理。

程序文件和快捷方式全部删除后，才清理仍指向相同位置的登记。登记读取或
清理失败单独报告并返回非零；这时程序文件已经移除，登记可能部分保留，需要
修复登记权限后重装或清理剩余登记。没有文件系统与注册表原子回滚保证。

## 验证边界

这些问题首先来自固定源码审查，不是未执行的 Windows 复现。当前本地检查
包含 Python 语法和补丁格式，没有本地 Windows/NSIS 执行结果。原生验证必须
使用实际完整开发安装包，保持真实版本、源提交和 `sourceDirty` 标记：

- 用普通文件占据真实 payload 子目录，`/S` 有界失败、不登记成功，原文件及
  用户数据保留。
- 用真实 Win32 句柄禁止已安装 DLL 被写入或删除，重装和卸载均非零退出，
  保留卸载器、登记和用户文件。部分更早的受管文件允许已经移除。
- 复制这次真实卸载器到测试独占目录，用 `/S _?=` 等待其实际退出；释放锁后
  重试，校验完整重新安装、产品窗口打开及最终卸载和数据保留。

同一包重装不能命名为跨版本升级。文件锁和目录冲突也不能代替磁盘不足、
ACL 变化、注册表拒绝访问、并发安装、断电及外部改写的验证。

## 固定上游来源

版本为 NSIS 3.11，提交 `7359413009afd4f0fff472d841fc2f2cc0e0a5f8`。下列
完整源文件按 Git blob 核对。复用官方指令和卸载协议，未复制或修改 NSIS
运行时；版权为 Nullsoft and Contributors，许可证为 zlib/libpng，继续保留
官方发行包的组件许可证。

| 固定源码 | Git blob | 核对内容 |
| --- | --- | --- |
| [Source/script.cpp](https://github.com/nsis-dev/nsis/blob/7359413009afd4f0fff472d841fc2f2cc0e0a5f8/Source/script.cpp) | `ea6e1b45a8677a3a882351451a7c93317084d208` | AllowSkipFiles 的静默默认值、CopyFiles 无交互标志 |
| [Source/exehead/exec.c](https://github.com/nsis-dev/nsis/blob/7359413009afd4f0fff472d841fc2f2cc0e0a5f8/Source/exehead/exec.c) | `95b0a44119d5fc3305e5a10a91a31031d5d0b980` | File、SetOutPath、ExecWait、WriteUninstaller、快捷方式和登记错误 |
| [Source/exehead/util.c](https://github.com/nsis-dev/nsis/blob/7359413009afd4f0fff472d841fc2f2cc0e0a5f8/Source/exehead/util.c) | `271a4165c170356eddc7f2d6a95778f864885074` | Delete/RMDir、静默消息、文件共享方式 |
| [Source/exehead/Main.c](https://github.com/nsis-dev/nsis/blob/7359413009afd4f0fff472d841fc2f2cc0e0a5f8/Source/exehead/Main.c) | `1bd135f7b15d57c867bfdd6a59749b90473de64e` | 临时卸载进程、`_?=`、最终退出码 |

官方指令说明见 [NSIS Scripting Reference](https://nsis.sourceforge.io/Docs/Chapter4.html)。
