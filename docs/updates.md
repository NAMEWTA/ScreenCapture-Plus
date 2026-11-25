# 更新下载与旧版桥接

## 入口与用户控制

“设置 → 关于 → 下载最新版本”取得固定仓库 `NAMEWTA/huhucapture` 的最新稳定发行。用户主动下载时，即使正在运行同一个版本，也可以再次下载。后台每日检查仍只在存在更高稳定版本、且应用空闲时提示。

下载任务属于应用，不属于关于页。关闭或重新打开设置只改变订阅；正在进行的下载继续运行，重开的页面显示同一个任务。退出应用先停止向界面发通知，取消实际 WinRT 请求，再等待后台线程关闭网络、文件并完成本任务的临时文件清理。

下载不等于已经安装。服务不会执行文件、自动解包、覆盖运行中的程序或重启。完整安装器需要用户退出旧程序后手动运行；便携 ZIP 必须解压整个包并保留其目录结构，不能只提取 EXE。关于页显示包对应的说明、实际文件位置和打开文件夹操作。

## P11a 的选择与校验

[旧版更新入口](../src/services/Update.cpp)使用独立的 [WinRT 下载任务](../src/services/update/WindowsUpdateDownload.cpp)，不引入 Qt 或另一套 JSON 解析器。它与 Qt 更新器共享 [纯契约规则](../src/core/ReleasePackage.h)、版本规则、文件身份和排他提交组件；完整元数据格式见 [Qt 更新协议](qt-updates.md)。

1. 固定 GitHub latest API 必须声明稳定发行、规范 `vX.Y.Z` tag、本仓库发行页和规范资产 URL。资产名称唯一、长度有界、SHA256 完整；不接受任意仓库或任意下载地址。
2. 当该发行包含 `release-manifest.json` 时，先核对实际清单长度和 SHA256，再选择 `windows/x64` 的完整安装器；发行仅提供完整 ZIP 时选择 ZIP。两者存在时优先安装器。清单与平台 `build-info-windows-x64.json` 的 schema、产品、仓库、版本、tag、源码提交、包名称、大小和哈希必须一致。
3. `developmentOnly`、`sourceDirty` 若出现，必须是 JSON 布尔值 `false`。`true`、字符串、数字和 `null` 均拒绝。依赖清单至少包括 Qt、gifski、LocalSend、FFmpeg 的版本、实际许可和 HTTPS 来源；GPL/AGPL 等实际许可不被错误排除。所选包的文件清单必须包含非空应用、Qt Core 与 Windows 平台插件。`contentRoot` 指定不同包的独立目录前缀，不能借用另一个包的运行库通过检查。
4. **只有该发行根本没有列出完整包清单时**，才兼容旧的 `huhucapture.exe` 资产。该路径继续使用 64 MiB 上限、实际 x64 可执行 PE 和嵌入版本检查。已列出的清单缺失、损坏、大小超限或内容不合要求时直接失败，不降级下载同发行的旧 EXE。
5. 安装器上限为 1 GiB，便携 ZIP 为 2 GiB。安装器需要真实 PE 可执行标记及匹配的嵌入版本；允许 NSIS 的 x86 引导程序安装清单绑定的 x64 应用。DLL 不能冒充可执行程序。ZIP 需要正确类型头，并完整满足链上的大小、哈希和包身份检查。

latest/manifest 正文上限为 1 MiB，平台 build-info 上限为 8 MiB。文件正文通过 WinRT `ResponseHeadersRead` 和输入流以 64 KiB 分块处理，一边写入独有 `.part`，一边计算 SHA256，不把完整安装包读进内存。HTTP 的 Content-Length、实际接收长度和清单长度分别校验；没有 Content-Length 的分块响应仍受精确正文预算约束。单个读操作和整个资源传输都有限时；自动重定向关闭，只接受有上限的 GitHub HTTPS 资产跳转。生产路径不接受外部 URL 配置或环境变量。

实现依据是 Microsoft 的 [HttpClient 流式读取说明](https://learn.microsoft.com/en-us/uwp/api/windows.web.http.httpclient)、[HttpCompletionOption](https://learn.microsoft.com/en-us/uwp/api/windows.web.http.httpcompletionoption) 和 [C++/WinRT 线程与异步操作](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/concurrency)。阻塞等待只发生在专用 MTA 工作线程，不依赖界面消息循环完成网络操作。

## 系统目录与文件边界

目标目录来自 Windows [Downloads 已知文件夹](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath)，跟随用户实际重定向。生产路径不拼接用户主目录，也不改存应用目录或 Documents 媒体目录。受控测试显式提供其独有的临时 Downloads 目录，不改用户系统设置。

完整包保留发行名称，例如 `huhucapture-vX.Y.Z-windows-x64-setup.exe` 或 `huhucapture-vX.Y.Z-windows-x64.zip`。旧 EXE 回退在本地保存为 `huhucapture-vX.Y.Z-windows-x64.exe`。同名文件存在时排他提交 `-001` 等新名称；既有下载不被覆盖。

临时文件由共享 FileCommitter 排他创建，任务记录其身份，打开同一个空文件后才写入。校验完成时核对打开句柄、路径快照和精确长度，再按现有提交契约发布。失败或取消只清理仍属于该任务身份的文件；无法安全清理或发布后持久化不确定时保留路径并报告失败，不能删除已交付文件或伪报成功。这里沿用调用方独占临时文件命名空间的契约，不宣称可以抵御同一账户在关闭写句柄与提交之间主动修改文件的攻击。

## 过渡发行与手动迁移

尚未安装桥接版的 v0.0.5 只认识旧单 EXE。当 Qt 完整包成为 latest 后，它不会自动回溯历史发行寻找桥接版本。用户可直接打开 [官方发行页](https://github.com/NAMEWTA/huhucapture/releases/latest)，下载对应 Windows 完整安装器或 ZIP，退出旧程序后手动安装或解压完整目录。不能将缺依赖的 Qt 裸 EXE 或改名安装器伪装为旧资产。

P11a 仍由现有 Windows 构建和生产验证门槛控制；代码实现不代表已经过渡发行。本次工作不创建版本、tag 或 Release。桥接的实际完整包下载、原 Windows 回归及独立过渡发行完成后，才满足相应计划项；Qt 双平台首发另受完整包和对应源码门槛控制。

## 验证范围

[Windows 专用 HTTP 驱动](../scripts/test-windows-update-bridge.py)启动真正的本地 HTTP 服务，调用编译了生产 WinRT worker 的 [原生测试入口](../tests/windows/WindowsUpdateDownloadTests.cpp)。它覆盖同版本下载、65 MiB ZIP、同名保留、流式取消和线程退出、真实测试 PE 的版本与 DLL 标记拒绝、无清单的 EXE 回退、清单错误不得回退、严格开发标记、仓库/架构/提交/依赖/目录前缀、错误哈希、截断、分块超长和重定向。

```powershell
python scripts/test-windows-update-bridge.py --client artifacts/tests/windows/WindowsUpdateDownloadTests.exe --evidence artifacts/tests/windows/update-bridge
```

默认 ZIP 和测试 PE 明确标为传输测试样本，不能证明真实产品完整或可安装。准备好完整、干净的实际发行资产后，可额外传入 `--release-fixture <目录>`；它保留原始 manifest/build-info 字节及开发标记，绝不把开发包重新标成正式发行以通过验证。

`scripts/test-windows.ps1 -Update` 继续使用真正的 GitHub latest、系统 Downloads 和设置页关闭重开验证，兼容新的包形态。原生 Windows 编译、真实完整包下载、用户级安装/升级/卸载、网络跳转、已知文件夹重定向和物理桌面验收必须各自记录，不能从代码审阅或本地契约测试推断已经完成。
