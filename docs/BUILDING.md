# 构建与发布

## 开发环境与版本

- Windows x64；应用继续面向 Windows 10 1803 及以上。
- Visual Studio 2026，安装 **Desktop development with C++**、MSVC v145、ATL 和 Windows SDK。
- PowerShell 7、Git、Rustup。

ScreenCapture-Plus 使用自己的版本序列，首个版本为 **v0.0.1**，Windows 文件版本为 **0.0.1.0**。开发构建默认值集中在 `build/windows/Version.props`；它不再沿用上游的 2.5.x 版本。构建标签覆盖这组资源值，不需要编辑生成的 EXE。

依赖固定在 `build/dependencies.lock.json`：Ling `2ae45005b2863b004b4432bfe85d6e63d7ee44ca`、gifski 1.34.0 的 `1060eab4500a20f27e2fa3ab7e85473d0e921cbd`、Rust 1.88.0。首次构建需要下载固定源码、工具链及 Cargo.lock 中的依赖。还原脚本验证远端与提交，遇到被修改的依赖工作区会停止。

正式配置为 **x64 Debug / Release**。若要支持 x86 或 ARM64，应先补全编码器和原生依赖链，再增加对应验收。

## 本地构建与检查

在仓库根目录运行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/package.ps1
pwsh ./scripts/test-windows.ps1 -Gui
```

`build.ps1` 自动发现 VS2026、恢复固定依赖、构建 gifski 静态库，再执行 MSBuild。Ling 与 Yoga 使用仓库内的薄工程包装固定源码，由项目引用保证顺序。应用和全部静态依赖统一使用静态 CRT，不需要开发者私有磁盘路径。

Debug 同样使用 release 静态 CRT（`/MT`），以匹配 Rust 静态库；保留禁用优化、PDB、运行时检查和普通断言，不在 C++ 编译中定义会选择 debug CRT 的 `_DEBUG`。

主产物位于 `artifacts/bin/x64/Release/ScreenCapture.exe`。符号、中间文件与测试结果均位于 `artifacts`，依赖源码位于 `.deps`；这些生成目录不提交。完成首次依赖构建后，可以打开根目录 `ScreenCapture.slnx` 调试，或运行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore
```

`test.ps1` 编译并运行所有顶层 `tests/*.cpp`，并解析构建、测试和发布 PowerShell 脚本的语法。`test-windows.ps1` 单独构建 `AnnotationRenderTests.vcxproj`，使用软件 D3D11 WARP、Direct2D、DirectWrite 和 WIC，直接执行正式的 `AnnotationRenderer.cpp`，无需显示器、GPU 或应用测试开关。它输出 PNG、测试报告和测试 EXE 到 `artifacts/tests/windows/`。测试边界及桌面验收见 [VALIDATION.md](VALIDATION.md)。

`test-windows.ps1 -Gui` 在 Release 构建完成后，把正式应用的对象文件（排除入口 main.obj）、资源和静态库链接到独立 GUI 测试程序，驱动真实 WinPin、工具条和共享备注编辑器，并保存桌面截图及实际剪贴板 PNG。测试不向产品增加测试参数；使用独立配置目录，进程超过 45 秒就终止。它要求可用的 Windows 输入桌面和锁定 Ling 所需的硬件 D3D11 设备；能力不足会明确失败，不能当作 GUI 通过。

按标签验证版本一致性：

```powershell
pwsh ./scripts/build.ps1 -Version v0.0.1
pwsh ./scripts/package.ps1 -Version v0.0.1
```

标签接受 `vMAJOR.MINOR.PATCH` 或不带 v 的形式，预发布可以使用 `-rc.1` 等后缀。Windows 第四段固定为 0，每个数字分量不得超过 65535。预发布应在 GitHub Release 中标记为 Pre-release；内置自动更新只处理稳定的三段版本。

## GitHub Actions

唯一工作流是 `.github/workflows/windows.yml`，与本地共用脚本。PR 和 main push 检出触发事件的精确 SHA，发布构建则检出指定标签。

| 触发 | 构建与发布行为 |
| --- | --- |
| Pull request | Release、核心回归、Windows 原生备注渲染、独立 EXE 启动、Debug、打包及真实 GUI/剪贴板集成；不发布版本 |
| main push | 执行同样验证；只有满足下述首次发布声明时才创建 v0.0.1 |
| Release `published` | 按该 Release 标签构建、测试、打包，向这个已有 Release 上传资产 |
| 手工运行，不填标签 | 构建选择的提交，仅保存 artifact |
| 手工运行，填写已有 Release 标签 | 按该标签重建并上传，支持临时构建或上传失败后的恢复 |

Windows job 使用 `windows-2025` 的 VS2026 环境，仅有 `contents: read`。发布 job 在整个构建 job 成功后取得 `contents: write`，使用仓库提供的 `GITHUB_TOKEN`；不要求另建 PAT。所有 Actions 固定到提交 SHA。

工作流保留以下产物：

| Artifact | 内容 | 保留期 |
| --- | --- | --- |
| `windows-x64` | EXE、便携包、符号包、构建元数据、SHA256 校验文件 | 14 天 |
| `windows-render-checks` | 原生渲染与 GUI 测试 EXE/PDB、报告、备注导出 PNG、真实窗口截图和剪贴板 PNG；失败时也尽量保留 | 14 天 |
| `windows-build-logs` | 失败时的 MSBuild binlog 与 gifski 构建日志 | 7 天 |

真实 GUI 检查位于可下载 EXE artifact 上传之后。因此即使 Runner 缺少桌面/图形能力，之前的编译、离屏渲染和打包证据也已保留；整个构建 job 仍会失败，阻止自动首发。

## 首次发布 v0.0.1

本仓库为明确授权的首次发布设置了一个固定入口，不会从普通提交推导新版本号。功能验证完成后，将 `releases/v0.0.1.md` 发布说明加入 main，便声明这次发布。工作分支与 PR 不执行首次发布 job。

对应 main 提交通过全部构建、测试和打包后，`first-release` job 下载同一次运行的 `windows-x64`，然后：

1. 确认固定仓库、main push 与非空声明文件；已有正式 v0.0.1 则直接结束，保留原版本。
2. 确认 main 尚指向本次测试的 SHA，检出与构建元数据也匹配该 SHA，EXE 版本为 0.0.1.0，并验证完整资产集合及每个 SHA256。
3. 确认不存在同名标签或草稿，再通过创建引用接口原子建立 `v0.0.1`，指向本次验证的提交；已存在的引用不会被移动或复用。
4. 复核标签 SHA，使用 `gh release create --verify-tag` 创建 Release、上传已验证资产并发布。

GitHub CLI 在带资产创建 Release 时先创建草稿、上传资产，再发布。由于 `GITHUB_TOKEN` 产生的 Release 事件不会再触发此工作流，首次 job 直接上传本轮产物，不依赖第二轮构建。

已有草稿或仅有标签时，该入口停止并要求维护者检查，不自动覆盖。若是前次上传中断，应先确认标签 SHA 和成功构建一致，再在 Releases 页面完成对应草稿或为现有标签发布 Release。源码修复应进入新的版本，避免修改已发布标签。

## 后续常规发布

后续版本继续使用 Release `published` 流程：

1. 合并需要发布的代码，确认对应 Windows CI 成功，并更新开发默认版本。
2. 在 Releases 页面创建标签，例如 `v0.0.2`，指向确认过的提交。
3. 填写说明并点击 **Publish release**；仅保存 Draft 不会启动常规工作流。
4. 等待 **Windows build and release** 完成。Release 页面先出现，EXE 等资产在构建通过后上传。

两种入口均提供：

- `ScreenCapture.exe`：单文件入口，名称也是更新器识别的固定资产名。
- `ScreenCapture-Plus-<tag>-windows-x64.zip`：EXE、额外语言、许可证与构建信息。
- `ScreenCapture-Plus-<tag>-windows-x64-symbols.zip`：定位崩溃用的 PDB。
- `SHA256SUMS.txt`：资产下载校验。
- `build-info.json`：源提交、标签、架构、工具链、固定依赖与 EXE SHA256。

打包检查版本资源，并通过 `dumpbin /dependents` 拒绝意外的 MSVC 运行库或 Ling/Yoga/gifski 外部 DLL。OCR 功能、入口及插件查找已经移除，不再安装或分发 OCR 插件；二维码识别与其 quirc 依赖保留。

常规 Release job 若因临时网络或 Runner 故障失败，可重跑失败 job，或手工填写已有标签重建；常规上传会替换同名资产，但不移动源码标签。若源码本身需要修复，应发布新的版本。组织若限制 token 写权限，日志会记录发布失败原因，应按仓库权限策略处理。

## 自动更新与版本一致性

更新器读取 `NAMEWTA/ScreenCapture-Plus` 的 latest Release，选择同一份元数据内的 `ScreenCapture.exe` 地址，并核对下载文件内嵌版本。它不再访问原作者 COS 的版本或 EXE 地址。

更新检查限于常驻模式、空闲时机与原有每日频率；安装前请求用户确认。缺资产、预发布、不合法标签或网络失败均静默跳过。

官方依据：[Release 事件](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#release)、[工作流权限](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#permissions)、[GITHUB_TOKEN 与事件触发](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow)、[GitHub CLI 创建 Release](https://cli.github.com/manual/gh_release_create)、[创建 Git 引用](https://docs.github.com/en/rest/git/refs#create-a-reference)、[Windows Runner 迁移](https://github.blog/changelog/2026-05-14-github-actions-upcoming-image-migrations/)、[Rust 静态 CRT](https://doc.rust-lang.org/reference/linkage.html)。
