# 对应源码的离线收集与验证

`scripts/create-source-bundle.py` 为 [Issue #5 第 13 节](https://github.com/NAMEWTA/huhucapture/issues/5) 的对应源码材料提供可重复的收集入口。它读取真实源码、固定依赖锁、Cargo vendor、构建记录和最终部署目录，生成带版本、平台和架构的源码档案及来源清单。它不下载依赖、不执行发布、不修改输入工作树、Git index 或分支，也不把 SDK 二进制或上游网址当成已提供源码。

完整性由实际输入计算。默认允许生成有用的未完成档案，清单中写入 `complete: false` 和逐项 `gaps`；`--require-complete` 在保留这些结果后以退出码 2 结束。身份、校验和、补丁、路径或构建字节互相矛盾时直接失败，退出码为 1，不生成一个看似有效的最终清单。不存在 `--approve`、`--force-complete` 或计划文件中的 `complete` 覆盖字段。

这个检查建立**所记录源码、构建输入及部署文件之间的可核对关系**。最终发行仍须审核实际依赖范围、许可证条件、重建方式与平台安装结果。通用构建记录由产生它的 CI 任务负责如实记录；工具不能从一份 JSON 推断未记录的链接输入，也不能证明某项法律判断。当前各构建器已有的 `build-info.json` 是重要原始材料，但不全部包含源码清单、静态链接范围和最终文件绑定；不能仅重命名这些文件就宣称材料完整。

## 入口及输出

要求 Python 3.11 或更新版本和 Git。常规收集不需要 Qt、编译器或联网。明确提供 `rustVendor.replay` 时执行已安装的固定 Cargo/Rust 工具，用 `--locked --offline` 实际重做 vendor；提供 Linux `debian-source` 时执行 `dpkg-source` 重放原源码包及 Debian 补丁。两者均不编译产品。

```sh
python3 scripts/create-source-bundle.py collect \
  --plan artifacts/source-inputs/collection-plan.json \
  --output artifacts/sources/linux

python3 scripts/create-source-bundle.py inventory \
  --directory artifacts/install/final-appdir \
  --output artifacts/source-inputs/deployment-inventory.json
```

输出目录和清单文件必须不存在。档案命名为 `huhucapture-v<版本>-<平台>-<架构>-sources.tar.gz`，对应清单为 `huhucapture-v<版本>-<平台>-<架构>-source-manifest.json`。平台/架构组合是 `linux-amd64` 或 `windows-x64`，版本来自实际项目；两端材料汇总时不会以同名覆盖不同字节。档案根目录是 `huhucapture-sources/`，包含：

| 路径 | 内容 |
| --- | --- |
| `sources/huhucapture/` | 精确、干净的项目 Git 提交，包括版本源、依赖锁、补丁、脚本、源码、测试及 NOTICE |
| `sources/<组件>/` | 验证后的真实上游源码，已重放匹配的受控补丁 |
| `sources/rust-vendor/` | 与聚合 Cargo.lock 对应的真实目录源码 |
| `materials/` | 原始源码档案、原始 `.crate`、原始构建/部署记录、补丁、来源证据及收集脚本 |
| `cargo-vendor-config.toml` | 验证原配置后生成的相对路径离线替换配置；不会写入原项目工作树 |
| `vendor-replay.json` | 实际 Cargo 离线重做 vendor 的工具版本、源码提交、参数、日志及输出哈希；没有执行时不生成 |
| `notices/rust/crates/` | 与精确 vendor 校验清单绑定的补充通知；标准许可选择另外标明来源，不修改 vendor |
| `source-inventory.json` | 每个源码文件和链接的身份、组件与缺项 |

外部来源清单保留相同数据及实际 `platform`、`architecture`，并绑定实际档案的名称、大小和 SHA256。生成时固定条目顺序、UID/GID、权限及提交时间，gzip 不写当前时间或临时文件名；在相同工具和输入下可重复生成相同字节。完成后实际重新打开档案、解出到新的临时目录并逐文件比较。构建记录中的原始内容属于输入，修改记录或工具版本可能改变档案。

## 收集计划

计划中的相对路径以计划文件所在目录为基准。下面的结构示意包含占位路径，不是可以通过完整性检查的发行材料：

```json
{
  "schemaVersion": 1,
  "platform": "linux",
  "project": "/actual/clean/project",
  "buildIdentity": "/actual/build/build-identity.json",
  "sources": [
    {"id": "qtbase", "directory": "/actual/sources/qtbase"},
    {"id": "ffmpeg", "directory": "/actual/project/.deps/media-validation/ffmpeg-source"},
    {"id": "x264", "directory": "/actual/project/.deps/media-validation/x264-source"},
    {"id": "libportal", "directory": "/actual/project/.deps/portal-validation/source"},
    {"id": "pipewire", "directory": "/actual/project/.deps/pipewire-validation/source"},
    {"id": "gifski", "directory": "/actual/clean/gifski-checkout"}
  ],
  "rustVendor": {
    "directory": "/actual/retained/rust-vendor",
    "registryCache": "/actual/cargo/registry/cache/actual-registry-directory",
    "config": "/actual/retained/vendor-config.toml",
    "noticeReviews": "third_party/rust-notices/upstream.json",
    "replay": {
      "cargo": "/actual/installed/toolchain/bin/cargo",
      "rustc": "/actual/installed/toolchain/bin/rustc",
      "cache": "/actual/cargo-home"
    }
  },
  "materials": [
    {"id": "native-rust-build", "path": "/actual/build/native-rust/build-info.json"},
    {"id": "native-rust-metadata", "path": "/actual/build/native-rust/cargo-metadata.json"}
  ],
  "buildReceipts": [],
  "deployment": {
    "directory": "/actual/final/deployment",
    "inventory": "/actual/retained/deployment-inventory.json",
    "sourceMap": "/actual/retained/deployment-source-map.json"
  }
}
```

项目必须来自实际构建的干净提交；不提供“允许脏源码”选项。导出从 Git blob 读取，并核对对象 SHA，因此 `.gitattributes` 的 `export-ignore` 不能悄悄删掉许可证或源文件。检查实际检出文件的原始字节，拒绝未记录的修改及未跟踪源码；通常应使用 `core.autocrlf=false` 的检出。被 Git 忽略的构建输出不自动进入源码包。初始化后的子模块按父树中的精确 gitlink 递归导出；缺少子模块会失败。读取 Git 时显式禁用输入仓库的 fsmonitor/hooks、签名自动展示、全局/系统配置、缺对象时的懒加载和网络协议，不能因为运行 `ls-files` 或读取提交时间而执行配置的监视/签名程序或获取新对象。

`qtbase`、`gifski`、`ffmpeg`、`x264`、`libportal`、`pipewire` 的提交从项目主锁读取，`appimage-runtime` 从打包工具锁读取，计划不能覆盖这些提交或补丁。libportal、PipeWire、FFmpeg 的实际检出可以处于锁定补丁已应用但未提交的状态：工具在一次性导出上按顺序重放锁中补丁，再与实际目录比较，包括删除的文件。原检出、索引与依赖缓存均不被重置或清理。

## 额外模块与原始源码档案

Qt 其他模块、系统库、Rust 工具链源码及 AppImage 内嵌依赖必须有独立来源记录。额外 Git 组件提供 `commit`、`repository` 和 `pinEvidence`；后者引用真实 JSON 来源文件的 SHA256 及 JSON Pointer。对应值须为完整提交，或以 `@<完整提交>` 结尾的来源定位符。

```json
{
  "id": "qtsvg",
  "directory": "/actual/sources/qtsvg",
  "commit": "ce2d9787bf7c6e4d9ab79b33b99ebbcfec7aef00",
  "repository": "https://github.com/qt/qtsvg.git",
  "pinEvidence": {
    "path": "/actual/matching-sdk/sbom/qtsvg-6.12.1.spdx.json",
    "sha256": "577609a028af444d85e2a83c9fefaab5935724679763274a330de6237f35f639",
    "pointer": "/packages/0/downloadLocation"
  },
  "sourceSbom": "/actual/matching-sdk/sbom/qtsvg-6.12.1.source.spdx"
}
```

这个 qtsvg 提交与文件哈希来自当前 Linux Qt 6.12.1 SDK 的真实 SBOM。使用另一份 SDK 时，必须重新核对它实际提供的来源记录。QtBase 的同类记录与主锁提交一致；当前 QtWayland、QtTranslations 的二进制 SBOM 只有版本，没有完整 Git SHA，不能把 `6.12.1` 猜成一个提交。QtWayland 的 source SPDX 提供逐文件 SHA1，可核对给定真实源码；它不自动补出缺失的提交或原始档案来源。QtBase SBOM 中的 ICU 为 73.2，不能用 Ubuntu 的另一版本 ICU 源码替代。

`sourceSbom` 可选，支持实际 Qt source SPDX 的 tag/value 文件清单；提供后每个记录文件必须存在且字节一致。它是附加证据，不代替完整上游树、许可证、构建参数或精确来源。

Linux 系统库可由 [原始 dpkg 源码收集入口](native-source-materials.md) 产生
`kind: "debian-source"`，计划保留精确 `sourcePackage`、`sourceVersion`、`dsc` /
`sha256` 和 `sourceIndex` / `sourceIndexSha256`。这些必须是原 Sources 字段、
`.dsc` 及其相邻的全部真实源归档。收集器重新核对两层 SHA256 后，用实际
`dpkg-source` 在私有副本上应用 Debian patches，保留 recipes 和原通知；不能
覆盖项目锁中的 Qt/ICU 等组件，也不能额外注入未记录补丁。当前原始发行方
Release 签名 / 索引链及对应构建收据未闭合时继续列为缺项，不因来源包可提取
就认定最终二进制完整。

`kind: "tar"` 可收集实际源 tar/gzip/xz/bzip2 档案：提供 `archive`、`sha256` 和可选的单一根目录 `prefix`。额外来源仍须提供指向该 SHA256 的 `pinEvidence`。固定 AppImage runtime 可直接使用打包工具锁中已核对的 `sourceArchive.sha256`。未知 tar 组件如有受控补丁，以 `patches: [{"path": "...", "sha256": "..."}]` 给出，并原样保留。

只支持明确的 Git 或源 tar 输入，不把 `.deb`、DLL、SDK 安装树或头文件目录推断为完整源码。发行版库要保留与实际二进制版本匹配的源包、发行版补丁、配置及来源证据，例如原始 `.dsc` 和其中校验和指向的源档案；仅保留上游未打补丁的 Git tag 不够。当前工具不自动获取或解译发行版源包、NSIS/CRT 许可例外，也不根据一个网址制造替代源码。相应输入未适配时，具体范围仍须保留在未完成材料中。

所有导入路径拒绝绝对成员名、`..` 越界、大小写冲突、重复成员、硬链接及设备等特殊文件。源码符号链接必须在整个组件收集完成后解析到组件内部的真实目标；拒绝组合后越界、悬空和循环链接。工具也拒绝通过被替换为符号链接的目录读取实际 Git 源文件。

## Cargo vendor 与静态 Rust 输入

聚合的唯一实际锁是 `third_party/localsend/Cargo.lock`，不是 gifski 的参考上游锁。先由固定 Cargo 正常取得锁中全部依赖，再离线保留目录源码：

```sh
# 在 CI 的实际项目根运行；前置依赖准备须已经取得锁中全部包。
cargo +1.97.1 vendor --locked --offline --versioned-dirs \
  --manifest-path third_party/localsend/Cargo.toml \
  "$PWD/artifacts/source-inputs/rust-vendor" \
  > artifacts/source-inputs/vendor-config.toml
```

这条命令缺少缓存时会失败，不能删掉 `--locked` 或伪造缺失包来通过。若 CI 需要网络预取，应在独立依赖准备步骤用固定 Cargo 的 `fetch --locked` 完成；收集器本身始终不联网。Cargo 会把 Git 依赖的清单转换为可替换目录源码，因此不能假定生成的 Cargo.toml 字节等于上游文件。[Cargo vendor](https://doc.rust-lang.org/cargo/commands/cargo-vendor.html) 和[源码替换说明](https://doc.rust-lang.org/cargo/reference/source-replacement.html)描述了这一行为。

收集器逐个检查所有锁中远端包，包括其他平台的锁定包。每个包名、版本、来源、`.cargo-checksum.json` 文件集合与内容必须匹配。注册表包还必须提供原始 `.crate`：先核对整个原始档案的 Cargo.lock SHA256，再解出并与 vendor 比较，防止修改源文件后同时改写目录校验清单。原始 `.crate` 与 vendor 都进入最终档案。只提供 `.cargo-checksum.json` 而没有原始档案会留下明确缺项。

注册表的比较视图遵循实际固定 Cargo 1.97.1 的 [`vendor_this` 规则](https://github.com/rust-lang/cargo/blob/c980f4866141969fab6254a680546a277789d6f0/src/cargo/ops/vendor.rs)：大小写精确的 `.git` 路径组件、任意层级文件名为 `.gitignore` 或 `.gitattributes` 的条目，以及包根的 `.cargo-ok` 不进入 vendor。普通隐藏文件、`.cargo_vcs_info.json`、`.github/` 下源码和嵌套的 `nested/.cargo-ok` 仍须逐字节匹配；不采用统一的隐藏文件忽略规则。原始 tar 完整保留，过滤只用于比较预期文件集合，不改写归档或目录校验清单。通用解包器仍先拒绝 `.git` 管理路径和其他不安全成员，这项兼容处理没有扩展可接受的源档案范围。

`rust-vendor` 的 `registryComparisonPolicy` 记录该规则对应的 Cargo 1.97.1、完整提交和官方源码地址；提供 vendor 时，项目锁中的工具链必须与此策略一致。新工具链需要重新核对实际规则，不能继续沿用旧策略却记录成另一版本。这一字段记录比较依据，不代表收集器实际执行了 Cargo，也不是离线重做 receipt。

剩余项的缺失、额外文件或字节变化都使收集失败，即使同时重写 `.cargo-checksum.json` 也不能通过。诊断分别列出三类差异的总数、每类最多五个路径及每个路径最多 160 个字符，便于核对真实失败而不输出整个源码树。这些注册表比较规则不替代许可证、固定 Cargo 离线 replay 或静态来源闭包要求。

gifski 的完整固定 Git 源码也必须提供；vendor 中源文件与该检出逐字节比较。Cargo.toml 的转换另由实际固定 Cargo 离线重做验证。`replay` 应使用明确的已安装 `cargo`、`rustc` 可执行文件，要求版本与主锁一致，并设置 `RUSTUP_AUTO_INSTALL=0`；即使误指向硬链接形式的 rustup proxy，也不能自动取得缺失工具链。它把所需缓存复制到新的临时目录，不使用原缓存作为可写工作区，再比较完整 vendor 输出。重放移除继承的 Cargo 配置/凭据及代理变量，拒绝项目及祖先目录的外部 Cargo 配置，明确离线并设置空 HTTP proxy。实际成功退出后才生成含 `exitCode`、环境摘要和日志的 `vendor-replay.json`，同时解除对应的 normalized Git manifest 缺项；缺少重做证据时保留缺项，不生成虚构成功日志。

`rust-toolchain-source` 是必需组件。Rust 的 `std`、`alloc`、panic/runtime、compiler-builtins 等静态输入不等于普通 Cargo vendor。应提供实际工具链所用标准库及运行时的完整源码和所需子模块，或带真实 `git-commit-hash` 的固定官方 `rust-src` 档案；重做时从 `rustc --version --verbose` 取得完整提交并核对。原始工具链/标准库许可也必须保留。[工具链材料收集器](rust-toolchain-materials.md)核对实际安装的目标库、官方源码分发包和通知；`rust-src` 不被表述为整个编译器的源码包。

若某个原始 crate 没有携带完整许可文字，不能修改 vendor 添加文件后重写校验和。可在 `rustVendor.licenseTexts` 中用 `包名@版本` 引用另一已验证组件中实际保留的许可文件，字段为 `component`、`path`、`registryChecksum`；校验和必须对应该 crate 的 Cargo.lock 条目。引用路径必须是该组件保留的非空 LICENSE/COPYING/COPYRIGHT/NOTICE 等文件。此映射保留来源关系，许可证适用性仍属于发行审核。

项目已有的精确补充通知通过 `rustVendor.noticeReviews: "third_party/rust-notices/upstream.json"` 接入。工具复用 [Rust 材料 helper](../build/package/rust_materials.py)，在原始 `.crate`、vendor 全文件校验成功后，核对原始 notice 记录中的包来源、registry SHA、完整 `.cargo-checksum.json` SHA、源提交或真实文件内容等价记录，再检查每份通知的 Git blob、大小与 SHA256。r-efi 的 `AUTHORS` 只在这些已核对记录中作为原始许可全文使用，不把任意 AUTHORS 文件自动判定为许可。helper、原始评审 JSON 和复制出的通知均进入档案，保留每个 crate 的 `supplementalNotices` 与 `noticeProvenance`。

gif-dispose 5.0.1 的精确源提交 `e50dcfbd38056d75184720e6c45daf23db16cecb` 未携带 LICENSE，但原始 `Cargo.toml` 明确声明 `MIT OR Apache-2.0`。这里明确选择 Apache-2.0：保留原 manifest 字节、作者和仓库字段，并另存 [SPDX 固定提交的标准全文](https://github.com/spdx/license-list-data/blob/cc4fe1ac462bd3ad3c81a07234baf7d9541ee871/text/Apache-2.0.txt)，原始 Git blob 为 `137069b823873b8bcf42979bcf8e9371052d26a2`、SHA256 为 `074e6e32c86a4c0ef8b3ed25b721ca23aca83df277cd88106ef7177c354615ff`。记录的 `origin` 是 `standard-license`，源 crate 提交与标准文本提交分别保存；这不是把标准文本说成上游遗漏的原始文件，也不填写模板版权或改变原许可表达式。选择、作者、manifest 或文字字节不符都会失败。

`rustls-platform-verifier-android` 0.1.1 的原 `.crate` 固定为 SHA256 `f87165f0995f63a9fbeea62b64d10b4d9d8e78ec6d7d51fb2125fda7bb36788f`。其八个文件没有许可证正文或 `.cargo_vcs_info.json`；原 manifest 声明 `MIT OR Apache-2.0`，发布 `include` 只列源码和生成的 Maven 目录。`Cargo.toml.orig` 与 `src/lib.rs` 精确匹配 [版本源提交](https://github.com/rustls/rustls-platform-verifier/tree/da3d9c36f48fb9f8f97e94f132fdab67ab0fc75b/android-release-support)，该目录的两个许可证链接指向同提交根目录的原 MIT / Apache 文本。补充记录单独保留这两份原文，固定完整评审记录及八文件的 SHA256、大小、Git blob 和来源分类；不改原 crate、vendor 或校验清单。其一个 Cargo 生成 manifest 与五个 Maven 生成文件不被称为该 Git 提交中的原文件。

AAR 的原 blob `8acc8b5fe0b36db4fb45ad8ac4a8f8c3b24fc0c5` 同时精确匹配[上游后来回填的 Maven 档案](https://github.com/rustls/rustls-platform-verifier/tree/a9c4a842e44e41b0ff0ef0ec12486853ce81f34c/android-release-support/maven/org/rustls/rustls-platform-verifier/0.1.1)。这只建立内容对应关系；导出的 `publicationCommitRecorded` 和 `buildProvenanceRecorded` 仍为 `false`，不能由后来的档案推断原构建参数或上传提交。专用绑定只接受这个固定 crate、原八文件清单和原通知，不能作为其他生成文件的通用例外。补充正文只消除该 crate 缺少保留许可文字的缺项；其他源码、构建及最终部署缺项照常保留。Android 锁包内的 AAR 与实际 Windows/Linux 发行目标仍须另行核对，不能仅从全平台 vendor 集合推断产品分发了它。

重建时，将档案根的 `cargo-vendor-config.toml` 复制到 `sources/huhucapture/.cargo/config.toml`，再从该项目根运行原构建命令。目录源码路径为 `../rust-vendor`，即 `sources/rust-vendor`。该操作只用于解出的重建副本，不改原始提交快照。

## 实际构建记录与最终文件映射

`buildReceipts` 引用构建任务产生的严格记录。记录至少包含以下字段；以下省略号不是合法输入值：

```json
{
  "schemaVersion": 1,
  "kind": "huhucapture-source-build-receipt",
  "platform": "linux",
  "commands": [["实际程序", "实际参数"]],
  "profileCommands": {"ffmpeg": 0},
  "toolchain": {"实际编译器": "实际版本与身份"},
  "environment": {"相关构建变量": "实际值"},
  "sources": [{"id": "ffmpeg", "sourceTreeSha256": "实际收集清单哈希"}],
  "staticInputs": [],
  "outputs": [{"path": "lib/libavcodec.so.63", "size": 123, "sha256": "实际输出SHA256"}]
}
```

`sources` 的哈希为收集器对完整文件清单的规范 JSON 计算的 SHA256，可从一次未完成收集的 `components` 读取。CI 应在构建前记录源码身份，构建后确认未变，再将真正执行的参数和输出身份绑定到记录；不能把打算执行的命令写成已执行命令。FFmpeg、x264、PipeWire 用 `profileCommands` 指定 `commands` 中实际配置参数数组的零起始索引。工具要求有序参数与当前锁完全一致，只允许现有构建器明确添加的 prefix、Windows release `/MD`、链接调试参数和 NASM 路径；不接受先出现正确参数、后追加禁用 swscale 或启用 nonfree 的矛盾记录。同一配置程序的重复调用也必须符合该合同。Windows 读取当前 `videoInspectionWindows.configure`，因此新增 swscale 配置不会被旧记录掩盖。

每项计划记录的外层结构为：

```json
{
  "id": "application-linux",
  "path": "/actual/retained/source-build-receipt.json",
  "buildRecords": [
    {"path": "/actual/build/CMakeCache.txt", "sha256": "实际原文件SHA256"},
    {"path": "/actual/build/link-build.log", "sha256": "实际原文件SHA256"}
  ],
  "staticFiles": {"huhucapture-native": "/actual/linked/libhuhucapture_native.a"}
}
```

工具原样保留 `buildRecords` 并核对哈希。后续解析和补丁应用读取的是已复制并验证的快照；原始输入之后被改变，不会让判断依据与归档材料变成两套字节。每项 `staticInputs` 需要 `id`、`component`、`sourceTreeSha256`、`size`、`sha256`；对应 `staticFiles` 必须指向真正参与该次链接的文件，实际字节与记录一致。归档不会把静态库本身当作源码。应用记录须包含 LocalSend、gifski、vendor、Rust 工具链源码，并绑定实际单一聚合 Rust 静态库。

四个现有原生 builder 通过 [`source_receipts.py`](../build/package/source_receipts.py) 在真实执行位置保存记录。它在配置前导出并核对固定提交及有序补丁；配置、编译、安装命令实际运行后才记录退出状态，最后重新检查原源码和产生的文件。每条命令的 stdout、stderr 和 execution JSON 单独留存，实际工具文件的完整大小、SHA256、限定环境及原配置文件进入记录。失败保留已经产生的日志，不能生成成功收据。Windows 的固定 MSYS 脚本只在原始 `set -eu` 脚本字节未变且真实 wrapper 成功退出后，绑定其配置与 make 参数数组；不是将预期命令写成已执行命令。

| 原生构建 | 同一次实际执行的收据目录 |
| --- | --- |
| libportal | `.deps/portal-validation/source-receipt` |
| PipeWire | `.deps/pipewire-validation/source-receipt` |
| x264 | `.deps/media-validation/x264-source-receipt` |
| Linux FFmpeg | `.deps/media-validation/ffmpeg-source-receipt` |
| Windows FFmpeg | `.deps/video-inspection-windows/source-receipt-Release`；Debug 独立保留在 `source-receipt-Debug`，不能替代正式 Release/MD 输入 |

`source-build-receipt.json` 配有 `receipt-plan.json`、真实 `build-execution.json`、完整源码输入清单及原构建记录快照。共享安装前缀按构建前后完整清单比较，FFmpeg 不把此前已经存在且未变的 x264 输出认作自己生成的文件。原生材料收集器另核对导出的 Git 源码清单、原成功执行记录与实际部署库的完整字节，再设置 `buildTimeOutputHashesVerified`。旧 SDK 或旧 `build-info.json` 没有这份原始收据时仍保留缺项，不从意图参数倒造成功记录。此机制没有重新编译旧资产，也不证明独立签名或可重复构建；新真实平台构建仍须运行后才能验收。

原生源码检查使用预期补丁 tree 初始化自己的独立索引，再执行普通 `git add -A`。不对已在 Git 中保留混合行尾的文件强制 `--renormalize`：这种额外转换会改变上游自身的预期 blob。原始工作目录字节、索引和配置始终保留，真实内容变化仍按精确预期 tree 拒绝。`tests/native_source_materials_tests.py` 用真实 Git 混合行尾仓库覆盖此边界；`tests/source_receipts_tests.py` 用真实进程退出、Linux C 编译产物及实际固定 shell 执行验证收据绑定，均不冒充完整依赖重建。

Linux AppImage runtime 除自己的固定源码外，已确认至少使用 `appimage-musl`、`appimage-fuse`、`appimage-squashfuse`、`appimage-zstd`、`appimage-zlib`、`appimage-mimalloc` 六项静态来源；源码必需检查和 runtime receipt 输入检查共用这一集合。固定 runtime 的 [Makefile](https://github.com/AppImage/type2-runtime/blob/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/src/runtime/Makefile)明确以 `-static` / `-static-pie` 链接 `-lmimalloc`，[Dockerfile](https://github.com/AppImage/type2-runtime/blob/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/scripts/docker/Dockerfile)安装 `mimalloc-dev`，不能只依据其 LICENSE 中列出的五项就遗漏它。

这六项是已经证实的最低要求，不是完整静态闭包的证明；最终仍必须逐项匹配真实官方构建和链接记录中的全部输入。上游 runtime 的 31 KiB 级源档案不包含这些嵌入库的完整源码，现有材料也没有给出 mimalloc 的精确版本或源归档。没有匹配官方 runtime 资产的原始构建及依赖材料时，完整性保持 false；不能从 URL、Alpine 基础镜像标签或库名推导内部版本，也不能声称重新构建结果等于原始发布资产。

最终 `deployment.inventory` 必须是 `inventory` 子命令对**实际最终部署目录**产生的全文件清单，并在收集时再次核对。目录不能为空，须有真正的 huhucapture PE/ELF、匹配的嵌入身份和旁置 `build-identity.json`。工具复用 `build/package/metadata.py` 的实际运行目录审计；缺少 Qt、许可证或安装标记等会留下阻塞缺项。默认可执行路径为 Windows 的 `huhucapture.exe` 或 Linux 的 `bin/huhucapture`；完整 AppDir 可显式设置 `deployment.executable: "usr/bin/huhucapture"`，DEB 解包目录可指定其真实 `opt/.../bin/huhucapture` 路径。产品可执行文件必须映射到包含项目源码身份的实际应用构建记录。`deployment.sourceMap` 按相对路径映射每个文件：

```json
{
  "lib/libavcodec.so.63": {"receipt": "ffmpeg-linux", "output": "lib/libavcodec.so.63"},
  "share/huhucapture/licenses/LICENSE": {"component": "huhucapture", "path": "LICENSE"},
  "lib/libexample.so": {"linkTarget": "libexample.so.1"}
}
```

直接复制的文件必须与指定源码文件字节相同；生成的程序、库、翻译或资源必须与构建记录的输出字节相同。三种映射形式互斥，不能同时填 `component` 和 `receipt` 让产品检查与实际字节验证走不同分支。链接必须与实际安全链接目标一致。所有文件都需要映射，不能以扩展名、system 或 unknown 标签绕过检查。完成身份、目录及映射验证后再次计算整个部署清单；中途变化会失败。DEB/AppImage 的部署器可能改写 ELF 或启动器，因此应在实际最后一次变换后记录真实输出和变换参数，不能用 SDK 中变换前的哈希代替。

两种发行形态共同放在冻结的 `payloads/` 下时，使用明确的 `deployment.products`，不再同时填写旧的 `deployment.executable`：

```json
{
  "directory": "/actual/prepared-stage/payloads",
  "inventory": "/actual/prepared-stage/source-deployment-inventory.json",
  "sourceMap": "/actual/retained/deployment-source-map.json",
  "products": [
    {"root": "appimage/usr", "executable": "bin/huhucapture", "packageKind": "appimage"},
    {"root": "deb/opt/huhucapture", "executable": "bin/huhucapture", "packageKind": "deb"}
  ]
}
```

Windows 对应 `installer`、`portable-zip` 两个 root，均使用 `huhucapture.exe`。每个产品目录分别检查实际二进制、相同构建身份、旁置记录和该包类型的安装标记，并独立运行部署审计；不能让另一目录的通过替代它。根目录不能重叠、越界或经由符号链接，包类型不能重复。整个 payloads 的清单和映射仍覆盖 root 外的 AppRun、DEB 控制文件、启动器及全部附加文件。报告保留按 root 排序的 `deployment.products`，与同一完整 `files`、`treeSha256` 供 finalize 再次逐项核对。单目录旧计划仍可用于未完成材料，正式双包聚合必须提供两种真实形态。

Windows 应用构建记录保存 CMake 实际选择的 x64 v143 CRT 原始路径、微软
Authenticode 签名信息及完整文件哈希。两种包的应用根目录通过 `microsoftRuntime`
映射逐个核对 DLL 与该 Visual Studio 安装中原始文件的字节，源码归档保留
校验记录。微软提供这些 DLL 的可再分发形式与条件。
[Visual Studio 2022 再分发说明](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)

Linux DEB 的 `control` 和 `md5sums` 由 CPack 根据实际打包输入生成。包准备阶段
保留原配置和执行记录。源码装配从同一个 DEB 重新提取控制文件，用 `debControl`
映射核对它们与冻结清单的尺寸和哈希。安装测试对照实际控制归档及文件系统中的安装内容。

## CI 接入顺序与当前边界

### 从实际材料装配最终部署计划

`scripts/assemble-source-plan.py` 填补材料收集与 `package-qt.py finalize`
之间的装配步骤。它读取一个或多个真实来源计划及生产者已经生成的
`receipt-plan.json`，先复用主收集器验证源码、锁定补丁、许可、原始 crate、
真实 offline vendor replay 和构建输出记录，再对 `prepare` 的冻结部署目录
建立逐文件映射。相对路径按各自输入计划所在目录解析；crate 许可内的源码路径
及固定 noticeReviews 路径仍保留其原有含义。

```sh
python scripts/assemble-source-plan.py \
  --inputs artifacts/native-build-sources/source-plan.json \
  --inputs .deps/qt-runtime-linux/source-plan-fragment.json \
  --inputs artifacts/native-source-plan-linux/source-plan-fragment.json \
  --inputs artifacts/system-source-linux/source-plan.json \
  --receipt-plan artifacts/application-source-receipt/receipt-plan.json \
  --stage artifacts/packages/linux-prepared \
  --output artifacts/source-plan/linux

python scripts/package-qt.py finalize \
  --stage artifacts/packages/linux-prepared \
  --output artifacts/packages/linux \
  --source-plan artifacts/source-plan/linux/source-plan.json \
  --source-output artifacts/source-complete/linux
```

其中每份输入必须已经由实际同一提交/平台的来源准备或构建步骤产生；这些示例
路径本身不是材料。`--inputs`、`--receipt-plan` 可以重复，但互相矛盾的组件、
receipt、项目或平台会失败。输出目录必须是新目录，不能覆盖冻结 payload 或
候选包。装配前后都重新执行原有 `verify_prepared`，来源提交、版本和包身份
必须相同，部署字节不能发生变化。

已验证构建记录的输出与最终文件的大小、SHA256 完全一致时可直接映射；
原生 ELF/PE 必须有构建记录，不会因源码目录恰好包含一个相同二进制就获得
构建来源。直接复制的源码或通知按精确字节映射。项目、Qt、Rust crate 和 Rust
源码工具链通知存在多份相同文本时，按采集器记录的安装路径规则选择对应原件。
仍无法唯一决定时，报告保留歧义，可以通过 `--source-map` 指定来源，由主收集器逐项验证。
工具不从文件名猜测版本，也不从预期命令重造 receipt。

输出为 `source-plan.json`、`deployment-source-map.json`、`source-plan-report.json`
和原始输入计划快照。报告列出未映射文件、候选来源、源码哈希、已验证 receipt
及全部已有缺项。装配成功只表示这些材料可以交给正式收集器；不会生成 Release、
填写 `complete` 或解除门禁。`finalize` 仍真实执行
`create-source-bundle.py --require-complete`。linuxdeploy/objcopy 改写过的字节
还必须通过下述原始逐文件变换链；只有变换前的 receipt 时继续报告缺项。

装配同时保留此次打包的原始输入清单、linuxdeploy 后与调试分离后的两份 Linux
清单，以及真实命令、退出状态和完整原始输出日志。每份中间清单都绑定实际执行
命令的连续索引范围及文件哈希；独立复核要求所有命令成功、原始日志未变、最终
输出仍与冻结 payload 一致。这些材料帮助定位字节发生变化的生产边界，本身不
充当缺失的上游源码或构建收据。旧候选若没有这些当时保留的材料，需重新运行
真实准备步骤，不能从最终文件倒填中间记录。

`file-transformations.json` 在实际复制、标记生成和 ELF 变换执行点记录输入/输出
路径、大小、SHA256 及原始命令索引。Linux 从已锁定 linuxdeploy 镜像提取它本身的
patchelf，利用该固定版本支持的 `PATCHELF` 环境入口调用记录包装器；包装器仍执行
同一 patchelf，并保留每次真实调用、退出码和完整 stdout/stderr。只接纳上游该版本
使用的 `--print-rpath` 与 `--set-rpath`，其他操作直接失败。`objcopy` 的调试分离与
debuglink 绑定其真实命令及实际调试文件。安装标记、DEB desktop 修改绑定原始字节、
构建身份与本次实际执行的脚本，脚本原件再与保留的项目源码逐字节核对。

装配器产生的 `sourceTransformations` 指向原始准备目录和完整执行材料；来源映射的
派生形式如下，保留原生产者 receipt，而不是创建另一份编译记录：

```json
{
  "transformation": "packaging",
  "output": "appimage/usr/bin/huhucapture",
  "input": {"receipt": "application", "output": "bin/huhucapture"}
}
```

消费端独立验证最终字节、逐步连续的 before/after、原始成功命令及完整日志，然后
对原始安装输入重新核对已有构建 receipt；所有原源码、构建记录与静态输入保持原样。

逐文件记录 schema 2 另支持 `observed-input` 根：linuxdeploy 新部署的文件若不在
原 CMake 安装目录里，第一次真实 PATCHELF 调用之前观察到的大小和 SHA256 必须
唯一匹配一项已独立验证的原生产者输出。这里证明的是该次实际输入的字节身份，
不推测复制来源路径，也不补写一个未观察到的 copy。首个观察的规范路径、原调用
序号、父命令索引和记录哈希都保留；随后每次只读 `--print-rpath` 与写入
`--set-rpath` 必须按原始顺序消费，之后的 strip/debuglink 也必须连续。

同一路径一旦出现未记录的中间变化，后续再次匹配任何生产者输出也不能重新起根。
原生产者缺失、多个输出有相同字节、任何只读/写入步骤缺失或最终字节不一致都
保留为缺项；显式来源映射和相似文件名不能绕过这条规则。已有 installed 根语义
保持不变，schema 1 不会获得新的 observed 根。新系统库只有 `.deb`、`.dsc` 或
来源版本关联时，仍然缺少实际构建证明；本机制不能替代它。最终 `--require-complete`
继续检查所有源码、原构建收据、静态输入及部署文件。旧准备目录缺少原始逐文件
记录时必须重新真实执行 `prepare`。

`source_receipts_tests.py` 可在 Linux 上使用
`HUHUCAPTURE_TEST_LINUXDEPLOY=/absolute/path/linuxdeploy-x86_64.AppImage` 执行
这一边界的实际测试；镜像必须逐字节符合打包锁。测试从真实 `cc` 生产收据开始，
由该镜像实际部署、修改 rpath、执行受控只读观察并 strip，然后验证最终 ELF 可运行。
它还实际编译两个同字节输出以检验歧义拒绝，并在真实调用之间替换文件以检验断链
不能重新起根。这些是窄范围生产材料合同，不是完整应用打包或产品发行验收。

后续还要再次执行 offline vendor replay 时，固定 Linux 保留流程需显式添加
`--retain-replay-cache`。该选项只把此次任务私有 cache 中的
`registry/cache`、`registry/index`、`git/db` 复制到 `inputs/replay-cache`，
逐字节核对前后清单，并让输出计划指向这些保留输入。不会复制 Cargo home
根目录的配置、credentials 或构建 target；默认的临时 cache 生命周期不变。
后续 replay 仍在另一临时副本执行固定工具链，保留 cache 不是一次成功 replay
的替代证明。

### 所需执行顺序

1. 同 SHA 干净检出并记录版本、主依赖锁和 Cargo.lock。依赖来源按照固定锁准备；保留构建器使用的真实上游目录和补丁状态，不在任务结束前删除。
2. 保留完整 vendor、原始注册表包、实际固定工具链源码、真实 Qt 来源/SBOM及许可。Linux 还要按最终部署字节查清 OpenSSL、ICU、其他 native 库和 AppImage 内嵌依赖的实际源版本。宿主 compositor、PipeWire daemon、glibc loader 和驱动不因开发 SDK 包含它们就自动成为产品部署输入。
3. 用第一次收集结果取得精确源码身份；构建任务记录真正执行的参数、工具、配置/链接证据、静态输入与输出。任何源码或配置变化都必须重新匹配记录，不能继续使用旧哈希。
4. 对每种实际最终包目录生成完整文件清单和映射；保存部署工具的实际变换记录，再收集第二次。存在缺项时上传源码档案和 `gaps` 作为开发证据，保留正式分发阻塞。
5. 材料齐全的发行任务使用 `--require-complete`。将实际平台/架构档案与对应来源清单传给 `package-qt.py` 的 `--source-bundle`、`--source-manifest`；身份/档案描述符/组件家族格式兼容现有消费者。Windows 与 Linux 分别绑定自己的实际输入，不用一端的清单证明另一端构建。

目前没有通过此工具产生并审核一份实际双平台完整对应源码包。完整原始 Qt/发行版/Rust/runtime 源码和匹配构建记录未全部供应时，工具应报告未完成；这是预期行为。它不解除 P11/P12 的正式发行、干净机器安装、实际桌面和长期录制验收门槛。

Windows Qt 原签名插件与原 SPDX 的完整字节差异不通过 PE 归一化消除。[独立 Qt 源码构建入口](windows-qt-source-build.md)从锁定 qtbase、qtsvg、Qttools 和 qttranslations 生成新的统一前缀，使用相同 producer 收据和 Qt 自身 SPDX 检查；需要先在真实原生 CI 构建并完成应用验证，再用于正式部署。原 SDK 与新构建材料各自保留来源，不互相替代。

### 在真实 Rust 与应用构建边界记录来源

已有 `native-rust/build-info.json` 的锁和输出哈希不能补成构建前源码证据。新的正式来源路径必须从干净的同 SHA checkout 开始，使用新的准备、Cargo target、CMake build、install 和 evidence 目录。先按项目锁安装 Rust 工具链及平台 SDK，再执行：

```sh
python scripts/prepare-native-build-sources.py --output artifacts/native-build-sources
python scripts/build-native-rust.py \
  --source-plan artifacts/native-build-sources/source-plan.json \
  --target-dir artifacts/cargo/localsend \
  --evidence artifacts/native-rust
```

第一步真实执行固定 Cargo 的 `fetch --locked`、原始 metadata 和离线 `vendor`，保留完整 gifski Git 源码、原始目录替换配置与 `.crate` 缓存位置，并在聚合库尚不存在时核对官方 Rust 源码、实际编译器和标准库文件。工具链报告明确记为 `preparedBeforeAggregateBuild: true`、`aggregateBuildBound: false`。第二步先独立导出项目、核对全部 vendor/原始包和标准库来源、再次真实离线 replay，再用相同固定 Cargo 和已核对的绝对 vendor 路径构建到新 target；私有 Cargo home 排除继承的目录替换与编译包装器。源码、配置、编译器、标准库或实际输出在前后发生变化都会失败。原 `build-info.json` 的 ABI 字段保持兼容，另加本次执行所得 `sourceBuildProof`，不会给旧 aggregate 倒造证明。

平台依赖和真实许可证准备好后，使用应用包装器替换该应用的原 configure、build 和 install 三个调用。Linux 的实际入口例如：

```sh
python scripts/build-application-with-sources.py \
  --source-plan artifacts/native-build-sources/source-plan.json \
  --native-receipt artifacts/native-rust/build-info.json \
  --build-directory artifacts/cmake/application-linux \
  --installed artifacts/install/linux \
  --evidence artifacts/application-source-receipt \
  --cmake-argument=-DHUHUCAPTURE_RUNTIME_LICENSES="$PWD/artifacts/runtime-licenses" \
  --cmake-argument=-DHUHUCAPTURE_OPENSSL_RUNTIME_DIR=/usr/lib/x86_64-linux-gnu
ctest --preset qt-application-linux
```

Windows 使用 `application-windows`、`install/windows`、`application-source-receipt` 路径和实际 Windows runtime license 目录；包装器选用现有 VS 2022 x64/v143、RelWithDebInfo 配置。每个额外 CMake 选项须独立传入一个 `--cmake-argument=<参数>`，包括平台测试所需的真实 fixture 路径。现有 CTest 预设继续指向相同实际 build 目录，应用测试和安装测试仍需执行。

应用包装器在 configure 前独立检查原 Rust 执行证明及全部源身份，并读取 CMake 真正生成的 file API link model，确认即将链接的是该聚合库的准确路径；相同字节但未链接的另一个文件不能替代。只有真实 configure、build、install 成功、安装后二进制嵌入身份与源码一致、原输入及工具再次核对无变化，才写出 `artifacts/application-source-receipt/source-build-receipt.json` 和 `receipt-plan.json`。保留材料包括命令退出码、日志、完整源码清单、工具身份、实际 CMake cache/install/file API/link 工程记录、原 Rust 执行记录和实际静态库。receipt 的产品输出覆盖实际应用可执行文件及本次 CMake 生成的身份、安装标记；安装器复制进来的 Qt DLL、其他运行库和后续打包变换仍须各自真实来源。

最终装配使用 `artifacts/native-build-sources/source-plan.json` 的标准 `sources`/`rustVendor`/`materials`，以及应用 `receipt-plan.json`。Qt 源码 builder 成功后另生成 `<Qt workspace>/source-plan-fragment.json`，直接引用原模块源码与 producer receipts。上述准备、源码目录、原始缓存和 build records 必须一直保留到最终来源装配结束；安装目录被部署工具改写后，必须另外绑定真实变换链。此路径实际运行成功也只闭合对应应用和 Rust 输入，不能替代 Qt、发行版系统库、AppImage 静态依赖与最终部署映射的各自完整性检查。

各 native 依赖使用同一轮 builder 的原始报告生成独立 fragment，不把后续开发留存 profile 的另一个 vendor 目录合入正式计划：

```sh
python scripts/collect-native-build-plans.py --output artifacts/native-source-plan-linux
```

此入口在 Linux 固定选择 libportal、PipeWire、x264 和 FFmpeg 的实际工作目录；Windows 同命令可使用 `artifacts/native-source-plan-windows` 输出目录，且只选择原 `Release/MD` FFmpeg producer。脚本核对锁定源码及补丁、原 `build-info.json` 与 `receipt-plan.json` 一致性、成功执行 journal、真实 builder 文件、工具/环境、原 prefix 内所有已记录输出的准确字节，并保留原收据副本。输出 `source-plan-fragment.json` 没有 `rustVendor`，可与构建前准备的 Rust 源计划、Qt 模块 fragment 和应用 receipt 无重复合并。旧 Linux 开发 profile 仍独立保留 incomplete 证据；它的 `collection-plan.json` 不应同时输入这个正式装配路径。Rust 的 ABI `build-info.json` 也不是标准 receipt-plan；其真实构建证明与 aggregate 已由应用 producer 检查并保留在应用 receipt 中。

### 保留现有 Linux 应用 CI 的真实来源

`scripts/retain-development-sources.py` 是当前完整 Linux 应用 job 的固定编排入口，调用上面的收集器。它默认离线，显式选项只允许取得现有 Cargo.lock 指定的公开来源；它不构建依赖，也不提供任意组件/profile 扩展。应在 `build-native-rust.py`、`build-portal.py`、`build-pipewire.py`、`build-software-media.py` 已成功之后运行；可以安排在应用测试和打包之后、最终上传之前，使前面的测试失败也不丢失已经取得的来源材料。

```sh
python scripts/retain-development-sources.py \
  --qt-prefix "$QT_ROOT_DIR" \
  --source-commit "$(git rev-parse HEAD)" \
  --output artifacts/source-retention/linux-incomplete
```

`--source-commit` 必须等于真正的 checkout HEAD。PR 工作流的合并检出不能替换成 PR 分支的 `head_sha`。脚本开始和结束都检查干净工作树，并确认自身已属于该提交；新脚本尚未提交时不能执行这个 CI profile。所有生成物只能进入该项目忽略的 `artifacts/`，输出目录必须是新的，旧结果不会被覆盖。

固定输入包括以下实际路径：

| 来源 | 原始输入 |
| --- | --- |
| libportal | `.deps/portal-validation/source` 与同 workspace 的 `build-info.json` |
| PipeWire | `.deps/pipewire-validation/source` 与同 workspace 的 `build-info.json` |
| x264、FFmpeg | `.deps/media-validation/x264-source`、`ffmpeg-source` 与 `build-info.json` |
| 聚合 Rust | `artifacts/native-rust/build-info.json`、`cargo-metadata.json`、`build.log`，以及记录指向的实际静态库 |
| Cargo 锁 | `third_party/localsend/Cargo.lock`，核对主锁、实际构建记录及当前源码文件身份 |
| gifski | 从本次 Cargo metadata 的精确 Git source 和 `manifest_path` 定位实际 checkout |
| Qt 来源材料 | `--qt-prefix` 下确实存在的 `sbom/` 文件，保留原文件名和字节 |
| AppImage runtime 自身源码（已有时） | `artifacts/packaging-tools/appimageRuntime/source.tar.gz`，按 `build/package/dependencies.lock.json` 的实际大小及 SHA256 核对 |
| 实际应用构建身份（已有时） | `artifacts/install/linux/bin/huhucapture` 的嵌入身份与同目录原始 `build-identity.json` |

原始构建记录以校验后的副本参与后续判断，并作为 `materials` 保存，不改写成严格构建 receipt。脚本核对四个 native 记录的固定提交、补丁和已有配置字段，以及 Rust 构建记录中的锁、源码文件和实际静态库哈希。新原生 builder 在同次执行中实际产生 `sourceBuildReceipt` 时，profile 校验其原工作目录边界及每份构建记录 SHA256，复制快照后交给主收集器独立校验；原记录缺失保持缺项，矛盾哈希或越界路径失败。Qt SBOM 只作为来源材料，不被声明为 Qt 完整源码。

实际安装树存在时，profile 从真实 ELF 读取嵌入身份，核对旁置原件、当前 clean checkout、版本及两份锁，再把验证后的旁置快照作为 `buildIdentity` 交给主收集器。缺少安装树保留具体缺项；矛盾身份硬失败。这个绑定只证明实际构建身份，不替代最终包变换后的部署映射或应用构建 receipt。

已有打包步骤取得 runtime 原始 tar 时，profile 将其校验后复制到 `materials/appimage-runtime-source/source.tar.gz`，并向现有 `sources` 追加 `appimage-runtime` 的 tar 条目；单一根目录为 `type2-runtime-<锁定完整提交>`。collector 再按导出项目的打包锁核对归档、保留原件并导出源码及许可证。原始路径缺失时只在 coverage 的 `missingMaterials` 中记录原因，继续保留 runtime 必需源码缺项；归档存在但大小或哈希矛盾时立即失败。此过程不下载，也不修改原始 tar 内的源码或版本占位文件。

runtime tar 及其固定哈希证明自身源码输入，不能替代官方构建 receipt 或上述至少六项静态来源。新增这一源码可能触发更具体的构建记录缺项，使 `gaps` 数量增加；脚本不会为了减少缺项计数而省略检查，也不会推测 mimalloc 的版本、来源或实际链接输入。

Cargo 的 metadata、锁、原始档案及 vendor 是四个分别记录的集合。脚本遍历锁中的全部 registry 包，从实际 metadata 路径推导本次 cache 目录，只保留整个 `.crate` SHA256 与 Cargo.lock 一致的公开 registry 档案。找不到的包进入 `missingRegistryArchives`；字节不符直接失败。缺少某个 metadata 条目不意味着该包一定不在缓存中，metadata 里有某个条目也不等于已经提供其原始档案。

gifski 的实际 checkout 先使用收集器已有的 Git 对象与逐字节检查，只额外识别 Cargo 自己的 `.cargo-ok` 完成标记，其他未记录源码仍会拒绝。之后从这个已核对的本地 checkout 建立干净对象副本，禁用 hooks、fsmonitor、签名自动展示、外部 Git 配置及懒加载；不访问远端。实际原始 checkout 不会被清理或重置，最终由主收集器再次核对固定源码。

vendor 使用确实已安装的 Rust/Cargo 1.97.1，先记录实际程序版本与哈希，然后执行 `vendor --locked --offline --versioned-dirs`。所有相关子进程都设置 `RUSTUP_AUTO_INSTALL=0`；除下面明确开启的 fetch 阶段外，始终设置 `CARGO_NET_OFFLINE=true`。一次性私有 cache 只复制本次 registry index 和已验证的锁定 `.crate`，gifski database 从已核对的本地 checkout 以禁止 hooks 和远端访问的 bare clone 准备。不复制整个 Cargo home、缓存 Git 配置或 credentials；移除继承的 Cargo 配置/凭据环境变量，并拒绝工作目录及祖先路径中的 `.cargo/config`、`config.toml`，避免 Cargo 绕过私有 home 读取外部配置。完成后删除此次临时 cache，原构建 cache 和 checkout 不参与写入。

固定公开 CI profile 还明确设置 `CARGO_HTTP_PROXY` 为空并移除继承的代理环境变量，采用直接下载；这个选择写入每条命令的环境摘要。固定 Cargo 的[代理选择代码](https://github.com/rust-lang/cargo/blob/c980f4866141969fab6254a680546a277789d6f0/src/cargo/util/network/proxy.rs)在未指定 Cargo 代理时会用 libgit2 读取外部 Git 配置，不能只依靠 Git CLI 的隔离变量。显式空值阻止这条回退，并按 [libcurl 的规则](https://curl.se/libcurl/c/CURLOPT_PROXY.html)禁用代理。唯一 Git 依赖 gifski 的私有 bare database 必须再次通过完整锁定提交与对象哈希检查；固定 Cargo 在已拥有该对象时直接使用本地 database，不进入 Git 网络凭据路径。

vendor 的真实退出码、命令及 stdout/stderr 都会保留。退出码为 0 才把实际目录、配置和精确 notice 评审交给 `rustVendor`；完整锁覆盖仍由主收集器逐项检查。私有 cache 持续保留到主收集器结束，以已核对的程序路径和 cache 实际再做一次 `replay`，然后才清理。调用前不会预写成功 receipt；第二次 vendor 的实际输出不匹配会使 collector 失败。vendor 非零时保留已验证的原始 `.crate` 和原始日志，省略 `rustVendor`，继续生成明确缺项的档案。不安装工具链，不生成替代 vendor/checksum，也不把某个子命令的失败记录改成成功。

需要补齐当前锁中尚未缓存的 registry 原始档案时，在同一固定 profile 增加显式选项：

```sh
python scripts/retain-development-sources.py \
  --qt-prefix "$QT_ROOT_DIR" \
  --source-commit "$(git rev-parse HEAD)" \
  --output artifacts/source-retention/linux-incomplete \
  --fetch-locked-sources
```

这个选项在任务私有 cache 中使用刚核对的 Cargo 1.97.1 可执行文件，真实执行 `fetch --locked --manifest-path <checkout>/third_party/localsend/Cargo.toml`，仅此子进程设置 `CARGO_NET_OFFLINE=false`。不传 `--target`，因此获取固定锁对所有平台保留的依赖；不重算或替换原生构建的 `cargo-metadata.json`。Cargo 的[官方 fetch 文档](https://doc.rust-lang.org/cargo/commands/cargo-fetch.html)说明锁定和目标选择规则；实际已安装版本对应的[固定实现](https://github.com/rust-lang/cargo/blob/c980f4866141969fab6254a680546a277789d6f0/src/cargo/ops/cargo_fetch.rs)调用完整 workspace resolution，并下载整个遍历集合。它不执行产品编译。

fetch 前先从实际提交导出并逐字节核对项目源码，保存 Cargo.lock 原始字节；fetch 返回后再次核对源码、锁和实际 HEAD。无论 fetch 返回 0 还是普通非零，都会逐条重扫私有 cache，只把 SHA256 与原锁一致的新增 `.crate` 发布到保留目录。已经保留的档案只核对、不覆盖；缺项、原始 coverage 和 fetch 后 coverage 分别记录。锁、源码或档案的真实矛盾仍立即失败。网络请求失败可能留下部分有效档案，这些档案会进入后续正常离线收集。

随后仍实际执行原有 offline vendor 和 collector。显式 fetch 非零（或真实启动失败）不会阻止这两个阶段尝试保留已有材料；如果收集成功，报告状态为 `incomplete-bundle-retained-fetch-failed`，脚本最终仍返回 **1**，保留这一请求步骤的失败。fetch 退出 0 也不能独立证明 vendor 成功或源码完整。只有未请求 fetch，或实际 fetch 成功且 collector 正常完成时，profile 才返回 0。collector 自身的矛盾不降级为普通缺项。

新输出目录包含：

| 路径 | 含义 |
| --- | --- |
| `bundle/` | 主收集器真实生成的 `huhucapture-v<版本>-linux-amd64-sources.tar.gz` 与同前缀 `-source-manifest.json`；本 profile 要求实际结果为 `complete: false` |
| `collection-plan.json` | 实际提供给收集器的固定来源计划 |
| `cargo-coverage.json` | 完整锁与实际 metadata 集合、原始档案及缺项、vendor 真退出码和实际输出包集合；请求 fetch 时另存前后 coverage、真实命令结果和源码/锁不变检查 |
| `retention-report.json` | 实际 checkout SHA、执行命令、退出码、材料哈希、最终失败原因或未完成归档结果 |
| `logs/`、`vendor-config.toml` | 实际命令输出；fetch 使用独立的 `cargo-fetch.stdout.log` / `cargo-fetch.stderr.log`；vendor 失败时 stdout 文件可能为空或不构成配置 |
| `materials/` | 复制并校验后的原始记录、Qt SBOM，以及已有且通过打包锁校验的 runtime 原始 tar |
| `inputs/registry-cache/` | 仅发布校验成功的原始 `.crate`，失败的复制不会冒充已验证材料 |
| `inputs/gifski/` | 核对过的本地干净 checkout，供主收集器导出 |

上传应使用 `if: always()`，保留 `bundle/`、两份报告、计划、日志、`materials/` 和 `inputs/registry-cache/`。需要保留 collector 失败时的 gifski 源文件时也可上传 `inputs/gifski/`，排除其 `.git/` 管理目录。`inputs/rust-vendor/` 是中间产物，vendor 非零时可能只写入一部分；有效源码以主收集器完成的档案为准。

本 profile 故意不供应严格 build receipts、最终部署映射、Rust 标准库及完整 Qt/发行版/AppImage 来源闭包，因此正常产物仍未完成。vendor 缺缓存是可记录的材料缺口；源码、锁、已有档案或主收集器发现真实矛盾时，脚本仍以非零退出，已经校验的输入及诊断文件留在新目录。它不把一次离线编排或组件检查表述为完整应用流水线、源码完备性或发布验收通过。

## 自动化验证

```sh
python3 tests/source_bundle_tests.py -v
python3 tests/source_retention_tests.py -v
```

测试使用真正的临时 Git 仓库、Git 补丁、源码 tar、Cargo registry 形式的原始档案和真实文件，不访问网络，不操作产品工作树。覆盖固定对象与许可证导出、索引/refs 不变、未记录修改/删除/未初始化子模块、篡改 vendor 后重写校验清单、路径及链接链安全、确定性归档、未完成状态与覆盖拒绝，以及 fsmonitor 不执行、保留后原输入被改变、矛盾配置参数和空/错误部署目录。Linux 的输出绑定测试用实际 `cc` 编译带真实嵌入身份的小型元数据探针并验证篡改检测；它不会被认定为完整产品，运行目录审计仍明确失败。缺少编译器会使该测试失败。普通 Windows 环境明确跳过这一 Linux ELF 检查以及需要 Unix 可执行脚本/符号链接权限的构造测试，其余 Git、原始档案和清单检查继续执行。

固定 profile 的窄合同测试另外使用临时 Git 仓库、实际文件与受控 Python 子进程，验证部分结果留存、前后 coverage、继承配置隔离、真实退出码/双日志、锁及其他源码中途变化拒绝，以及显式 fetch 失败后的最终非零语义。runtime 的窄回归核对缺失材料、大小/SHA矛盾、快照路径和固定 tar 收集，确认只有自身源码时仍缺 receipt、旧五项静态输入不能满足包含 mimalloc 的最低要求。测试不会联网或冒充一次真实 Cargo fetch/vendor 或官方 runtime 构建；这些执行必须另外保留实际证据。

这些测试证明收集和绑定规则，不是一次产品源码完备性、实际 Cargo 工具链重做或正式许可验收。实际固定 Cargo 重做及完整平台源码包必须另外保留真实执行证据。
