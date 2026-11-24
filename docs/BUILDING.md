# 构建与发布

## 开发环境

- Windows x64；应用继续面向 Windows 10 1803 及以上。
- Visual Studio 2026，安装 **Desktop development with C++**、MSVC v145、ATL 和 Windows SDK。
- PowerShell 7、Git、Rustup。

编译依赖固定在 `build/dependencies.lock.json`：Ling `2ae45005b2863b004b4432bfe85d6e63d7ee44ca`，gifski 1.34.0 的 `1060eab4500a20f27e2fa3ab7e85473d0e921cbd`，Rust 1.88.0。首次构建需要下载这些源码、Rust 工具链及 Cargo.lock 中的依赖。还原脚本会验证远端及提交，遇到被修改的依赖工作区会停止，避免覆盖开发者的工作。

当前可重复构建的正式配置为 **x64 Debug / Release**。此前的 Win32 工程项缺少完整依赖配置，不作为可用发布目标沿用；如需 x86/ARM64，需要单独补全编码器和原生依赖链并验收。

## 本地构建

在仓库根目录运行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/package.ps1
```

构建脚本自动发现 VS2026，恢复固定依赖，构建 gifski 静态库，再执行 MSBuild。Ling 和 Yoga 用仓库内的薄工程包装固定源码，由项目引用保证顺序。所有 C++ 与 Rust 静态库使用静态 CRT；无需在源码内设置开发者私有盘符。

Debug 同样使用 release 静态 CRT（`/MT`），以匹配 Rust 静态库；保留禁用优化、PDB、运行时检查和普通断言，不在 C++ 编译中定义会选择 debug CRT 的 `_DEBUG`。

产物：`artifacts/bin/x64/Release/ScreenCapture.exe`。符号与中间文件也位于 `artifacts`，依赖源码位于 `.deps`，两者均不提交。完成首次依赖构建后可打开根目录 `ScreenCapture.slnx` 调试，或运行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore
```

不要手工编辑生成的 exe 版本。`build/windows/Version.props` 保存开发构建默认值，发布构建从标签覆盖：

```powershell
pwsh ./scripts/build.ps1 -Version v2.5.17
pwsh ./scripts/package.ps1 -Version v2.5.17
```

版本示例不代表已经发布该版本。标签接受 `vMAJOR.MINOR.PATCH` 或不带 v 的形式，预发布可用 `-rc.1` 等后缀；Windows 第四段固定为 0，每个数值不得超过 65535。预发布标签应在 GitHub Release 中同时勾选 Pre-release，自动更新只处理稳定的三段版本。

## GitHub Actions

唯一工作流是 `.github/workflows/windows.yml`，与本地共用脚本。

| 触发 | 执行与结果 |
| --- | --- |
| Pull request | 构建 Release、运行核心回归及独立 EXE 托盘启动检查、检查 Debug、打包，提供 workflow artifact |
| main push | 执行同样验证，提供 workflow artifact |
| Release `published` | 按这个 Release 的标签检出，构建并验证，上传资产到这个已有 Release |
| 手工运行，不填标签 | 构建当前选择的 ref，仅保留 artifact |
| 手工运行，填写已有 Release 标签 | 按该标签重建，验证后重新上传资产，适合失败恢复 |

工作流使用 `windows-2025` 的 VS2026 环境。构建 job 只有 `contents: read`；发布 job 在构建成功后取得 `contents: write`，使用仓库自动提供的 `GITHUB_TOKEN` 上传，不要求新增 PAT 或 NPM_TOKEN。所有使用的 Actions 固定到提交 SHA。

## 发布操作

1. 先把需要发布的代码合并到默认分支，确认对应 Windows CI 成功。
2. 在仓库 Releases 页面新建标签，例如 `v2.5.17`，目标必须包含此工作流和发布代码。
3. 填写说明并点击 **Publish release**。只保存 Draft 不会启动此工作流。
4. 等待 **Windows build and release** 完成。GitHub 在发布瞬间先创建 Release 页面，构建完成后才会出现 EXE 资产。

成功后包括：

- `ScreenCapture.exe`：单文件运行入口，名称也是内置更新器识别的稳定资产名。
- `ScreenCapture-Plus-<tag>-windows-x64.zip`：EXE、额外语言、许可与构建信息。
- `ScreenCapture-Plus-<tag>-windows-x64-symbols.zip`：定位崩溃用的 PDB。
- `SHA256SUMS.txt`：下载校验。
- `build-info.json`：源提交、标签、架构、工具链、依赖锁与 EXE SHA256。

打包时会核对 EXE 的版本资源，并通过 `dumpbin /dependents` 拒绝意外引入的 MSVC 运行库或 Ling/Yoga/gifski 外部 DLL。OCR 插件保持原有独立安装方式，不被打包脚本静默捆绑。

若 Release job 失败，在 Actions 中查看具体步骤及 `windows-build-logs`。修复代码后需要发布指向修复提交的新标签；如果只是暂时的下载/Runner 故障，可以重跑失败 job。手工填入标签重跑只替换该 Release 的资产，不创建新的 Release。

新 fork 若被 GitHub 暂停 Actions，需要先在 Actions 页面启用；组织若额外限制 token 写权限，发布 job 会明确失败，应按仓库权限策略开放 `contents: write`。本项目工作流本身不会修改组织权限。

## 自动更新与版本一致性

更新器读取 `NAMEWTA/ScreenCapture-Plus` 的 latest Release，查找同一份 Release 元数据里的 `ScreenCapture.exe` 下载地址，再核对下载文件的内嵌版本。它不再访问原作者 COS 的版本与 EXE 地址，也不使用可能在两个请求之间变化的 `/latest/download` 资产链接。

更新检查仍限于常驻模式、空闲时机和原有每日频率；更新前仍请求用户确认。未发布资产、预发布、不合法标签或网络失败均沿用静默跳过的行为。

官方依据：[Release 事件](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#release)、[工作流权限](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#permissions)、[Windows Runner 迁移说明](https://github.blog/changelog/2026-05-14-github-actions-upcoming-image-migrations/)、[Rust 静态 CRT](https://doc.rust-lang.org/reference/linkage.html)、[Cargo rustc](https://doc.rust-lang.org/cargo/commands/cargo-rustc.html)。
