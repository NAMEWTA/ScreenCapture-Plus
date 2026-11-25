# 对应源码的离线收集与验证

`scripts/create-source-bundle.py` 为 [Issue #5 第 13 节](https://github.com/NAMEWTA/huhucapture/issues/5) 的对应源码材料提供可重复的收集入口。它读取真实源码、固定依赖锁、Cargo vendor、构建记录和最终部署目录，生成源码档案及 `source-manifest.json`。它不下载依赖、不执行发布、不修改输入工作树、Git index 或分支，也不把 SDK 二进制或上游网址当成已提供源码。

完整性由实际输入计算。默认允许生成有用的未完成档案，清单中写入 `complete: false` 和逐项 `gaps`；`--require-complete` 在保留这些结果后以退出码 2 结束。身份、校验和、补丁、路径或构建字节互相矛盾时直接失败，退出码为 1，不生成一个看似有效的最终清单。不存在 `--approve`、`--force-complete` 或计划文件中的 `complete` 覆盖字段。

这个检查建立**所记录源码、构建输入及部署文件之间的可核对关系**。最终发行仍须审核实际依赖范围、许可证条件、重建方式与平台安装结果。通用构建记录由产生它的 CI 任务负责如实记录；工具不能从一份 JSON 推断未记录的链接输入，也不能证明某项法律判断。当前各构建器已有的 `build-info.json` 是重要原始材料，但不全部包含源码清单、静态链接范围和最终文件绑定；不能仅重命名这些文件就宣称材料完整。

## 入口及输出

要求 Python 3.11 或更新版本和 Git。常规收集不需要 Qt、编译器或联网。只有明确提供 `rustVendor.replay` 时才执行已安装的固定 Cargo/Rust 工具，用 `--locked --offline` 实际重做 vendor，不编译产品。

```sh
python3 scripts/create-source-bundle.py collect \
  --plan artifacts/source-inputs/collection-plan.json \
  --output artifacts/sources/linux

python3 scripts/create-source-bundle.py inventory \
  --directory artifacts/install/final-appdir \
  --output artifacts/source-inputs/deployment-inventory.json
```

输出目录和清单文件必须不存在。档案命名为 `huhucapture-v<版本>-<平台>-sources.tar.gz`，根目录是 `huhucapture-sources/`，包含：

| 路径 | 内容 |
| --- | --- |
| `sources/huhucapture/` | 精确、干净的项目 Git 提交，包括版本源、依赖锁、补丁、脚本、源码、测试及 NOTICE |
| `sources/<组件>/` | 验证后的真实上游源码，已重放匹配的受控补丁 |
| `sources/rust-vendor/` | 与聚合 Cargo.lock 对应的真实目录源码 |
| `materials/` | 原始源码档案、原始 `.crate`、原始构建/部署记录、补丁、来源证据及收集脚本 |
| `cargo-vendor-config.toml` | 验证原配置后生成的相对路径离线替换配置；不会写入原项目工作树 |
| `vendor-replay.json` | 实际 Cargo 离线重做 vendor 的工具版本、源码提交、参数、日志及输出哈希；没有执行时不生成 |
| `source-inventory.json` | 每个源码文件和链接的身份、组件与缺项 |

外部 `source-manifest.json` 保留相同数据，并绑定实际档案的名称、大小和 SHA256。生成时固定条目顺序、UID/GID、权限及提交时间，gzip 不写当前时间或临时文件名；在相同工具和输入下可重复生成相同字节。完成后实际重新打开档案、解出到新的临时目录并逐文件比较。构建记录中的原始内容属于输入，修改记录或工具版本可能改变档案。

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

gifski 的完整固定 Git 源码也必须提供；vendor 中源文件与该检出逐字节比较。Cargo.toml 的转换另由实际固定 Cargo 离线重做验证。`replay` 应使用明确的已安装 `cargo`、`rustc` 可执行文件，要求版本与主锁一致，并设置 `RUSTUP_AUTO_INSTALL=0`；即使误指向硬链接形式的 rustup proxy，也不能自动取得缺失工具链。它把所需缓存复制到新的临时目录，不使用原缓存作为可写工作区，再比较完整 vendor 输出。缺少重做证据时保留缺项，不生成虚构成功日志。

`rust-toolchain-source` 是必需组件。Rust 的 `std`、`alloc`、panic/runtime、compiler-builtins 等静态输入不等于普通 Cargo vendor。应提供实际编译器对应的完整源码及子模块，或带真实 `git-commit-hash` 的固定官方源码档案；重做时从 `rustc --version --verbose` 取得完整提交并核对。原始工具链/标准库许可也必须保留。

若某个原始 crate 没有携带完整许可文字，不能修改 vendor 添加文件后重写校验和。可在 `rustVendor.licenseTexts` 中用 `包名@版本` 引用另一已验证组件中实际保留的许可文件，字段为 `component`、`path`、`registryChecksum`；校验和必须对应该 crate 的 Cargo.lock 条目。引用路径必须是该组件保留的非空 LICENSE/COPYING/COPYRIGHT/NOTICE 等文件。此映射保留来源关系，许可证适用性仍属于发行审核。

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

Linux AppImage runtime 除自己的固定源码外，必须包括 `appimage-musl`、`appimage-fuse`、`appimage-squashfuse`、`appimage-zstd`、`appimage-zlib`，并在对应 runtime 构建记录中绑定这些真实静态输入。上游 runtime 的 31 KiB 级 Git 源档案并不等于这些嵌入库的完整源码。没有匹配官方 runtime 资产的原始构建及依赖材料时，完整性保持 false；不能从 URL 推导内部版本或声称重新构建结果等于原始发布资产。

最终 `deployment.inventory` 必须是 `inventory` 子命令对**实际最终部署目录**产生的全文件清单，并在收集时再次核对。目录不能为空，须有真正的 huhucapture PE/ELF、匹配的嵌入身份和旁置 `build-identity.json`。工具复用 `build/package/metadata.py` 的实际运行目录审计；缺少 Qt、许可证或安装标记等会留下阻塞缺项。默认可执行路径为 Windows 的 `huhucapture.exe` 或 Linux 的 `bin/huhucapture`；完整 AppDir 可显式设置 `deployment.executable: "usr/bin/huhucapture"`，DEB 解包目录可指定其真实 `opt/.../bin/huhucapture` 路径。产品可执行文件必须映射到包含项目源码身份的实际应用构建记录。`deployment.sourceMap` 按相对路径映射每个文件：

```json
{
  "lib/libavcodec.so.63": {"receipt": "ffmpeg-linux", "output": "lib/libavcodec.so.63"},
  "share/huhucapture/licenses/LICENSE": {"component": "huhucapture", "path": "LICENSE"},
  "lib/libexample.so": {"linkTarget": "libexample.so.1"}
}
```

直接复制的文件必须与指定源码文件字节相同；生成的程序、库、翻译或资源必须与构建记录的输出字节相同。三种映射形式互斥，不能同时填 `component` 和 `receipt` 让产品检查与实际字节验证走不同分支。链接必须与实际安全链接目标一致。所有文件都需要映射，不能以扩展名、system 或 unknown 标签绕过检查。完成身份、目录及映射验证后再次计算整个部署清单；中途变化会失败。DEB/AppImage 的部署器可能改写 ELF 或启动器，因此应在实际最后一次变换后记录真实输出和变换参数，不能用 SDK 中变换前的哈希代替。

## CI 接入顺序与当前边界

1. 同 SHA 干净检出并记录版本、主依赖锁和 Cargo.lock。依赖来源按照固定锁准备；保留构建器使用的真实上游目录和补丁状态，不在任务结束前删除。
2. 保留完整 vendor、原始注册表包、实际固定工具链源码、真实 Qt 来源/SBOM及许可。Linux 还要按最终部署字节查清 OpenSSL、ICU、其他 native 库和 AppImage 内嵌依赖的实际源版本。宿主 compositor、PipeWire daemon、glibc loader 和驱动不因开发 SDK 包含它们就自动成为产品部署输入。
3. 用第一次收集结果取得精确源码身份；构建任务记录真正执行的参数、工具、配置/链接证据、静态输入与输出。任何源码或配置变化都必须重新匹配记录，不能继续使用旧哈希。
4. 对每种实际最终包目录生成完整文件清单和映射；保存部署工具的实际变换记录，再收集第二次。存在缺项时上传源码档案和 `gaps` 作为开发证据，保留正式分发阻塞。
5. 材料齐全的发行任务使用 `--require-complete`。将实际档案与 `source-manifest.json` 传给 `package-qt.py` 的 `--source-bundle`、`--source-manifest`；身份/档案描述符/组件家族格式兼容现有消费者。Windows 与 Linux 分别绑定自己的实际输入，不用一端的清单证明另一端构建。

目前没有通过此工具产生并审核一份实际双平台完整对应源码包。完整原始 Qt/发行版/Rust/runtime 源码和匹配构建记录未全部供应时，工具应报告未完成；这是预期行为。它不解除 P11/P12 的正式发行、干净机器安装、实际桌面和长期录制验收门槛。

### 保留现有 Linux 应用 CI 的真实来源

`scripts/retain-development-sources.py` 是当前完整 Linux 应用 job 的固定编排入口，调用上面的收集器。它不负责下载或构建依赖，也不提供任意组件/profile 扩展。应在 `build-native-rust.py`、`build-portal.py`、`build-pipewire.py`、`build-software-media.py` 已成功之后运行；可以安排在应用测试和打包之后、最终上传之前，使前面的测试失败也不丢失已经取得的来源材料。

```sh
python scripts/retain-development-sources.py \
  --qt-prefix "$QT_ROOT_DIR" \
  --source-commit "$GITHUB_SHA" \
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

原始构建记录以校验后的副本参与后续判断，并作为 `materials` 保存，不改写成严格构建 receipt。脚本核对四个 native 记录的固定提交、补丁和已有配置字段，以及 Rust 构建记录中的锁、源码文件和实际静态库哈希。Qt SBOM 只作为来源材料，不被声明为 Qt 完整源码。

Cargo 的 metadata、锁、原始档案及 vendor 是四个分别记录的集合。脚本遍历锁中的全部 registry 包，从实际 metadata 路径推导本次 cache 目录，只保留整个 `.crate` SHA256 与 Cargo.lock 一致的公开 registry 档案。找不到的包进入 `missingRegistryArchives`；字节不符直接失败。缺少某个 metadata 条目不意味着该包一定不在缓存中，metadata 里有某个条目也不等于已经提供其原始档案。

gifski 的实际 checkout 先使用收集器已有的 Git 对象与逐字节检查，只额外识别 Cargo 自己的 `.cargo-ok` 完成标记，其他未记录源码仍会拒绝。之后从这个已核对的本地 checkout 建立干净对象副本，禁用 hooks、fsmonitor、签名自动展示、外部 Git 配置及懒加载；不访问远端。实际原始 checkout 不会被清理或重置，最终由主收集器再次核对固定源码。

vendor 使用确实已安装的 Rust/Cargo 1.97.1，先记录实际程序版本与哈希，然后执行 `vendor --locked --offline --versioned-dirs`。所有相关子进程都设置 `RUSTUP_AUTO_INSTALL=0` 和 `CARGO_NET_OFFLINE=true`。一次性私有 cache 只复制本次 registry index、对应 gifski Git database 和已验证的锁定 `.crate`；不复制整个 Cargo home、配置或 credentials，完成后删除此次临时 cache。

vendor 的真实退出码、命令及 stdout/stderr 都会保留。退出码为 0 才把实际目录和配置传给 `rustVendor`；完整锁覆盖仍由主收集器逐项检查。vendor 非零时保留已验证的原始 `.crate` 和原始日志，省略 `rustVendor`，继续生成明确缺项的档案。安装工具链、联网补齐、生成虚假 vendor/checksum 或把失败改成成功都不属于此 profile。

新输出目录包含：

| 路径 | 含义 |
| --- | --- |
| `bundle/` | 主收集器真实生成的源码档案与 `source-manifest.json`；本 profile 要求实际结果为 `complete: false` |
| `collection-plan.json` | 实际提供给收集器的固定来源计划 |
| `cargo-coverage.json` | 完整锁与实际 metadata 集合、原始档案及缺项、vendor 真退出码和实际输出包集合 |
| `retention-report.json` | 实际 checkout SHA、执行命令、退出码、材料哈希、最终失败原因或未完成归档结果 |
| `logs/`、`vendor-config.toml` | 实际命令输出；vendor 失败时 stdout 文件可能为空或不构成配置 |
| `materials/` | 复制并校验后的原始记录及 Qt SBOM |
| `inputs/registry-cache/` | 仅发布校验成功的原始 `.crate`，失败的复制不会冒充已验证材料 |
| `inputs/gifski/` | 核对过的本地干净 checkout，供主收集器导出 |

上传应使用 `if: always()`，保留 `bundle/`、两份报告、计划、日志、`materials/` 和 `inputs/registry-cache/`。需要保留 collector 失败时的 gifski 源文件时也可上传 `inputs/gifski/`，排除其 `.git/` 管理目录。`inputs/rust-vendor/` 是中间产物，vendor 非零时可能只写入一部分；有效源码以主收集器完成的档案为准。

本 profile 故意不供应严格 build receipts、最终部署映射、Rust 标准库及完整 Qt/发行版/AppImage 来源闭包，因此正常产物仍未完成。vendor 缺缓存是可记录的材料缺口；源码、锁、已有档案或主收集器发现真实矛盾时，脚本仍以非零退出，已经校验的输入及诊断文件留在新目录。它不把一次离线编排或组件检查表述为完整应用流水线、源码完备性或发布验收通过。

## 自动化验证

```sh
python3 tests/source_bundle_tests.py -v
```

测试使用真正的临时 Git 仓库、Git 补丁、源码 tar、Cargo registry 形式的原始档案和真实文件，不访问网络，不操作产品工作树。覆盖固定对象与许可证导出、索引/refs 不变、未记录修改/删除/未初始化子模块、篡改 vendor 后重写校验清单、路径及链接链安全、确定性归档、未完成状态与覆盖拒绝，以及 fsmonitor 不执行、保留后原输入被改变、矛盾配置参数和空/错误部署目录。Linux 的输出绑定测试用实际 `cc` 编译带真实嵌入身份的小型元数据探针并验证篡改检测；它不会被认定为完整产品，运行目录审计仍明确失败。缺少编译器会使该测试失败。普通 Windows 环境明确跳过这一 Linux ELF 检查以及需要 Unix 可执行脚本/符号链接权限的构造测试，其余 Git、原始档案和清单检查继续执行。

这些测试证明收集和绑定规则，不是一次产品源码完备性、实际 Cargo 工具链重做或正式许可验收。实际固定 Cargo 重做及完整平台源码包必须另外保留真实执行证据。
