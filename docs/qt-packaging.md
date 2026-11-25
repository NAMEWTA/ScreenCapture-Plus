# Qt 完整应用的安装树与打包

本实现对应 [Issue #5 第 13 节](https://github.com/NAMEWTA/huhucapture/issues/5)。统一 `huhucapture` 可执行目标先由 CMake 安装为真实运行目录，再使用 Qt 部署 API、NSIS、CPack/DEB、linuxdeploy 和 appimagetool 生成平台包。脚本不修改版本、创建 tag、上传资产、安装程序或公开 Release。旧 Windows 发行线在迁移门槛通过前继续保留。

`scripts/package-qt.py --help` 列出入口，`build/cmake/qt-install.cmake` 定义唯一安装树。版本的唯一输入是 `build/version.json`；CMake 原生读取它，打包脚本通过 `scripts/version_source.py` 的 `read_version(ROOT)` 读取同一文件。旧 Windows 构建脚本在自己的产物目录生成 `Version.props`，没有第二份受维护的版本文件。Qt 与旧程序共用 `resources/version-info.rc`，打包身份和清单均沿用同一版本。

## 实际构建身份

配置阶段读取 Git HEAD、工作区是否有改动、编译器、Qt 版本、构建配置、主依赖锁及 Cargo.lock 的 SHA256。相同的 JSON 同时写入 `build-identity.json` 和应用可执行文件中保留的只读记录。安装标记独立描述实际安装形态：

```json
{
  "schemaVersion": 1,
  "product": "huhucapture",
  "version": "0.0.5",
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

Windows 使用 `--platform windows`。输出目录必须为空。脚本按 `build/package/dependencies.lock.json` 下载固定官方资产，流式计算 SHA256，核对精确字节数，并保留工具的固定提交源码、许可证及来源清单。Windows 下载固定 NSIS 3.11 发行文件，不运行该安装器；真正打包时使用 7-Zip 解开该已验证文件，直接调用其中唯一的 `makensis.exe`。不会从开发机 PATH 中随便找一个 NSIS 编译器。

Linux 使用四项独立锁定的工具：linuxdeploy、它的 Qt 插件、appimagetool 和 type-2 runtime。appimagetool 总是收到 `--runtime-file`，不能临时下载一个浮动 latest runtime。固定官方 asset ID 对应的实际二进制、源码和许可字节已经分别核对；锁文件保存这些 SHA256 和长度，不接受 bootstrap-only 工具。复用来源与最初取得这些字节的独立开发工作见 [准确归属记录](../third_party/project-reuse/qt-packaging-upstream.json)。恢复脚本不安装或发布产品。

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

`HUHUCAPTURE_PACKAGE_KIND` 的 Windows 默认值为 `portable-zip`，Linux 默认值为 `appimage`。打包器在自己创建的新目录内，分别写入 installer/portable-zip 或 deb/appimage 的准确标记，不修改原安装树。应用仅核对自己可执行文件所在安装树的标记来选择更新包；当前目录、共享数据目录或 `APPIMAGE` 环境值都不能替代这个标记。AppImage 外层路径另用于自启，属于不同职责。

Qt 的公开 `qt_generate_deploy_script` / `qt_deploy_runtime_dependencies` 选择匹配 SDK 的运行库、平台插件、图片插件、TLS 后端和翻译。Qt 6.12 的 `QTP0008` 使用 `NEW`，生成脚本文本不经额外宏转义；Windows 的 `qt.conf` 明确放在根可执行文件旁。Windows 另外部署直接导入的 FFmpeg DLL，以及 CMake 从匹配 MSVC REDIST 集找到的 release app-local CRT，拒绝 Debug CRT 和任意 PATH DLL。

Linux 应用设 `$ORIGIN/../lib` 的安装 RPATH。明确复制 PipeWire 客户端动态加载的 protocol-native、client-node、adapter 模块和必要 SPA 支持/音视频转换插件，以及 `client.conf`。AppImage 的 `AppRun` 和 DEB 的 `/usr/bin/huhucapture` 启动器设置各自私有的 PipeWire/SPA/config 目录。不会随包启动或携带 PipeWire daemon、WirePlumber、桌面 compositor、glibc loader 或 GPU 驱动。

Linux 安装还必须配置 `HUHUCAPTURE_OPENSSL_RUNTIME_DIR`，指向已恢复 OpenSSL 3 依赖的 `libssl.so.3` / `libcrypto.so.3` 所在目录，开发 SDK 的对应目录是 `system/usr/lib/x86_64-linux-gnu`。Qt OpenSSL 后端用 `dlopen`，仅遍历 ELF 的 `DT_NEEDED` 无法发现这两个依赖，不能因为 `tls/` 中有插件就假定 HTTPS 可用。安装时将真实 ELF 字节以 SONAME 写入私有 `lib/`，不保留 SDK 的绝对符号链接；最终审计要求实际 OpenSSL 后端和这两个库齐全。Windows 使用原生 Schannel 后端。可信根证书仍由宿主提供，DEB 明确依赖 `ca-certificates`。

### 运行时许可输入的目录布局

`HUHUCAPTURE_RUNTIME_LICENSES` 指向从真实依赖构建或 SDK 恢复的许可目录。安装过程原样保留这些文件，不把根 MIT 许可扩展解释为所有依赖的许可。

| 平台 | 配置输入 | 安装位置 |
| --- | --- | --- |
| Linux | 已恢复 SDK 的 `licenses`，包含 `ffmpeg/`、`portal/`、`pipewire/`、`x264/`、`system/*.copyright`、`project/` | `share/huhucapture/licenses/runtime/` |
| Windows | FFmpeg 对应配置 prefix 的 `licenses`，至少有 `ffmpeg/LICENSE.md` 和 `ffmpeg/COPYING.LGPLv2.1`；另将已取得的 Qt 许可原文保留为 `qt/` | `licenses/runtime/` |

仓库自身 `LICENSE`、`third_party/NOTICE.md`、随源码保存的第三方许可和主依赖锁另行安装。若匹配 Qt SDK 带 `sbom/`，同时安装为 `qt-sbom/`。SBOM 便于关联实际 SDK，不能代替许可原文、对应源码或可重建资料。开发打包检查运行目录包含许可材料；正式发行仍须审核整套实际组合，尤其是 Linux GPL 配置的 FFmpeg/x264、AGPL 的 gifski、完整 Rust vendor 依赖和 Qt 自带第三方库。

## 生成实际平台包

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

同路径升级先调用已登记的旧用户级卸载器，移除旧版明确列出的程序文件，从而退休过期 DLL 和插件。卸载器只删除本次安装清单中的文件，不递归移除用户的 Documents、媒体、配置或任意自选目录。原生 Windows 还必须验证交互安装、运行中提示、重复安装、升级、静默卸载、中文/空格路径和重新安装；脚本生成 installer 成功不等于这些交互场景已通过。

### Linux DEB 与 AppImage

linuxdeploy 使用固定 Qt 插件、匹配的 `qmake`、实际 Wayland 插件并保留调试数据，随后再次审计树。AppImage 带自己的 `AppRun` 和桌面文件。DEB 把同一运行目录放在 `/opt/huhucapture`，在 `/usr/bin` 安装薄启动器，并安装 desktop/icon。CPack 的 `dpkg-shlibdeps` 根据实际 ELF 与私有库目录计算系统依赖，同时声明 Portal 运行服务需求；没有将整台构建机的库目录复制进包。

调试分离由 GNU `objcopy` 执行，保存每个实际处理文件的 ELF build-id、运行文件 SHA256 和 debug 文件 SHA256，并设置 `.gnu_debuglink`。DEB 用 `dpkg-deb` 实际解包，AppImage 用自身的 `--appimage-extract` 实际解包；解包结果必须与待打包树的完整清单相等。AppImage 工具支持 `APPIMAGE_EXTRACT_AND_RUN=1`，构建环境无需 FUSE；用户也可以执行 `./huhucapture-vX.Y.Z-linux-amd64.AppImage --appimage-extract` 后运行 `squashfs-root/AppRun`。

所有输出目录必须为空，已存在结果不会被覆盖。中途失败保留已经写出的输出和日志，成功清理的只是在任务中创建的临时打包树。脚本不执行生成的应用或安装器。

## 正式聚合与对应源码

正式包去掉 `--development`，必须来自干净且匹配的源提交，并提供 `--source-bundle` 与 `--source-manifest`。源清单必须指向实际档案的名称、字节数、SHA256，标明同产品/仓库/版本/提交、`complete: true` 和所含组件。至少包括 huhucapture、Qt、gifski、LocalSend、FFmpeg、Rust vendor；Linux 还必须包括 libportal、PipeWire、x264 和 AppImage runtime。需要保存匹配的补丁、构建参数、许可证及重建方式；不可以只附一个 upstream URL 就宣称已经准备对应源码。

当前脚本验证档案字节与源清单绑定、身份和必要组件声明。源档案的依赖完备性及组合发行义务仍须独立审核，不能从 `complete` 字段推导已完成审核。现阶段没有提供伪造的“已批准”源材料；缺少真实源档案会阻止非开发模式打包。完整对应源码生成与审核是正式发行门槛，当前开发包不是正式分发许可结论。

两个平台的正式包生成后，使用同一脚本聚合：

```sh
python3 scripts/package-qt.py aggregate \
  --input artifacts/packages/windows \
  --input artifacts/packages/linux \
  --output artifacts/release
```

聚合必须同时有 installer、portable-zip、DEB 和 AppImage 四项实际资产，源提交/版本一致，平台 build-info 未改变，所有引用的包、符号及源材料的长度和 SHA256 均匹配。聚合只复制已验证文件，并从它们生成 `release-manifest.json` 和统一 `SHA256SUMS.txt`；不会两端各自覆盖一个公共清单。更新器消费的唯一 schema 见 [qt-updates.md](qt-updates.md)。

## 已有自动化和仍需完成的验收

`python3 tests/package_metadata_tests.py` 使用真实文件、真实 ZIP、真实编译器生成的 ELF 元数据探针，验证改动检测、越界符号链接、资产身份、传输头、排他写入、嵌入身份与旁置记录不一致，以及脏源码的正式模式拒绝。这些测试不伪造可安装产品，也不把 metadata probe 计为应用安装证据。

安装目录的 TLS 实测使用单独编译的 `package_runtime_probe`，然后运行：

```sh
python3 scripts/test-qt-package-runtime.py \
  --prefix artifacts/install/runtime \
  --probe artifacts/cmake/application/package_runtime_probe \
  --certificate tests/fixtures/qt-tls/localhost-test-cert.pem \
  --private-key tests/fixtures/qt-tls/localhost-test-key.pem \
  --report artifacts/package-tls-runtime.json
```

Windows 把 `--probe` 指向对应配置目录的 `.exe`。辅助脚本清除开发用 Qt/动态库搜索环境，在可写的实际安装目录中排他放置一个临时探针，以验证 Windows 的 app-local CRT 搜索行为；结束后只删除这个身份，产品文件和 `qt.conf` 保持原样。探针核对实际加载的 Qt 库、TLS 插件及 Linux OpenSSL / Windows CRT 的路径都在该安装树中，报告运行版本、文件 SHA256 和大小。它创建真正的本地 TLS 连接，要求正常证书验证拒绝公开测试自签证书；不会导入信任根、忽略错误或访问外部网络。缺少 OpenSSL 时若偶然回退到构建机 `/usr/lib`，即使 TLS 握手能够进行，这个测试仍会失败。该探针不随产品打包，也不代替桌面图形安装验收。

完整发布前需要另外记录：从本次真实完整包运行的新下载器本地 HTTP 测试、同 SHA 双平台构建测试、干净 Windows 的安装/升级/卸载及 CRT 独立性、Ubuntu/Kubuntu 实际安装和 Portal/Wayland/X11 用户旅程、包内所有动态依赖和许可对应源码审核。Qt 首版进入 latest 后再跑真实 GitHub latest 下载烟测。原有单 EXE 更新器的桥接发行与跳过桥接的手动迁移说明依旧是独立门槛，不能由本文件或一次打包成功代替。

`qt-application.yml` 是唯一的完整 Qt 应用与开发包流水线，两个平台都消费同一检出和固定依赖。`qt-validation.yml` 保留 Debug / Release 的组件与实际 Portal / Wayland 回归；原 Windows 工作流在 P12 前仍承担旧产品回归。测试目标集中在 `build/cmake/tests.cmake`，关闭 `BUILD_TESTING` 后不会寻找 Python、Xvfb 或 D-Bus 测试启动器，仍完整构建、安装同一个产品目标。任何独立检查失败都会保留失败状态，即使后续继续收集安装树证据；开发包没有正式发布权限。

本地已实际完成无测试配置的 RelWithDebInfo 构建、CMake 安装和清除开发搜索路径后的包内 Qt/OpenSSL TLS 拒绝检查。元数据测试也验证了真实文件、ZIP 与编译生成的 ELF 身份。这些结果证明相应构建与部署边界；X11/Wayland 桌面运行、完整 DEB/AppImage、Windows 安装/升级/卸载、实际设备和对应源码仍须分别记录实际验收结果。

## 官方实现依据

- [Qt CMake 部署运行依赖](https://doc.qt.io/qt-6/qt-deploy-runtime-dependencies.html)
- [Qt Windows 部署](https://doc.qt.io/qt-6/windows-deployment.html)
- [Qt QTP0008 转义策略](https://doc.qt.io/qt-6/qt-cmake-policy-qtp0008.html)
- [CMake CPack DEB](https://cmake.org/cmake/help/latest/cpack_gen/deb.html)
- [NSIS 编译命令](https://nsis.sourceforge.io/Docs/Chapter3.html)
- [NSIS 指令与用户上下文](https://nsis.sourceforge.io/Docs/Chapter4.html)
- [固定 linuxdeploy Qt 插件源码](https://github.com/linuxdeploy/linuxdeploy-plugin-qt/tree/ce5291e25979d7d6417551d787f308b88c1b8d76)
- [固定 appimagetool 源码](https://github.com/AppImage/appimagetool/tree/8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81)
