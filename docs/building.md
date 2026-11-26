# 构建与发布

## Qt 迁移验证入口

当前正式 Windows 构建仍使用本页后续的 PowerShell / MSBuild 入口。根目录 CMake 默认构建 [Issue #5 的迁移验证](qt-migration.md)；显式开启 `HUHUCAPTURE_BUILD_APP=ON` 可构建统一 Qt 应用。完整应用预设及服务边界见 [Qt 应用](qt-application.md)，实际安装树和开发包命令见 [打包说明](qt-packaging.md)。这些入口仍受 P12 发布验收约束。CMake 要求 3.25 以上及 C++20 编译器，纯核心配置不需要 Qt：

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

本地和 GitHub Actions 使用同一组脚本，避免本地可构建、发布时却采用另一套依赖与参数。默认构建版本为 **v0.0.6**，Windows 文件版本为 **0.0.6.0**，唯一手写版本来源是 [build/version.json](../build/version.json)。旧 Windows 桥接的发布意图由 [bridge-publish.json](../releases/bridge-publish.json) 单独声明；正式 Qt 发布使用 [publish.json](../releases/publish.json)，其中仍保留的 v0.0.5 声明不是本次桥接输入。源码版本和声明存在不表示发行已经公开。

CMake 使用原生 JSON 解析从同一个文件设置 `PROJECT_VERSION`，生成 `app/Version.h`、Qt Windows RC、About 版本和包身份；运行时不会读取版本 JSON 或启动 Python。根目录 `VERSION` 和保留 Windows 工程的 `build/windows/Version.props` 是兼容生成文件，不再独立维护。修改 JSON 后运行 `python scripts/version_source.py --write`，普通检查使用 `python scripts/version_source.py --check`；CMake、PowerShell 构建/打包脚本和直接 Visual Studio/MSBuild 构建都会拒绝未同步的生成值。MSBuild 在编译前核对 JSON 与模板的实际 SHA256、源版本和资源分量，正常构建不依赖 Python。

资源的第四段固定为零。`build.ps1 -Version`、打包及发布声明只核对版本，不覆盖资源值；不同标签或陈旧 EXE 会明确失败。版本格式为无前导零的 `MAJOR.MINOR.PATCH`，每段不超过 65535，以匹配 Windows 资源格式。修改 JSON 或兼容生成文件会触发 CMake 重新配置。迁移构建使用同一版本源，当前 v0.0.6 对应 P11a 独立桥接；首个完整 Qt 重构版本的目标为 v0.1.0，其版本及正式发布声明仍须在相应发布门槛满足后更新。

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

[Windows 工作流](../.github/workflows/windows.yml) 保留旧生产版本的 Release / Debug、
核心与脚本、原生 GUI、更新下载和五资产验证。Qt 组件、原生 Rust、视频检查和
[完整 Qt 应用](../.github/workflows/qt-application.yml) 则分别验证各自实际能力。
普通 PR、main push 和开发构建产出的 artifact 不能自动成为正式发行。

Qt 正式包使用同一个来源提交、唯一版本源和完整发行清单。安装器、便携 ZIP、
DEB、AppImage、两端符号、构建信息、校验和及对应源码必须齐全。`prepare` 先冻结
最终实际载荷；`finalize` 按载荷计算来源完整性。开发包明确标记为 development，
不能通过修改标志或省略运行库成为正式包。详情见 [Qt 打包](qt-packaging.md)。

P11a 是 Issue #5 明确规定的过渡例外：旧 Windows 更新桥接按现有生产五资产及
受控完整包下载契约独立验证和发行，不等待 Qt/Linux 来源和 P12 完成。桥接发行
只交付旧 Windows 产品及更新迁移能力，不声称交付 Qt 或 Linux。它必须有显式
桥接声明、匹配的源版本与当前 main，以及同 SHA 的实际 Windows 验证证据。

新 Qt 版本由 [完整发布工作流](../.github/workflows/qt-release.yml) 显式执行。
入口先核对当前 main、同 SHA 的必要成功流水线和真正的正式 aggregate，再通过
实际旧 About 下载该完整包。所有检查通过后创建新 tag 和未公开草稿，逐一上传
并核对远端资产的长度和 SHA-256，最后才公开为正式版本。既有 tag 或发行资产
不会被强制覆盖。P11a 桥接使用其独立声明入口，不把两种产品的资产混装。

## 声明新版本

发行提交须同时包含匹配的 `build/version.json`、生成的 `VERSION` 与
`build/windows/Version.props`、对应发布声明、版本说明和更新记录。稳定标签为
`vMAJOR.MINOR.PATCH`，Windows 第四段固定为 0；构建参数仅核对版本，不重新标记
另一份二进制。下一完整重构目标是 **v0.1.0**，当前 v0.0.5 声明不会被新入口自动
重新发布。P11a 桥接采用下一未占用补丁版本，仍明确标记其过渡范围。

正式 Qt 发布采用 `releases/publish.json` 中的 tag 和说明路径；桥接使用单独的
显式 bridge 声明。工作流验证当前 main 没有前进，不将旧运行的资产发布成新源码。
发布串行进行，创建发行的 token 不依赖额外触发事件补齐资产。

## 失败与恢复

编译、测试、来源收集或安装旅程失败时修复实际原因后重新验证。没有执行的
物理桌面、音源、IME、长时录制或双端互传验收保持未验证，不通过关闭 Issue
代替验收。Runner/网络临时故障可重跑同一源码，不必因此更改产品版本。

已存在未公开 tag 或草稿时，先核对其 SHA、对应成功构建与全部资产。发布器拒绝
移动引用或覆盖旧资产；修复后必须保持程序、符号、构建信息、来源及校验和是
同一次构建。公开版本中的源码缺陷用新版本修复，保留旧发行的可追溯性。

本文定义可执行流程，不表示某个候选已通过全部门槛。实际状态以
[Actions](https://github.com/NAMEWTA/huhucapture/actions) 和
[Releases](https://github.com/NAMEWTA/huhucapture/releases) 为准。
