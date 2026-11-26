# Qt 完整应用的安装树与打包

本实现对应 [Issue #5 第 13 节](https://github.com/NAMEWTA/huhucapture/issues/5)。统一 `huhucapture` 可执行目标先由 CMake 安装为真实运行目录，再使用 Qt 部署 API、NSIS、CPack/DEB、linuxdeploy 和 appimagetool 生成平台包。打包脚本不修改版本、创建 tag、上传资产、安装程序或公开 Release。旧 Windows 构建与桥接回归继续保留；公开发行统一经过本文的显式完整包入口。

`scripts/package-qt.py --help` 列出入口，`build/cmake/qt-install.cmake` 定义唯一安装树。版本的唯一输入是 `build/version.json`；CMake 原生读取它，打包脚本通过 `scripts/version_source.py` 的 `read_version(ROOT)` 读取并核对生成文件。`VERSION` 与 Windows `Version.props` 是受校验的生成输出，不能独立改动。打包身份、Windows RC 和清单均沿用同一版本。

## 实际构建身份

配置阶段读取 Git HEAD、工作区是否有改动、编译器、Qt 版本、构建配置、主依赖锁及 Cargo.lock 的 SHA256。相同的 JSON 同时写入 `build-identity.json` 和应用可执行文件中保留的只读记录。安装标记独立描述实际安装形态：

```json
{
  "schemaVersion": 1,
  "product": "huhucapture",
  "version": "X.Y.Z",
  "platform": "linux",
  "architecture": "amd64",
  "packageKind": "appimage"
}
```

打包器验证真实 PE/ELF 架构、嵌入记录与旁置 JSON 完全一致、当前源提交与版本一致、依赖锁没有在配置后变化。只接受 `Release` 或 `RelWithDebInfo`。Linux 调试符号还要求可执行文件确实具有调试信息，因此正常采用 `RelWithDebInfo`。Windows 安装 `Symbols` component 时记录该次链接的 EXE 和 PDB 哈希，符号包必须与实际运行包相符。

`--development` 允许在开发阶段验证完整包，并将 `developmentOnly: true` 和实际 `sourceDirty` 写入平台元数据；它不将脏源码伪装为某次正式提交。正式聚合拒绝开发包或 `sourceDirty: true`。新版更新器在 release-manifest 和 build-info 两处同样拒绝这些标记；正确的包哈希不能绕过这项检查。开发身份只适用于开发构建，不表示已完成干净机器验收。

## 恢复固定的打包工具

```sh
python3 scripts/restore-packaging-tools.py --platform linux --output .deps/packaging-tools
```

Windows 使用 `--platform windows`。输出目录必须为空。脚本按 `build/package/dependencies.lock.json` 下载固定官方资产，流式计算 SHA256，核对精确字节数，并保留工具的固定提交源码、许可证及来源清单。Windows 下载固定 NSIS 3.11 发行文件，不运行该安装器；真正打包时使用 7-Zip 解开已经通过锁定 SHA256 检查的文件。

NSIS 官方 [`v311` 发行脚本](https://github.com/NSIS-Dev/nsis/blob/7359413009afd4f0fff472d841fc2f2cc0e0a5f8/Examples/makensis.nsi) 同时提供根目录 `makensis.exe` 启动器和 `Bin/makensis.exe` 编译器。打包器要求恰好一个包含这两个文件及必需 Include、Stubs、插件、界面和许可文件的完整分发根；多余编译器或不完整布局都会明确失败。它直接调用该根的 `Bin/makensis.exe`，按照同一固定提交中 [`Source/build.cpp`](https://github.com/NSIS-Dev/nsis/blob/7359413009afd4f0fff472d841fc2f2cc0e0a5f8/Source/build.cpp) 的目录解析规则，显式将 `NSISDIR` 绑定到该分发根，并用 `/NOCONFIG` 排除机器或用户配置。实际 `/VERSION` 必须精确返回 `v3.11`。两个候选的相对路径、大小、SHA256 和最终选择同时写入构建日志及包工具记录；不使用 PATH 中的其他 NSIS 编译器。

`tests/package_metadata_tests.py` 在各平台检查双文件布局、缺失 stub、重复分发根、多余编译器和未锁定归档的拒绝边界。在 Windows 已恢复官方归档后，同一测试还实际解包并运行选定编译器的版本入口。目录选择测试不证明安装器已经编译成功；后续真实 NSIS 编译与安装、重装、卸载回归分别承担这些验证。

Linux 使用四项独立锁定的工具：linuxdeploy、它的 Qt 插件、appimagetool 和 type-2 runtime。appimagetool 总是收到 `--runtime-file`，不能临时下载一个浮动 latest runtime。Qt 插件的上游资产原来没有官方 SHA256；专用 `packaging-dependencies.yml` 从固定官方 asset ID 取得实际字节、核对发布身份，并记录了成功作业的产物。当前锁已经保存审核后的实际 SHA256，不再接受 bootstrap-only 工具。该工作流只恢复依赖，不安装或发布产品。

若需要将原始 Qt 翻译输入随构建证据保存，可传 `--qt-translations <matching-sdk>/translations`，并可同时传 `--qt-translations-archive <original-archive>` 和 `--qt-translations-sha256 <reviewed-sha256>`。这些参数保留真正的 `.qm` 或原始档案，不生成替代翻译文件。最终部署依然从实际匹配的 Qt SDK 取文件。

## 原生安装目录

应用构建需启用仓库的统一 Qt 应用选项并使用实际 SDK。CMake 的 `install` 目标在 `BUILD_TESTING=OFF` 时也存在，不依赖测试可执行文件。以下命令仅展示安装和打包阶段；完整配置选项由根构建预设及对应 CI 固定。

```sh
cmake --build artifacts/cmake/application --config RelWithDebInfo
cmake --install artifacts/cmake/application --config RelWithDebInfo \
  --component Runtime --prefix "$PWD/artifacts/install/runtime"
```

Windows 另安装匹配的符号组件：

```powershell
cmake --install artifacts/cmake/application --config RelWithDebInfo `
  --component Symbols --prefix "$PWD/artifacts/install/symbols"
```

`HUHUCAPTURE_PACKAGE_KIND` 的 Windows 默认值为 `portable-zip`，Linux 默认值为 `appimage`。打包器在自己创建的新目录内，分别写入 installer/portable-zip 或 deb/appimage 的准确标记，不修改原安装树。应用只从自身安装树的标记选择更新形态；当前目录、共享数据目录和 `APPIMAGE` 环境变量不能代替这个标记。AppImage 的有效外层路径另供自启使用。

Qt 的公开 `qt_generate_deploy_script` / `qt_deploy_runtime_dependencies` 选择匹配 SDK 的运行库、平台插件、图片插件、TLS 后端和翻译。Qt 6.12 的 `QTP0008` 使用 `NEW`，生成脚本文本不经额外宏转义；Windows 的 `qt.conf` 明确放在根可执行文件旁。Windows 另外部署直接导入的 FFmpeg DLL，以及 CMake 从匹配 MSVC REDIST 集找到的 release app-local CRT，拒绝 Debug CRT 和任意 PATH DLL。

Qt Windows 产品的唯一 RC 还嵌入 `asInvoker`、`PerMonitorV2` 和 `longPathAware` 的进程 manifest；链接不再另外生成默认 manifest。旧 `True/PM` 只声明 PerMonitor v1，不能满足实际物理像素截图边界。运行目录探针从被安装的产品 EXE 资源读取该 manifest，安装 GUI 旅程仍必须真正完成截图拖选和像素核对。

Linux 应用设 `$ORIGIN/../lib` 的安装 RPATH。明确复制 PipeWire 客户端动态加载的 protocol-native、client-node、adapter 模块和必要 SPA 支持/音视频转换插件，以及 `client.conf`。AppImage 的 `AppRun` 和 DEB 的 `/usr/bin/huhucapture` 启动器设置各自私有的 PipeWire/SPA/config 目录。不会随包启动或携带 PipeWire daemon、WirePlumber、桌面 compositor、glibc loader 或 GPU 驱动。

Linux 安装还必须配置 `HUHUCAPTURE_OPENSSL_RUNTIME_DIR`，指向已恢复 OpenSSL 3 依赖的 `libssl.so.3` / `libcrypto.so.3` 所在目录，开发 SDK 的对应目录是 `system/usr/lib/x86_64-linux-gnu`。Qt OpenSSL 后端用 `dlopen`，仅遍历 ELF 的 `DT_NEEDED` 无法发现这两个依赖，不能因为 `tls/` 中有插件就假定 HTTPS 可用。安装时将真实 ELF 字节以 SONAME 写入私有 `lib/`，不保留 SDK 的绝对符号链接；最终审计要求实际 OpenSSL 后端和这两个库齐全。Windows 使用原生 Schannel 后端。可信根证书仍由宿主提供，DEB 明确依赖 `ca-certificates`。

### 运行时许可输入的目录布局

`HUHUCAPTURE_RUNTIME_LICENSES` 指向从真实依赖构建或 SDK 恢复的许可目录。安装过程原样保留这些文件，不把根 MIT 许可扩展解释为所有依赖的许可。

| 平台 | 配置输入 | 安装位置 |
| --- | --- | --- |
| Linux | 已恢复 SDK 的 `licenses`，包含 `ffmpeg/`、`portal/`、`pipewire/`、`x264/`、`system/*.copyright`、`project/` | `share/huhucapture/licenses/runtime/` |
| Windows | FFmpeg 对应配置 prefix 的 `licenses`，至少有 `ffmpeg/LICENSE.md` 和 `ffmpeg/COPYING.LGPLv2.1`；另将已取得的 Qt 许可原文保留为 `qt/` | `licenses/runtime/` |

仓库自身 `LICENSE`、`third_party/NOTICE.md`、随源码保存的第三方许可和主依赖锁另行安装。若匹配 Qt SDK 带 `sbom/`，同时安装为 `qt-sbom/`。SBOM 便于关联实际 SDK，不能代替许可原文、对应源码或可重建资料。开发打包检查运行目录包含许可材料；正式发行仍须审核整套实际组合，尤其是 Linux GPL 配置的 FFmpeg/x264、AGPL 的 gifski、完整 Rust vendor 依赖和 Qt 自带第三方库。

项目通知按 `build/package/runtime-notices.json` 的显式清单安装，保留原路径及文字，覆盖 KDE `LICENSES/`、示例作者通知和协议 XML 中的版权声明。生产安装由 CMake 直接核验该清单，不要求 Python。实际库和 Qt SDK、Rust vendor、Rust 标准库、原生依赖的材料分别见 [发行材料](distribution-materials.md)、[原生来源](native-source-materials.md) 和 [Rust 工具链来源](rust-toolchain-materials.md)。这些材料在原始部署后收集，实际构建命令的收据由构建器在执行时保存；不能为旧二进制倒造一份执行记录。

## 生成实际平台包

以下 `package --development` 是开发便利入口，相当于 `prepare` 后立即以相同开发意图 `finalize`。它保留 `<output>-prepared` 目录，不允许随后删去开发标记当作正式包。正式入口见下一节。

Linux 示例：

```sh
python3 scripts/package-qt.py package \
  --installed artifacts/install/runtime \
  --output artifacts/packages/linux \
  --tools .deps/packaging-tools \
  --qmake "$QT_ROOT_DIR/bin/qmake" \
  --minimum-system "Ubuntu 24.04 / Kubuntu 24.04, amd64" \
  --development
```

Windows 示例：

```powershell
python scripts/package-qt.py package `
  --installed artifacts/install/runtime `
  --output artifacts/packages/windows `
  --symbols artifacts/install/symbols/symbols `
  --nsis-installer .deps/packaging-tools/nsis/nsis-3.11-setup.exe `
  --minimum-system "Windows 10 1809, x64" `
  --development
```

最低系统字段必须以实际验收基线为依据。脚本只记录这个声明，不用一个字符串证明该环境可运行。缺少本机 `cpack`、`dpkg-deb`、`objcopy`、`readelf`、`zstd` 或 Windows `7z` 时会直接失败；没有用同名空文件或伪造资产替代工具。

### Windows 安装与便携包

两个包从同一实际安装目录复制得到，每一份目录独立审计。ZIP 重新打开后校验 CRC、条目集合、长度和每个条目的 SHA256。受控 NSIS 模板使用 `RequestExecutionLevel user`、LocalAppData、HKCU 的安装/卸载登记及当前用户开始菜单。它检查 x64 和 Windows 10 build 17763 下限，安装/卸载前提示关闭正在使用的可执行文件，不杀进程、不自动启动应用。

`File` 的输出参数整体引用为 `"/oname=文件名"`，源路径单独引用；不能写成 `/oname="文件名"`。固定 NSIS parser 对引号所在位置的规则仍适用于中文、空格与 `$`。每个文件复制/删除和目录创建后检查实际错误，`AllowSkipFiles off` 防止静默忽略失败。旧卸载器先逐字节复制到 NSIS 自己的临时目录，再以最后的 `_?=` 参数等待真正卸载完成；详细失败与保留语义见 [安装器边界](windows-installer.md)。

同路径升级先调用已登记的旧用户级卸载器，移除旧版明确列出的程序文件，从而退休过期 DLL 和插件。已有安装登记时，改变目标目录会在任何文件修改前被拒绝；迁移位置需要先卸载，用户媒体与配置继续保留。静默 `/S` 下的更换目录、运行中程序及旧卸载失败均返回非零状态，不显示阻塞提示框。旧位置的卸载器只有仍持有当前 HKCU 安装位置登记时，才能删除共享快捷方式和登记；复制或遗留的卸载器不能清理另一处活动安装的所有权。

模板显式从 64 位 HKCU 视图读取目录，并保留用户 `/D=` 的输入。NSIS 官方说明指出 [InstallDirRegKey 不受 SetRegView 影响](https://nsis.sourceforge.io/Reference/SetRegView)，因此这里使用 `.onInit` 读取；[NSIS 命令行规范](https://nsis.sourceforge.io/Docs/Chapter3.html)要求 `/D=` 为最后一项、路径包含空格时也不加引号。卸载器只删除本次安装清单中的文件，不递归移除用户的 Documents、媒体、配置或任意自选目录。

实际生成的安装器可在全新 Windows CI 账户运行以下有界回归：

```powershell
python scripts/test-windows-installer.py --installer artifacts/packages/windows/huhucapture-vX.Y.Z-windows-x64-setup.exe --evidence artifacts/tests/windows/installer
```

测试首先拒绝既有品牌 HKCU 安装/卸载登记或当前用户开始菜单目录，然后在自己创建的中文/空格路径检查安装、同目录重装、原样保留无关文件与 `config.json`、静默拒绝变更目录、遗留卸载器不影响活动安装、正常卸载仍保留用户文件。它只调用已观察到且仍匹配哈希/登记的本次卸载器作失败清理，不直接删除未知注册表或递归清理目录。每步有实际进程期限，报告保留真实安装器哈希。

交互安装、运行中提示、跨版本升级、重新安装、物理桌面和干净机器依赖仍须分别验证；生成 installer 或这组原生自动化通过，均不能替代那些验收。

### Linux DEB 与 AppImage

linuxdeploy 使用固定 Qt 插件、匹配的 `qmake`、实际 Wayland 插件并保留调试数据，随后再次审计树。AppImage 带自己的 `AppRun` 和桌面文件。DEB 把同一运行目录放在 `/opt/huhucapture`，在 `/usr/bin` 安装薄启动器，并安装 desktop/icon。CPack 的 `dpkg-shlibdeps` 根据实际 ELF 与私有库目录计算系统依赖，同时声明 Portal 运行服务需求；没有将整台构建机的库目录复制进包。

调试分离由 GNU `objcopy` 执行，保存每个实际处理文件的 ELF build-id、运行文件 SHA256 和 debug 文件 SHA256，并设置 `.gnu_debuglink`。DEB 用 `dpkg-deb` 实际解包，AppImage 用自身的 `--appimage-extract` 实际解包；解包结果必须与待打包树的完整清单相等。AppImage 工具支持 `APPIMAGE_EXTRACT_AND_RUN=1`，构建环境无需 FUSE；用户也可以执行 `./huhucapture-vX.Y.Z-linux-amd64.AppImage --appimage-extract` 后运行 `squashfs-root/AppRun`。

所有输出目录必须为空，已存在结果不会被覆盖。中途失败保留已经写出的输出和日志；成功也保留最终 payload、控制文件及变换记录，以便后续来源收集和复核。打包阶段只执行系统打包工具与实际解包检查，不启动产品 GUI 或安装生成的安装器。

## 冻结最终 payload，再收集真实对应源码

正式构建从干净、相同提交开始；`prepare` 时即确定正式意图，不提供 `--development`。以 Linux 为例：

```sh
python3 scripts/package-qt.py prepare \
  --installed artifacts/install/linux \
  --stage artifacts/packages/linux-prepared \
  --tools artifacts/packaging-tools --qmake "$QT_ROOT_DIR/bin/qmake" \
  --minimum-system 'Ubuntu 24.04 amd64'
```

Windows 用同一 `prepare` 入口，另提供实际 `--symbols` 和锁定 `--nsis-installer`。准备阶段完成 linuxdeploy/patchelf、调试分离、每种形态的 `installation.json`、DEB 控制文件、真正压缩包及解包核验，然后保留：

| 保存内容 | 用途 |
| --- | --- |
| `payloads/installer`、`payloads/portable-zip` | 两份真实 Windows payload，各自含准确标记 |
| `payloads/appimage`、`payloads/deb` | 最终 AppDir、DEB payload 与实际 DEBIAN 控制文件 |
| `source-deployment-inventory.json` | 直接复用来源收集器生成的全树 file/symlink 清单及两种产品根 |
| `installed-input-inventory.json` | 打包开始前的原始安装目录清单 |
| `linuxdeploy-output-inventory.json`、`linux-stripped-inventory.json` | Linux 原生部署后与调试分离后的实际中间清单，绑定原命令范围及哈希 |
| `transformations.json`、`executions/` | 本次实际工具、argv、退出码、完整原始输出日志及变换输入/输出 |
| `file-transformations.json` | 实际复制、patchelf、objcopy、安装标记和 desktop 改写的逐文件连续输入/输出；由同源码原脚本及成功执行记录绑定 |
| `prepared.json`、`artifacts/` | 不可换意图的构建身份、payload 哈希及真正包和符号文件 |

Windows 产品根为 `installer` / `portable-zip`。Linux 产品根为 `appimage/usr` / `deb/opt/huhucapture`；两者以外的 AppRun、desktop、启动器和 DEB control 仍属于全树，不能漏出来源映射。最终来源报告的 `deployment.files`、`treeSha256`、`products` 必须精确等于准备阶段的清单。不能只收原始 install prefix，再宣称它覆盖后来被 linuxdeploy 修改的 AppImage。

用 [来源计划装配器](source-bundles.md) 验证实际 producer/material 并在冻结目录以外形成 `source-plan.json`，再执行：

```sh
python3 scripts/package-qt.py finalize \
  --stage artifacts/packages/linux-prepared --output artifacts/packages/linux \
  --source-plan artifacts/source-plans/linux/source-plan.json \
  --source-output artifacts/source-complete/linux
```

该入口调用真实 `collect --require-complete`。也可提供已经按相同最终 stage 收集的 `--source-bundle` / `--source-manifest` 对；不能同时提供两套来源输入。缺少真实 producer 收据、源码、许可或最终文件映射，收集器必须给出明确 gaps 并失败，打包器不会填空 map、修改 `complete` 或为开发包换标签。

### 保留可独立复核的准备目录

在真实准备步骤成功后，运行：

```sh
python3 scripts/retain-package-stage.py \
  --stage artifacts/packages/linux-prepared \
  --output artifacts/package-stage-linux
```

Windows 使用同一入口与自己的 stage/output。该步骤先调用原 `verify_prepared`，
再把原始 JSON、完整 `payloads`、`artifacts`、`executions` 以及 Linux 的两份中间
清单和 `work/debug` 保存到 tar.gz；选用实际源码构建的 runtime 时，还保留
`work/source-runtime` 中原样复制的 runtime 与许可。归档保留文件权限、符号链接与空日志。不保留不被
复核引用的 CPack、再次解包或工具展开副本。随后安全解出到新目录，逐项比对原始
字节、权限和链接，并真实再次调用 `verify_prepared`。只有全部通过才输出
`huhucapture-v<version>-<platform>-<architecture>-prepared-stage.tar.gz` 与
`package-stage-retention.json`。报告绑定归档 SHA256、原 prepared 文件及完整保留
清单；原 development/release intent 和全部 JSON 保持原字节。此保留记录不替代
来源完整性或发布门禁，上游源码、构建收据与静态输入仍按各生产者另外完整保留。
迁移后验证使用保留执行记录中的原准备目录来核对当时的命令参数，并读取当前目录
中的实际复制件与 AppImage 字节，不改写历史路径。源码 runtime 的外部构建目录仍须
在原记录位置完整可用，并通过原 `verify_build`；仅保留包装目录不能替代这一构建证据。

来源档案和清单分别命名为 `huhucapture-vX.Y.Z-<platform>-<architecture>-sources.tar.gz` 与 `...-source-manifest.json`。Windows 为 `windows-x64`，Linux 为 `linux-amd64`，避免两端不同内容共用一个名字。至少覆盖 huhucapture、Qt、gifski、LocalSend、FFmpeg、Rust vendor；Linux 另含 libportal、PipeWire、x264 和 AppImage runtime 及其实际静态闭包。需要保留实际补丁、构建参数、原许可、输出映射和重建方法，URL 不是对应源码。

finalize 核对来源档案原字节、外部清单与档案内实际 `source-inventory.json`、原构建身份和完整最终 deployment，在收集前后再次读取 payload 清单。它只复制已经生成且仍匹配的包/符号字节，再生成平台元数据，不重新压缩、不改 ELF 或标记。`complete: true` 只有在收集器真实计算并闭合所有材料后才有意义；这一计算仍不替代平台运行、安装与组合发行审阅。

## 正式聚合与唯一公开入口

两个平台的正式包生成后，使用同一脚本聚合：

```sh
python3 scripts/package-qt.py aggregate \
  --input artifacts/packages/windows \
  --input artifacts/packages/linux \
  --output artifacts/release
```

聚合必须同时有 installer、portable-zip、DEB 和 AppImage 四项实际资产，源提交/版本一致，平台 build-info 未改变，所有引用的包、符号及源材料的长度和 SHA256 均匹配。聚合只复制已验证文件，并从它们生成 `release-manifest.json` 和统一 `SHA256SUMS.txt`；不会两端各自覆盖一个公共清单。更新器消费的唯一 schema 见 [qt-updates.md](qt-updates.md)。

平台元数据、build-info 和 aggregate 都显式包含 `developmentOnly: false`、`sourceDirty: false`。正式聚合拒绝遗漏、错误类型或非 false 值；最终目录只容纳已声明的包、符号、平台 build-info、各自来源档案/清单和总清单/checksums。

`qt-application.yml` 的普通 PR/push 继续产生明确的开发包；只有 main 上显式 `release_candidate` 调度可以准备正式候选。两个原生 job 必须完成实际安装旅程及 production Qt About 对本平台两个完整包的下载验证，Windows 原始 MP3 还须通过 Linux 解码，然后才能聚合为 `qt-release-candidate-<source SHA>`。真实材料未闭合时，formal job 在 finalize 阻断，不能用普通开发产物补位。

Qt 完整包公开发行只由 `qt-release.yml` 显式调度。它先核对当前 main、相同 SHA 的全部必要 workflow 和成功正式候选；Windows 再用本次原始 aggregate 跑旧 WinRT/关于页的完整包正例。`publish-qt-release.py` 读取匹配的 `releases/publish.json` 与说明，并核对 P11a 独立桥接已经实际发布。它拒绝已有 tag、Release 或 draft，先创建新 tag 和未公开 draft，上传每项原字节并核对 GitHub 返回的名称、长度及 SHA256，最后才公开为 latest。没有 `--clobber`，不会移动 tag 或修复未知 draft；中断留下的 draft/tag 须单独核对处理。

旧 `windows.yml` 只保留构建、桥接、真实 GUI 和已有裸 EXE 下载回归，原两条自动发布路径已移除。Issue 明确的 P11a 例外由独立 `windows-bridge-release.yml` 与受限的 `publish-release.ps1` / `publish-bridge-release.py` 实现：仅显式声明、当前 main、同 SHA 旧 Windows 五资产及真实受控下载/关于页证据可发布，不能发布 Qt/Linux，也不等待本节的完整来源闭合。两条发行 workflow 共用不取消的串行组，见 [桥接发行](bridge-release.md)。P12 物理/最低系统验收和用户迁移说明仍是发行决策的独立门槛。

## 已有自动化和仍需完成的验收

`python3 tests/package_metadata_tests.py` 使用真实文件、真实 ZIP、真实编译器生成的 ELF 元数据探针，验证改动检测、越界符号链接、资产身份、传输头、排他写入、嵌入身份与旁置记录不一致，以及脏源码的正式模式拒绝。这些测试不伪造可安装产品，也不把 metadata probe 计为应用安装证据。

安装目录的 TLS 实测使用单独编译的 `qt_package_runtime_probe`，然后运行：

```sh
python3 scripts/test-qt-package-runtime.py \
  --prefix artifacts/install/runtime \
  --probe artifacts/cmake/application/qt_package_runtime_probe \
  --certificate tests/fixtures/qt-tls/localhost-test-cert.pem \
  --private-key tests/fixtures/qt-tls/localhost-test-key.pem \
  --report artifacts/package-tls-runtime.json
```

Windows 把 `--probe` 指向对应配置目录的 `.exe`。辅助脚本清除开发用 Qt/动态库搜索环境，在可写的实际安装目录中排他放置一个临时探针，以验证 Windows 的 app-local CRT 搜索行为；结束后只删除这个身份，产品文件和 `qt.conf` 保持原样。探针核对实际加载的 Qt 库、TLS 插件及 Linux OpenSSL / Windows CRT 的路径都在该安装树中，报告运行版本、文件 SHA256 和大小。它创建真正的本地 TLS 连接，要求正常证书验证拒绝公开测试自签证书；不会导入信任根、忽略错误或访问外部网络。缺少 OpenSSL 时若偶然回退到构建机 `/usr/lib`，即使 TLS 握手能够进行，这个测试仍会失败。该探针不随产品打包，也不代替桌面图形安装验收。

完整发布前需要另外记录：从本次真实完整包运行的新下载器本地 HTTP 测试、同 SHA 双平台构建测试、干净 Windows 的安装/升级/卸载及 CRT 独立性、Ubuntu/Kubuntu 实际安装和 Portal/Wayland/X11 用户旅程、包内所有动态依赖和许可对应源码审核。Qt 首版进入 latest 后再跑真实 GitHub latest 下载烟测。原有单 EXE 更新器的桥接发行与跳过桥接的手动迁移说明依旧是独立门槛，不能由本文件或一次打包成功代替。

## 官方实现依据

- [Qt CMake 部署运行依赖](https://doc.qt.io/qt-6/qt-deploy-runtime-dependencies.html)
- [Qt Windows 部署](https://doc.qt.io/qt-6/windows-deployment.html)
- [Qt QTP0008 转义策略](https://doc.qt.io/qt-6/qt-cmake-policy-qtp0008.html)
- [CMake CPack DEB](https://cmake.org/cmake/help/latest/cpack_gen/deb.html)
- [NSIS 编译命令](https://nsis.sourceforge.io/Docs/Chapter3.html)
- [NSIS 指令与用户上下文](https://nsis.sourceforge.io/Docs/Chapter4.html)
- [固定 linuxdeploy Qt 插件源码](https://github.com/linuxdeploy/linuxdeploy-plugin-qt/tree/ce5291e25979d7d6417551d787f308b88c1b8d76)
- [固定 appimagetool 源码](https://github.com/AppImage/appimagetool/tree/8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81)
