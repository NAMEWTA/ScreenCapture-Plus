# 构建与发布

## 共享核心与 Qt 验证

根目录 CMake 统一构建 Windows 与 Linux 应用。默认配置只构建独立核心，使用 `HUHUCAPTURE_BUILD_APP=ON` 构建完整应用。完整应用预设及服务边界见 [Qt 应用](qt-application.md)，实际安装树和开发包命令见 [打包说明](qt-packaging.md)。CMake 要求 3.25 以上及 C++20 编译器，纯核心配置不需要 Qt：

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

真实桌面授权 probe 与自动故障测试的区别见 [Linux Portal](linux-portal.md)。LocalSend 的 Cargo workspace 使用 Rust 1.97.1 和提交的实际锁文件，构建命令见 [core 适配](localsend-core.md)。这些验证入口没有发布权限，也不会创建正式 Release。完整产品构建按以下生产入口验证。

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

## 生产构建

还原 [依赖锁](../build/dependencies.lock.json) 所列输入后，使用本平台的完整预设：

| 平台 | 本地入口 |
| --- | --- |
| Windows x64 | `cmake --preset qt-application-windows`、`cmake --build --preset qt-application-windows`、`ctest --preset qt-application-windows` |
| Linux amd64 | `cmake --preset qt-application-linux`、`cmake --build --preset qt-application-linux`、`ctest --preset qt-application-linux` |

[原生应用工作流](../.github/workflows/qt-application.yml) 定义了两端精确的还原、构建、安装和测试环境。安装树及正式打包步骤见 [Qt 打包](qt-packaging.md)。

## 版本与发行

仅修改 [build/version.json](../build/version.json) 定义版本，再运行 `python scripts/version_source.py --write`。CMake 使用同一来源生成 About、版本头、Windows RC 和包身份。发布时同步 [releases/publish.json](../releases/publish.json) 与对应标签说明。

在当前 `main` 上显式调度 `qt-application.yml`，设置 `release_candidate=true`，取得双平台完整包、同源来源归档及聚合制品。用该次成功运行及精确提交调度 `qt-release.yml`。发行工作流复核聚合和已发布的 v0.0.6 更新桥，经新旧下载验证后公开经过校验的资产。标签和草稿失败后的恢复流程见 [Qt 打包](qt-packaging.md)。

[v0.0.6 的源码标签](https://github.com/NAMEWTA/huhucapture/tree/v0.0.6) 保留该版的 Windows 单 EXE 工程、依赖、旧工具链和验证流程。
