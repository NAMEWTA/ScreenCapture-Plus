# huhucapture 构建与发布

本地和 GitHub Actions 共用仓库脚本，构建版本、依赖和发布资产都可追溯。当前稳定版本声明为 **v0.0.1**，Windows 文件版本为 **0.0.1.0**。

## 环境与固定依赖

正式配置为 Windows **x64 Debug / Release**，应用目标为 Windows 10 1803 及以上。开发机需要：

- Visual Studio 2026，安装 C++ 桌面开发、MSVC v145、ATL 和 Windows SDK。
- PowerShell 7、Git、Rustup。
- 首次恢复依赖所需的网络访问。

依赖来自 [build/dependencies.lock.json](../build/dependencies.lock.json)：Ling 固定到 `2ae45005b2863b004b4432bfe85d6e63d7ee44ca`，gifski 1.34.0 固定到 `1060eab4500a20f27e2fa3ab7e85473d0e921cbd`，Rust 使用 1.88.0。恢复脚本核对远端和提交，遇到已修改的依赖工作区会停止。Cargo 依赖遵循相应锁文件。

若增加 x86 或 ARM64，需先补齐编码器和原生依赖链、打包及验收，不能仅增加一个工程平台选项。

## 本地工作流

在仓库根目录运行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

`build.ps1` 自动发现 VS2026，恢复依赖，构建 gifski 静态库并运行 MSBuild。Ling / Yoga 通过仓库内工程包装固定源码，使用项目引用确定构建顺序。应用工程为 `build/windows/huhucapture.vcxproj`，解决方案为 `huhucapture.slnx`。

应用与静态依赖统一使用静态 CRT。Debug 也使用 `/MT`，匹配 Rust 静态库；保留禁用优化、PDB、运行时检查和普通断言，不通过 `_DEBUG` 选择 debug CRT。

| 位置 | 内容 |
| --- | --- |
| `artifacts/bin/x64/Release/huhucapture.exe` | 正式程序 |
| `artifacts` | 符号、中间文件、日志、测试和打包结果 |
| `.deps` | 固定提交的依赖源码 |
| `huhucapture.slnx` | 首次恢复后可用 Visual Studio 打开调试 |

生成目录不提交。构建 / 依赖设置变化时还应执行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore
```

### 验证层次

`test.ps1` 分别编译、运行顶层 `tests/*.cpp`，并解析 PowerShell 脚本语法。`test-windows.ps1` 构建独立原生测试，使用软件 WARP、Direct2D、DirectWrite 和 WIC 运行正式批注 renderer，生成并回读 PNG。

`smoke.ps1` 在隔离目录用便携配置启动正式 EXE 的托盘模式。`test-windows.ps1 -Gui` 另行链接已有 Release 对象、资源和依赖，排除应用入口 `main.obj`，通过独立入口驱动真实编辑窗口和剪贴板。GUI 测试使用隔离配置、45 秒硬超时，需要可用输入桌面和硬件 D3D11；能力不足会明确失败。

测试入口不进入正式 EXE。自动化覆盖与手工验收边界见 [VALIDATION.md](VALIDATION.md)。

## 版本与身份

[build/windows/Version.props](../build/windows/Version.props) 保存默认版本。构建标签可覆盖资源值，无需事后改写 EXE：

```powershell
pwsh ./scripts/build.ps1 -Version v0.0.1
pwsh ./scripts/package.ps1 -Version v0.0.1
```

版本标签接受 `vMAJOR.MINOR.PATCH` 或不带 `v` 的形式，手工预发布可带 `-rc.1` 等后缀。Windows 第四段固定为 0，每个数字分量不得超过 65535。内置更新器只处理稳定的三段版本；GitHub 上的预发布应标记为 Pre-release。

产品名、程序、方案、配置目录和资产前缀统一为 `huhucapture`。运行时品牌与更新源在 `src/app/Brand.h`，仓库元数据为 `NAMEWTA/huhucapture`。改动版本或品牌时核对资源、打包和更新器，不在生成文件中单独修补。

## GitHub Actions

常规工作流为 [.github/workflows/windows.yml](../.github/workflows/windows.yml)，Actions 固定到提交 SHA。PR / main push 检出触发事件的精确 SHA；发布任务检出指定标签。

| 触发 | 行为 |
| --- | --- |
| Pull request | Release、核心测试、脚本语法、原生渲染、启动、Debug、打包和 GUI / 剪贴板检查；不发布 |
| main push | 同样的完整验证；之后按 `releases/publish.json` 声明发布 |
| Release `published` | 按该 Release 的标签构建并检查，上传到已有 Release |
| 手工运行，不填标签 | 验证选择的提交，只保存 artifact |
| 手工运行，填写已有 Release 标签 | 按标签重建并上传，供临时失败后恢复 |

Windows job 使用 `windows-2025` 的 VS2026 环境和 `contents: read`。发布 job 在整个构建成功后使用 `contents: write`，通过仓库 `GITHUB_TOKEN` 发布，无需另建 PAT。

| Artifact | 内容 | 保留期 |
| --- | --- | --- |
| `windows-x64` | EXE、便携 ZIP、符号 ZIP、构建信息、SHA256 | 14 天 |
| `windows-render-checks` | 原生 / GUI 测试程序与符号、报告、渲染 PNG、窗口截图、实际剪贴板 PNG | 14 天 |
| `windows-build-logs` | 失败时的 MSBuild binlog 与 gifski 日志 | 7 天 |

可下载程序 artifact 在 GUI 检查前上传，渲染诊断失败时也尽量保留。因此部分证据可供排错；GUI 检查失败仍使整个构建失败，并阻止声明版本发布。

## 声明一个稳定版本

[releases/publish.json](../releases/publish.json) 明确表达发布意图：

```json
{
  "schemaVersion": 1,
  "tag": "v0.0.1",
  "notes": "releases/v0.0.1.md"
}
```

准备后续版本时，在同一次改动中更新默认版本、声明 tag 和对应发布说明。声明必须使用规范的稳定标签 `vMAJOR.MINOR.PATCH`；说明文件必须是非空的 `releases/<tag>.md`。普通提交不会自行推导新版本。

main 的完整构建成功后，发布 job 下载本次运行的 `windows-x64` 并运行 `scripts/publish-release.ps1`：

1. 验证固定仓库、main push、声明及源提交；已有正式 Release 时结束，保留其原样。没有声明则只保留构建产物。
2. 核对当前 main、检出 SHA 和构建元数据一致，验证 x64 / Release、EXE 版本、五个资产及各自 SHA256。
3. 遇到同名草稿或未发布标签时停止，交由维护者核对；创建标签前再次确认 main，避免旧运行发布已被新提交替代的内容。
4. 原子创建指向已验证提交的新 tag，复核目标 SHA，不移动已存在引用。
5. 创建 Release，上传已经验证的资产，再完成发布。

发布并发按仓库串行化，不因新运行取消正在执行的发布。main 已前进时，旧运行保留 artifact，由新提交验证后的运行处理声明。保留已经发布的声明不会重复创建版本；发布下一版必须显式改动版本和声明。

`GITHUB_TOKEN` 创建的 Release 不会依赖另一轮事件构建来上传文件，因此声明发布直接使用本轮已检查的资产。正常流程不负责重写仓库历史或清理已有发行版。

## 常规发布与恢复

也可使用 GitHub 的 Release `published` 入口，适用于预发布或手工管理：

1. 确认待发布提交对应的 Windows CI 成功，默认版本与目标版本相符。
2. 为该提交创建目标标签，填写发布说明；带预发布后缀的版本勾选 Pre-release。
3. 点击 Publish release。仅保存 Draft 不会触发该入口。
4. 等待 **Windows build and release** 成功，资产随后上传到这个 Release。

临时网络 / Runner 失败可以重跑失败任务，或手工输入已有 Release 标签重建。常规上传会替换同名资产，但不会移动源码标签。源码修复应发布新的版本。若声明发布留下草稿或独立标签，先核对 tag SHA 与成功构建，再完成对应草稿或发布已有标签；不要通过跳过校验掩盖不一致。

仓库或组织限制 token 写权限时，以 Actions 日志中的失败原因排查权限。发布所需输入、资产与版本全部通过检查后才能上传。

## 发布资产与检查

| 文件 | 内容 |
| --- | --- |
| `huhucapture.exe` | 单文件入口，也是更新器查找的固定资产名 |
| `huhucapture-<tag>-windows-x64.zip` | EXE、额外语言、许可与构建信息 |
| `huhucapture-<tag>-windows-x64-symbols.zip` | 问题定位用 PDB |
| `build-info.json` | 仓库、源提交、标签、架构、工具链、固定依赖、EXE SHA256 |
| `SHA256SUMS.txt` | 下载校验 |

打包核对版本资源，同时要求 `ProductName` / `FileDescription` 为 `huhucapture`、`OriginalFilename` 为 `huhucapture.exe`、`CompanyName` 为 `NAMEWTA`。脚本用 `dumpbin /dependents` 拒绝意外依赖 MSVC 运行库或 Ling / Yoga / gifski 外部 DLL。便携包包含必要许可；OCR 插件不属于分发内容，QR 的 quirc 依赖保留。

内置更新器使用 `NAMEWTA/huhucapture` 的 latest 稳定 Release，在同一份响应中选择 `huhucapture.exe`，并核对下载文件版本。检查限于常驻模式、空闲时机和每日频率，安装前请求用户确认。手动更新时先退出正在运行的程序，再替换文件；默认配置位于 `%appdata%\huhucapture`。

实际发布以 [GitHub Releases](https://github.com/NAMEWTA/huhucapture/releases) 的资产及其 `build-info.json` 为准，构建结果以 [Actions](https://github.com/NAMEWTA/huhucapture/actions/workflows/windows.yml) 为准。本文描述流程，不代替具体运行的成功记录。
