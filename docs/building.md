# 构建与发布

本地与 GitHub Actions 使用同一套旧 Windows 生产入口。此版本准备 **v0.0.6 更新桥**，Windows 文件版本为 **0.0.6.0**；实际公开版本以 [Releases](https://github.com/NAMEWTA/huhucapture/releases) 为准。唯一手写版本源为 [build/version.json](../build/version.json)，`VERSION` 与 `build/windows/Version.props` 是受检的派生文件。

## 环境与依赖

目标仍为 Windows 10 1803 及以上、x64。使用 Visual Studio 2026 的 C++ 桌面组件、MSVC v145、ATL、Windows SDK、PowerShell 7、Git 和 Rustup；Python 3.13 用于维护、受控 HTTP 测试与发布验证。依赖版本及来源由 [依赖锁文件](../build/dependencies.lock.json) 指定。

Ling、Yoga 和 gifski 静态链接。应用及依赖使用 `/MT`，Debug 仍保留符号、运行时检查和未优化信息。桥接复用 Windows WinRT 网络与文件 API，不引入 Qt 或新的运行库。升级依赖仍须修改锁文件、许可和实际编译验证，不能修改恢复目录伪装升级。

## 本地验证入口

在仓库根目录执行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore
pwsh ./scripts/test-legacy-update.ps1 -Configuration Debug
pwsh ./scripts/package.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/test-legacy-update.ps1 -Gui
pwsh ./scripts/test-windows.ps1 -Update
```

Release EXE 在 `artifacts/bin/x64/Release/huhucapture.exe`。首次恢复依赖后可以打开 `huhucapture.slnx`。产物放在 `artifacts`，依赖在 `.deps`，均不提交。

`test.ps1` 编译并执行全部标准 C++20 核心测试，解析实际 PowerShell 脚本，还执行版本源与发布准入测试。Windows 版本源检查实际调用 MSBuild 和 PowerShell，拒绝过期派生文件及版本参数覆盖。Linux 上纯规则测试不证明 Windows 编译、原生窗口、网络下载或设备行为已通过，具体边界见 [验证指南](validation.md)。

`test-legacy-update.ps1` 编译并运行生产 WinRT 下载器 probe，受控 HTTP 服务提供真实旧 EXE 与大于 64 MiB 的完整包传输 fixture；`-Gui` 另驱动真实关于页、实际 Downloads 和六种语言。该 fixture 不是可安装 Qt 产品。`test-windows.ps1 -Update` 仍点击正式下载按钮访问实际 GitHub latest，联网失败会使检查失败，不能用 fixture 替代。

## 单一版本源

版本变更只编辑 `build/version.json`，随后明确生成并核验派生文件：

```powershell
python scripts/version_source.py --write
python scripts/version_source.py --check
```

稳定版本为规范 `MAJOR.MINOR.PATCH`，每段为 0 至 65535，Windows 第四段固定 0。MSBuild 导入的 `Version.props` 校验源文件和模板的真实哈希，以及 `VERSION`、资源版本的相等性。构建不自动修复过期生成文件；`build.ps1 -Version` 和 `package.ps1 -Version` 只能核对同一个版本。

## 原 Windows 五项资产

| 资产 | 用途与必要条件 |
| --- | --- |
| `huhucapture.exe` | 可直接运行；真实 x64 EXE、版本与品牌一致，不超过旧下载器的 64 MiB 上限 |
| `huhucapture-vX.Y.Z-windows-x64.zip` | 同一 EXE、六种语言、依赖许可与 build-info；旧原生产品不含 Qt DLL |
| `huhucapture-vX.Y.Z-windows-x64-symbols.zip` | 同一次 Release 构建的实际 PDB，供故障定位 |
| `build-info.json` | 源 SHA、版本、架构、配置、工具链、依赖锁和 EXE 哈希 |
| `SHA256SUMS.txt` | 其余四项实际资产的 SHA256 |

GitHub 自动生成的源码 ZIP / tar.gz 不属于这五项可运行资产。EXE、PDB 和元数据必须来自同一构建；重新编译同版本不保证符号匹配。Windows 打包检查实际产品资源与 PE 依赖，拒绝额外的 MSVC、Ling、Yoga 或 gifski DLL。

## CI 与独立桥接发布

[Windows workflow](../.github/workflows/windows.yml) 只构建、验证并保留 artifact，push、PR、release 事件以及手工旧标签回归都不上传 Release 资产。main 上用于新发行的 run 必须完整完成 Debug、Release、GUI、实际 latest 下载及桥接检查；标签回归跳过的步骤不能拿来满足新发行准入。

`windows-x64` 保留五项资产，`windows-render-checks` 保留原生、GUI 和 Debug / Release 桥接报告；失败尽量保留 `windows-build-logs`。打包 artifact 在后续 GUI 检查前可能已经上传，能下载 artifact 不代表 run 已全部成功。

唯一新发布入口是 [windows-bridge-release.yml](../.github/workflows/windows-bridge-release.yml)。它只在规范仓库当前 `main` 接受显式 `source_commit` 和成功的同 SHA `windows_run`，使用原 artifact，不重新构建。声明见 [bridge-publish.json](../releases/bridge-publish.json)，必须和唯一版本源、[v0.0.6 说明](../releases/v0.0.6.md) 一致。历史 `releases/publish.json` 不触发新发布。

发布器核验原 Windows 全部门槛、真实下载报告、实际五资产和当前 main，拒绝已有 tag、Release、draft 或第二次桥接。通过后创建固定 tag 与未公开 draft，上传原字节并核对 GitHub 的长度及 SHA256，最后才公开为 latest。完整准入和流程见 [独立桥接发行](bridge-release.md)。

## 失败与恢复

构建或检查失败时修复原因，不跳过门槛。网络或 runner 临时故障可对同一源码重跑；main 已前进则必须使用新的成功 run。禁止移动已存在的 tag 或使用 `--clobber` 替换发行资产。

中断留下的新 tag 或 draft 会使下一次自动尝试拒绝。维护者须核对其 SHA、原 run、已上传字节与声明，再单独处理；脚本不会自行删除、覆盖或认领它。源码修复应使用新的版本，保留旧发行可追溯性。本文描述可执行流程，不声明未运行的检查已经通过。
