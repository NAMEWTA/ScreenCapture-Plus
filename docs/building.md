# 构建与发布

本地和 GitHub Actions 使用同一组脚本，避免本地可构建、发布时却采用另一套依赖与参数。默认构建版本为 **v0.0.4**，Windows 文件版本为 **0.0.4.0**。版本号来自 [Version.props](../build/windows/Version.props)，发布意图来自 [publish.json](../releases/publish.json)。

## 环境与依赖决策

目标为 Windows 10 1803 及以上、x64。开发环境使用 Visual Studio 2026 的 C++ 桌面组件、MSVC v145、ATL、Windows SDK，以及 PowerShell 7、Git 和 Rustup。依赖版本与提交的唯一来源是 [依赖锁文件](../build/dependencies.lock.json)。

Ling 和 Yoga 通过仓库工程包装并建立项目引用，gifski 构建为 Rust 静态库。应用和依赖使用静态 CRT，保持单 EXE 分发；Debug 同样使用 `/MT` 以匹配静态依赖，同时保留符号、运行时检查和未优化调试信息。不要通过加入运行时 DLL 掩盖不一致的 CRT 配置。

恢复脚本核对远端、固定提交和本地修改。升级依赖应更新锁文件、检查 API 与许可，执行原生构建；不能只修改 `.deps` 中的源码。新增架构需补齐完整依赖链和验收，不只是增加一个 MSBuild 平台名。

## 本地入口

在仓库根目录执行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

Release EXE 位于 `artifacts/bin/x64/Release/huhucapture.exe`。首次依赖恢复后，可以打开 `huhucapture.slnx` 调试。改动构建参数或依赖时还执行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore
```

生成物集中在 `artifacts`，依赖恢复到 `.deps`，两者均不提交。测试的能力边界、证据和物理设备验收见 [验证指南](validation.md)。

## 发行资产的分工

| 资产 | 发布理由 |
| --- | --- |
| `huhucapture.exe` | 用户直接运行的单文件，也是更新器查找的固定资产名 |
| `huhucapture-<tag>-windows-x64.zip` | 同一 EXE 加语言、许可/声明与构建信息，适合完整保存分发内容 |
| `huhucapture-<tag>-windows-x64-symbols.zip` | 同构建 PDB，供调试器解释地址与调用栈，单独分发以控制普通用户下载量 |
| `build-info.json` | 源提交、版本、架构、工具链、锁定依赖和 EXE 哈希，为故障报告与构建追溯提供依据 |
| `SHA256SUMS.txt` | 核验其余四项下载内容是否与发布资产一致 |

符号包有独立维护价值，因此保留。删除它会失去定位发行版崩溃的重要材料。PDB 必须匹配生成发行 EXE 的同一次构建；同源代码或同版本号重新编译不能保证匹配。普通使用无需下载它，调试加载步骤与全部下载选项见 [README](../README.md#为什么保留-symbolszip)。

GitHub 自动提供的源码 ZIP / tar.gz 是标签快照，不属于这五项构建资产，也不是可运行程序。便携 ZIP 包含依赖许可；分发不能因单 EXE 便利而删除必要声明。

## CI 与发布门槛

[Windows 工作流](../.github/workflows/windows.yml) 固定 Actions 提交，检出触发事件的精确源码。构建任务只具有读取权限，发布任务在全部验证通过后才获得写入发行资产的权限。

| 入口 | 行为 |
| --- | --- |
| Pull request | 完整验证与 artifact，供审阅；不发布 |
| main push | 完整验证后按显式声明发布，已有正式版本保持不变 |
| Release `published` | 检出指定 tag 验证，并上传到该已存在 Release |
| 手工运行，不填标签 | 验证所选提交并保存 artifact |
| 手工运行，填写已存在 Release 标签 | 检出该 tag 重建和上传，供恢复使用 |

发布要求 Release / Debug（常规 main、PR）、核心与脚本检查、原生渲染、单 EXE 启动、真实窗口/剪贴板和打包门槛全部通过。打包核对版本与产品资源，检查 PE 依赖，拒绝意外增加外部 MSVC、Ling、Yoga 或 gifski DLL。

`windows-x64` 保留 EXE、便携包、符号、构建信息和校验和；`windows-render-checks` 保留原生/GUI 报告与图片；失败时尽量保留 `windows-build-logs`。具体保留期以工作流为准。程序 artifact 可能在后续 GUI 检查前已经上传，因此“能下载 artifact”本身不证明完整构建成功。

## 声明新版本

一次发布在同一提交中修改：

1. `build/windows/Version.props` 默认版本；
2. `releases/publish.json` 的稳定 tag 与说明路径；
3. `releases/<tag>.md`、更新记录和当前下载说明。

发布声明示例：

```json
{
  "schemaVersion": 1,
  "tag": "v0.0.4",
  "notes": "releases/v0.0.4.md"
}
```

稳定标签使用 `vMAJOR.MINOR.PATCH`，Windows 第四段固定为 0，各数字分量受 Windows 版本资源范围约束。可在手工预发布流程使用后缀，但内置更新器只处理稳定三段版本。构建参数覆盖版本时，构建与打包必须使用同一个标签。

main 构建成功后，[声明发布脚本](../scripts/publish-release.ps1) 使用本次已验证的 artifact，核对仓库、源 SHA、main 当前头、版本、架构、五个资产和哈希。只有一致才创建新 tag 和 Release。它不会通过重新构建另一份内容来完成这次发布，也不会自行推导下一个版本号。

已有正式 Release 时不再重复发布。main 已前进时旧运行不发布过时提交，新的运行重新完成验证。发布并发串行化；GitHub token 创建的发布不依赖再触发一轮构建来补资产。

## 失败与恢复

编译、测试或打包失败时修复原因再运行，不能跳过失败检查。Runner 或网络临时失败可重跑失败任务；对应源码未改变时，无需为一次重跑修改版本号。

如果留下同名草稿或未发布 tag，先核对它指向的 SHA、成功构建和资产元数据。声明脚本会停止而不移动已有引用，维护者应完成匹配的草稿/标签流程，不能通过强行覆盖掩盖不一致。

已有 Release 可通过手工标签入口重建和上传，但需要保持 EXE、PDB、构建信息与哈希作为同一组，不能只替换其中一个文件。源码修复发布新的补丁版本，保留旧版本的可追溯性。

问题报告引用实际 [Actions](https://github.com/NAMEWTA/huhucapture/actions/workflows/windows.yml) 与 [Release](https://github.com/NAMEWTA/huhucapture/releases)。本文定义流程，不表示某个待发布提交已通过全部检查。
