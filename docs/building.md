# 构建与发布

## Qt 迁移验证入口

当前正式 Windows 构建仍使用本页后续的 PowerShell / MSBuild 入口。根目录 CMake 用于 [Issue #5 的迁移验证](qt-migration.md)，还不生成最终 Qt 应用或安装包。CMake 要求 3.25 以上及 C++20 编译器，纯核心配置不需要 Qt：

```sh
cmake --preset core
cmake --build --preset core --parallel 2
ctest --preset core
```

Qt 验证严格使用 6.12.1 的 Core / Gui / Widgets / Network / Svg。SDK 恢复步骤、固定 aqtinstall 提交和实际平台架构见 [.github/workflows/qt-validation.yml](../.github/workflows/qt-validation.yml)；不能以另一个发行版 Qt 代替。Linux SDK 包含 QtWayland 和匹配的私有头，实际中文字体也是导出验证的前置条件。安装并将该 SDK 的 `bin` 与前缀加入环境后：

CI 通过 aqt 官方支持的 `AQT_CONFIG` 读取 [build/aqt.ini](../build/aqt.ini)，
串行恢复共享安装目录的 SDK 归档，避免 Windows 外部 7-Zip 并行创建同一目录的竞态。
该设置来自固定 aqt 源码的 `Settings.concurrency` / `run_installer`，
不修改下载内容、不关闭 SHA256 校验，也不忽略解压错误。

```sh
cmake --preset qt-validation
cmake --build --preset qt-validation --parallel 2
ctest --preset qt-validation
```

Windows 使用与 Qt 匹配的 MSVC v143、x64、动态 CRT；在对应开发环境执行：

```powershell
cmake -S . -B artifacts/cmake/qt-validation -G "Visual Studio 17 2022" -A x64 -T v143 -DHUHUCAPTURE_QT_VALIDATION=ON
cmake --build artifacts/cmake/qt-validation --config Debug --parallel 2
ctest --test-dir artifacts/cmake/qt-validation -C Debug --output-on-failure --no-tests=error
cmake --build artifacts/cmake/qt-validation --config Release --parallel 2
ctest --test-dir artifacts/cmake/qt-validation -C Release --output-on-failure --no-tests=error
```

Linux Portal 验证还需要 GLib / GIO 2.80 以上、Meson、Ninja、pkg-config、D-Bus 与 Qt 6.12.1 私有开发头。`scripts/build-portal.py` 在空的 `.deps/portal-validation` 中恢复锁定 libportal，检查受控补丁 SHA256，再构建 Qt 后端；不修改系统安装或清空已有源码。已有工作区时应指定另一个空 `--workspace` 并使用它实际产生的前缀。

```sh
python3 scripts/build-portal.py
export PKG_CONFIG_PATH="$PWD/.deps/portal-validation/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake --preset qt-validation -DHUHUCAPTURE_PORTAL_VALIDATION=ON
cmake --build --preset qt-validation --parallel 2
ctest --preset qt-validation
```

真实桌面授权 probe 与自动故障测试的区别见 [Linux Portal](linux-portal.md)。LocalSend 的 Cargo workspace 使用 Rust 1.97.1 和提交的实际锁文件，构建命令见 [core 适配](localsend-core.md)。这些验证入口没有发布权限，也不会创建正式 Release。Qt SDK 资产哈希、聚合 Rust 库、软件编码和最终部署工具仍须按 P00 完成验证，P11 / P12 / P13 通过后才能切换生产发布。

受限开发环境可使用独立的 [开发依赖工作流](../.github/workflows/development-dependencies.yml)
生成 Linux amd64 SDK。它保留固定 Qt Widgets / Wayland client、Portal、PipeWire、
软件媒体依赖、CMake、Ninja、pkg-config、所需系统开发文件及许可证，并生成逐文件
SHA256 与相对符号链接清单。Qt 官方归档中的未使用 QtSql、Wayland compositor 和
QML imports 连同其导入元数据被明确排除，排除清单记录在 `metadata.json`；所有
保留 ELF 的依赖仍须完整。Qt Wayland client、平台插件和匹配的私有头不会因此删除。

恢复使用 `scripts/bundle-development-dependencies.py restore`，必须指定下载产物的
实际 `--sha256` 与一个空 `--destination`，随后加载恢复目录中的 `sdk/environment.sh`。
脚本逐项验证内容、权限、链接和原始前缀替换；CI 还将原依赖前缀改名，再用恢复后的
SDK 实际构建 Portal / PipeWire / 软件编码目标。该产物是开发依赖，不是用户安装包，
构建或恢复成功不能计作产品安装验收。宿主 C++ ABI / glibc 要求保留在元数据中，
不复制或替换宿主加载器。

Rust 聚合库由独立验证工作流保留实际 `.a` / `.lib` 和 `build-info.json`。CMake 会核对
静态库、Cargo.lock 以及全部已记录桥接和上游源文件的 SHA256，拒绝用同一 Cargo.lock
下的旧桥接库代替当前源码。Windows 还核对实际依赖导入库及匹配的动态 Release CRT。

## 现有 Windows 生产构建

本地和 GitHub Actions 使用同一组脚本，避免本地可构建、发布时却采用另一套依赖与参数。默认构建版本为 **v0.0.5**，Windows 文件版本为 **0.0.5.0**。唯一版本输入来自 [build/version.json](../build/version.json)，发布意图来自 [publish.json](../releases/publish.json)。

CMake 从 `build/version.json` 的 schemaVersion 1 / version 字段设置 `PROJECT_VERSION`，
并生成 `app/Version.h`；Qt 应用层通过 `huhucapture_version` 目标使用该头。保留 Windows
入口的 `scripts/build.ps1` 从同一个 JSON 和 `Version.props.in` 生成
`artifacts/generated/Version.props`，资源第四段固定为零。MSBuild 在实际构建前核对
源 JSON 的 SHA256，拒绝陈旧的生成文件和不一致的命令行版本覆盖。直接使用保留
Visual Studio 工程前也先运行该脚本；不手工修改生成的 props。

`build.ps1 -Version`、打包及发布声明只核对版本，不覆盖资源值；不同标签或陈旧 EXE
会明确失败。版本格式为无前导零的 `MAJOR.MINOR.PATCH`，每段不超过 65535，以匹配
Windows 资源格式。修改 JSON 会触发 CMake 重新配置。迁移分支保持当前正式版本号，
只有发布门槛满足后才执行规定的 Patch 递增。

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
pwsh ./scripts/test-windows.ps1 -Update
pwsh ./scripts/package.ps1
```

Release EXE 位于 `artifacts/bin/x64/Release/huhucapture.exe`。首次依赖恢复后，可以打开 `huhucapture.slnx` 调试。改动构建参数或依赖时还执行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore
```

生成物集中在 `artifacts`，依赖恢复到 `.deps`，两者均不提交。测试的能力边界、证据和物理设备验收见 [验证指南](validation.md)。

`test-windows.ps1 -Update` 会访问真实 GitHub 稳定 Release，并通过正式关于页将 EXE 下载到系统 Downloads；测试只清理本次拥有的下载。该模式使用独立的 90 秒测试进程超时，常规 `-Gui` 保持 45 秒。联网失败会使更新下载检查失败，不能用既有 artifact 替代这项门槛。运行时的文件与失败规则见 [更新下载](updates.md)。

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

新版本声明发布要求 Release / Debug、核心与脚本检查、原生渲染、单 EXE 启动、真实窗口/剪贴板、真实更新下载和打包门槛全部通过。Debug 与真实更新下载在常规 main、PR 和不填标签的手工构建中执行；Release 事件及填写标签的恢复构建不执行这两项，避免待上传资产的版本成为 latest 后，下载门槛反过来阻止它上传首批资产。打包核对版本与产品资源，检查 PE 依赖，拒绝意外增加外部 MSVC、Ling、Yoga 或 gifski DLL。

`windows-x64` 保留 EXE、便携包、符号、构建信息和校验和；`windows-render-checks` 保留原生、GUI 与更新下载报告及图片；失败时尽量保留 `windows-build-logs`。具体保留期以工作流为准。程序 artifact 可能在后续 GUI 检查前已经上传，因此“能下载 artifact”本身不证明完整构建成功。

## 声明新版本

发行所指向的提交必须同时包含相互匹配的：

1. `build/version.json` 中的产品版本；
2. `releases/publish.json` 的稳定 tag 与说明路径；
3. `releases/<tag>.md`、更新记录和当前下载说明。

发布声明示例：

```json
{
  "schemaVersion": 1,
  "tag": "v0.0.5",
  "notes": "releases/v0.0.5.md"
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
