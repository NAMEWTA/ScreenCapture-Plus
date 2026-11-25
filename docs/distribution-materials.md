# 发行物的通知与来源材料

`build/package/runtime-notices.json` 是仓库原有许可证和协议版权文本的唯一安装列表。`build/cmake/runtime-notices.cmake` 直接由 CMake 安装原始字节，不依赖 Python 或测试目标。它保留原相对路径，并包含 `LICENSES/` 下的完整许可、FFmpeg 示例的 `*-license.txt` 以及带有原作者声明的 Wayland 协议 XML；文件缺失时配置失败，不能因为文件名没有以 `LICENSE` 开头而漏装。清单也保留历史来源通知，不据此宣称每个上游都链接进当前 Qt 程序。

这份项目通知列表不能覆盖 Qt SDK、Rust 传递依赖或实际部署的系统库。`scripts/collect-runtime-materials.py` 分阶段收集可以核实的材料，始终保留 `correspondingSourceComplete: false`，不生成能通过正式源码门槛的声明。

## Linux 系统库

在实际构建 runner 上，CMake 安装完成后、`package-qt.py` 修改 RPATH / strip 之前运行：

```sh
python3 scripts/collect-runtime-materials.py system \
  --installed artifacts/install/linux \
  --original-prefix "qt=$QT_ROOT_DIR" \
  --original-prefix "portal=$PWD/.deps/portal-validation/install" \
  --original-prefix "pipewire=$PWD/.deps/pipewire-validation/install" \
  --original-prefix "media=$PWD/.deps/media-validation/install" \
  --output artifacts/runtime-materials-linux
```

输入必须是同次真实构建的四个原始前缀，输出必须是独立空目录。收集器先调用现有安装树验证，核对实际 ELF 应用与嵌入身份，再检查 `lib/` 中的真实 ELF 文件。对非系统前缀，只记录完全相同字节的输入路径，不把该匹配当作已收集相应上游源码。

系统库通过真实 SONAME 在 `ldconfig` 缓存中定位候选，要求只有一个完整大小/SHA256 匹配，随后复用开发 SDK 收集器的 `owner()` 调用实际 `dpkg-query`。记录 binary package、实际版本、架构、source package 与 source version；保存该包原 `copyright` 文件及 runner 的 common-licenses。只匹配文件名、猜包名、借另一台系统的包数据库或用最新版本代替都不满足这个检查。

成功输出的 `runtime-materials.json` 将每个已检查路径绑定到原输入字节与实际包，`notices/system/` 含未改写的通知。无法归属的路径写入 `failures`，命令失败，原应用安装树和原依赖不修改。成功也不意味着正式发行就绪：`systemAttributionComplete` 仅指本阶段系统库归属检查，所有 source package 档案的状态仍为 `not-collected`。

收集成功后，CI 将整个输出目录复制到待打包树的 `share/huhucapture/licenses/collected-runtime/`，随后正常打包，使该次真实记录本身进入最终文件清单。复制只针对本次生成的安装树；不要对用户已安装的目录执行这个工程步骤。正式打包原有对应源码门槛仍须独立通过。

Qt 平台插件的公开部署流程可能改变 RPATH，系统库步骤将 `plugins/` 中实际文件的哈希记入 `deferredPlugins`。这些文件交给下面的 Qt 步骤核实；目录名本身不是来源证明。ICU 等 SDK 随附库不能套用发行版同名包的许可证材料。

## Qt SDK、部署重放与模块源码

同一 runner、同一 CMake 构建目录，在安装后、打包变换前执行：

```sh
python3 scripts/collect-runtime-materials.py qt \
  --installed artifacts/install/linux \
  --qt-prefix "$QT_ROOT_DIR" \
  --build-directory artifacts/cmake/application-runtime \
  --configuration RelWithDebInfo \
  --output artifacts/runtime-materials-qt
```

Windows 使用实际安装目录、Visual Studio 构建目录和该次应用的实际配置；当前完整产品 preset 同样使用 `--configuration RelWithDebInfo`。收集器会选择对应配置的 Qt 生成脚本，并核对 `qwindows` / Schannel 所属的实际原始 SDK。它不使用另一平台的 SBOM，也不把 SDK 的 Release DLL 误当作应用配置。

`build/package/qt_materials.py` 读取 SDK 原 `*.spdx.json`，对实际部署的 Qt 库、插件、编译翻译逐一核对其原输入 SHA1 和完整 SHA256。SPDX 原声明、包关系、作者、许可表达式及完整原文继续保留；不会将所有 Qt 文件统一宣称为单一许可。平台库和插件还必须与一次真实官方部署重放的完整字节相符，翻译目录因部署命令使用 `NO_TRANSLATIONS`，按原始官方 QM 文件校验。

重放仅接受本项目当前生成的公开 `qt_deploy_runtime_dependencies` 调用及相对目的目录。收集器不执行生成目录里的任意 include、可选插件脚本或 hooks；它只读取生成 support 中的字面量设置，核对原 SDK / 工具路径，然后调用同一 SDK 的 `Qt6CoreDeploySupport.cmake`。CMake 与 `windeployqt` 负责所有实际变换，没有自行编写 ELF / PE 归一化。`CMAKE_INSTALL_PREFIX` 和 `QT_DEPLOY_PREFIX` 均指向收集器独占的临时目录，子进程移除 `DESTDIR`，不会以安装脚本向用户目录或原安装树写入。记录实际脚本、工具哈希、stdout/stderr 和输出匹配，并复核原安装清单及原构建/SDK 输入未变。部署选项变化时必须重新审查，不能扩大接受范围来跳过失败。

`qt-materials.json` 的 `binaryBindingComplete` 只表示这些实际 Qt 文件已完成原始 SBOM 和部署字节绑定。`sourceModulesNotCollected` 说明尚无完整源码的模块。需要收集源码与许可全文时，在独立目录取得固定、干净的原 Git checkout，再重复传入，例如：

```sh
python3 scripts/collect-runtime-materials.py qt \
  --installed artifacts/install/linux \
  --qt-prefix "$QT_ROOT_DIR" \
  --build-directory artifacts/cmake/application-runtime \
  --configuration RelWithDebInfo \
  --output artifacts/runtime-materials-qt \
  --module-source qtbase=artifacts/source-inputs/qtbase \
  --module-source qtsvg=artifacts/source-inputs/qtsvg \
  --module-source qtwayland=artifacts/source-inputs/qtwayland \
  --module-source qttranslations=artifacts/source-inputs/qttranslations \
  --source-output artifacts/corresponding-sources-qt
```

两个命令是同一收集入口的不同选择，不能向同一已有输出目录执行第二次。源码使用 Git 自己的 `archive`，保留整棵已跟踪源码、构建文件与第三方声明；未收集的 Git submodule、脏树或与 SPDX 不同的 commit 均拒绝。运行包只需要 `notices/` 和 `qt-materials.json`，完整模块档案及 `qt-sources.json` 作为独立源码产物保存。

完整应用 CI 先执行 `python scripts/restore-qt-module-sources.py --platform linux --workspace artifacts/qt-module-sources`，Windows 将平台参数改为 `windows`。
该入口复用既有依赖恢复方式，只接受新空目录，按主依赖锁读取固定官方仓库和提交，
实际 Git fetch、detached checkout、核对 HEAD 与干净状态。qtbase 直接复用原有
`qtbaseRepository` / `qtbaseCommit`，没有第二份版本来源；Windows 取得三个模块，
Linux 另取得 qtwayland。翻译模块还获取官方真实 release tag 并核对其指向，
不会本地制造 tag。恢复成功只证明这些准确源码已取得，仍由后续收集器核对 SDK
关联并归档；它不会消除下述 Qt translations 与 ICU 缺口。

当前固定 Linux 6.12.1 SDK 的 qtbase / qtsvg `downloadLocation` 分别含 commit `5af1a37e31a869cd6eb8a73d82e2c71a450e6634` / `ce2d9787bf7c6e4d9ab79b33b99ebbcfec7aef00`。qtwayland 的 source SPDX `.tag` 哈希可绑定 `966a42a19fcde2ff0cee3e0cd692237eef9bfb3d`。收集器从实际 SDK 核对这些条件，不把这份 Linux 观察当作 Windows SDK 的来源。

qttranslations 只有版本元数据、缺少原 source SPDX 时，即使收集了 `v6.12.1` 对应源码，也明确记为 `version-tag-only-not-bound-to-sdk`。其当前上游 tag 对应 `201bdab42f65751f1392b38a5f64ecccc9a0657d`，仍需精确来源或官方编译重现证明，不能仅凭同版本声明已绑定。ICU 和原 SDK 配置的完整对应来源同样保留未完成状态。

## Rust 真实依赖图、vendor 与通知

在构建聚合静态库的同一个 runner 上执行：

```sh
python3 scripts/collect-runtime-materials.py rust \
  --native-receipt artifacts/native-rust/build-info.json \
  --cargo-metadata artifacts/native-rust/cargo-metadata.json \
  --output artifacts/runtime-materials-rust \
  --source-output artifacts/corresponding-sources-rust
```

`build/package/rust_materials.py` 复用 `build-native-rust.py` 的完整源码指纹规则，核对实际静态库、Cargo.lock、原 receipt，并使用锁定工具链重新取得真实 Cargo metadata。比较包含依赖边、features、目标条件和原许可表达式的完整解析图，允许 runner 根路径不同；原 metadata 和 receipt 字节不改写。确需恢复相同静态库时用显式 `--native-library` 指定路径，其完整哈希仍必须匹配原 receipt。

源码由真正的 `cargo vendor --locked --versioned-dirs` 产生。收集器不重新解析依赖版本，也不生成或修正 Cargo 校验数据：每个 registry package checksum 必须等于原锁，Git 来源必须带固定 commit，每个 vendored 文件必须与 Cargo 自己的 `.cargo-checksum.json` 一致，缺少或额外文件都拒绝。保留官方生成的 `.cargo/config.toml`、完整 vendor 树和原生 workspace 源码。Cargo metadata / vendor 同时包含构建与目标平台依赖；报告不把它们冒充为精确链接符号列表。

原始 `LICENSE`、`COPYING`、`COPYRIGHT`、`NOTICE`、`UNLICENSE` 和 crate 声明的许可文件复制到 `notices/rust/`；SPDX 表达式、作者、源地址和版本保留在 `rust-materials.json`。找不到原许可全文的 crate 列入 `missingCrateNoticeTexts`，不能根据一个表达式伪造作者文本。实际 Rust 工具链提供的版权文本另行保留；Cargo vendor 不包含 Rust 标准库完整源码，报告中的这项状态仍为 `not-collected`。

`vendorSourceComplete` 表示原 receipt/解析图所需的 vendored crates 和 workspace 已确实收集并核验；`licenseTextCoverageComplete` 还要求原 crate / 工具链通知齐全。二者均不能替代组合发行的许可与完整对应源码门槛。普通运行包只加入本次 `notices/` 和 `rust-materials.json`；完整 vendor、源码和命令日志保存在独立 CI 产物中。

## 项目通知的独立留存

需要在构建证据中单独保留原文时可以运行：

```sh
python3 scripts/collect-runtime-materials.py notices \
  --output artifacts/project-notices
```

这个入口和 CMake 消费同一份列表，并逐一验证复制前后原文件与输出哈希。它不会覆盖旧结果，也不改变项目许可证、版本、依赖锁或正式发布声明。

## 验证和未完成的材料

`python3 tests/runtime_materials_tests.py --cargo-metadata artifacts/native-rust/cargo-metadata.json` 在原构建 runner 验证原始通知复制、真正的纯 CMake 通知安装、真实 Qt 平台/TLS/QM 的 SPDX 字节与损坏拒绝，以及实际 Cargo 图的缺项/许可错配拒绝。Qt SDK 取 `QT_ROOT_DIR` 或显式 `--qt-prefix`；没有对应真实输入会打印未执行范围。Linux 另验真实系统 ELF、字节改变拒绝、实际 dpkg source/version。Windows 会明确跳过 Linux 专属项。没有包数据库的本地容器只能显式使用 `--without-dpkg` 运行有限检查；此选项不用于 CI 的完整门槛。完整 Qt 重放、Cargo vendor、源码归档仍由上述真实收集入口分别验收。

完整材料仍需要：同提交项目源码与构建脚本；所有 Qt 模块/ICU 的精确对应来源、许可全文及原 SDK 配置；本次 CI 真正完成的 Cargo vendor 和缺失通知处理；Rust 标准库源码；libportal/PipeWire/FFmpeg/x264 的精确源码、补丁和真实配置；实际系统 source package 档案；AppImage runtime 的已锁来源。这些材料的集合必须覆盖实际 payload，不能仅凭几个组件名字或一个 `complete: true` 字段确认。

官方工具依据：[dpkg-query](https://manpages.debian.org/bookworm/dpkg/dpkg-query.1.en.html)、[Qt SBOM](https://doc.qt.io/qt-6/sbom.html)、[Qt 6.12.1 deployment 实现](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/Qt6CoreDeploySupport.cmake)、[Cargo metadata](https://doc.rust-lang.org/cargo/commands/cargo-metadata.html)、[Cargo vendor](https://doc.rust-lang.org/cargo/commands/cargo-vendor.html) 及其 [0.98.0 源实现](https://github.com/rust-lang/cargo/blob/0.98.0/src/cargo/ops/vendor.rs)。源码材料尚未全部收集及验证，开发包不代表组合发行许可审核通过。
