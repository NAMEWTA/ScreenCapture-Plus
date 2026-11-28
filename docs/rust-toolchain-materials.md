# Rust 标准库源码与工具链通知

项目的 Cargo vendor 记录原生聚合库的依赖图。它不包含已编译进程序的 Rust 标准库，也不能替代标准库源码。`scripts/collect-rust-toolchain-materials.py` 单独收集实际锁定工具链的标准库分发源、目标库记录和原始通知，复用现有 `qtMigration.rustToolchain`，没有第二个 Rust 版本配置。

## 真实 runner 上的入口

在同次原生聚合库构建后，使用官方 rustup 安装该固定工具链的 `rust-src`，然后执行收集器。组件安装是明确的 CI 准备步骤；收集器本身只读 sysroot，不隐式安装、更新或修复工具链。

```sh
rust_version="$(python3 -c 'import json; print(json.load(open("build/dependencies.lock.json"))["qtMigration"]["rustToolchain"])')"
rustup component add rust-src --toolchain "$rust_version"
python3 scripts/collect-rust-toolchain-materials.py \
  --native-receipt artifacts/native-rust/build-info.json \
  --output artifacts/runtime-materials-rust-toolchain \
  --source-output artifacts/corresponding-sources-rust-toolchain
```

Windows 使用同一 Python 入口，工具链版本可从现有 JSON 读取：

```powershell
$rustVersion = (Get-Content build/dependencies.lock.json -Raw | ConvertFrom-Json).qtMigration.rustToolchain
rustup component add rust-src --toolchain $rustVersion
if ($LASTEXITCODE) { exit $LASTEXITCODE }
python scripts/collect-rust-toolchain-materials.py --native-receipt artifacts/native-rust/build-info.json --output artifacts/runtime-materials-rust-toolchain --source-output artifacts/corresponding-sources-rust-toolchain
if ($LASTEXITCODE) { exit $LASTEXITCODE }
```

两个输出必须是不同的新空目录。恢复同一已验证静态库时，可显式指定 `--native-library`；静态库的完整哈希仍须与未改写的原 receipt 一致。已有输出、输入不匹配或资料缺失会失败，不能复用部分旧结果冒充本次收集。

## 证据链与实际检查

收集器先复用 `build-native-rust.py` 的源码指纹规则，核对本次原生聚合库、Cargo.lock、固定工具链版本和目标。随后记录真实 `rustc -Vv`、sysroot、`--print target-libdir`、`rustup which` 和已安装组件列表。目标仅接受本项目实际构建的 Windows x64 / Linux amd64；工具链目录和真实编译器必须一致。

原 `lib/rustlib/multirust-channel-manifest.toml` 原字节另行保存。收集器从这个真实 manifest 选择该版本、该 target 的 `rust-std`、`rustc` 以及通用 `rust-src`，只接受官方 `static.rust-lang.org` 日期路径与明确 SHA256。下载复用项目已有的有界 HTTPS / SHA256 实现。rustup 会解析并重新序列化 manifest，可能不保留上游的 `git_commit_hash`；收集器不编造该字段，而是核对每个官方分发档案里的完整 `git-commit-hash` 和 `version` 必须与实际 `rustc -Vv` 相同。

Python 标准库 `tarfile` 仅顺序读取成员。收集器不运行归档里的 `install.sh`，也不调用通用提取功能；重复路径、越界路径、链接、特殊文件、超预算或截断内容均拒绝。

| 实际组件 | 完整字节核对与保留范围 |
| --- | --- |
| `rust-src` | 核对已安装标准库源码树的每一个文件，并检查其文件集合与原档案完全一致；保留整份原 `.tar.xz`、源码清单、原 vendor 构建配置和源内通知。 |
| `rust-std` | 核对真实 target lib 目录的全部文件；缺失、额外、其他 target 或字节不同均失败；记录每个原始 rlib / 相关目标文件的大小与 SHA256。 |
| `rustc` | 核对实际编译器文件与其原分发字节；核对并保留顶层 `COPYRIGHT.html`、`COPYRIGHT-library.html` 及整个 `share/doc/rust/licenses/` 原始许可集合。 |
| 分发元数据 | 保留每个官方分发档案的版本、完整 commit、版权和许可 overlay，以及原档案实际携带的构建配置等顶层元数据。 |

固定 Rust 1.97.1 的官方 `rust-src` 构建会复制 `library`、`src/llvm-project/libunwind`，并使用官方 Cargo vendor 收集标准库 workspace 的依赖。这里直接保留该官方结果，没有再次解析或手工挑选一组“看起来像标准库依赖”的 crate。标准库专用 `COPYRIGHT-library.html` 同样使用官方生成的原文，不用项目自身 Cargo metadata 代替。

## 输出与发行边界

普通应用包只加入 `notices/` 和 `rust-toolchain-materials.json`。不要把 sysroot、Rust 编译器或验证下载目录装进用户的应用目录。

运行时通知使用 `rust-toolchain/<组件>/<原归档路径哈希前16位>/<原文件名>` 的紧凑目录。
报告逐项保存相对于原分发包根目录的 `archivePath`、安装相对路径、完整 SHA256
和大小；原文件名、许可全文和作者声明均保持原始字节。不同来源路径不会因同名
通知而合并，独占创建也会拒绝目标冲突。完整源码仍保留原档案的原始目录结构。
这避免把标准库 vendor 的深层源码目录复制到普通运行目录，造成 Windows 安装
路径超限；不要求用户修改系统长路径策略，也不通过减少通知集合解决问题。

完整官方 `rust-src` 压缩档案、`rust-src-files.json`、分发顶层元数据和 `rust-toolchain-sources.json` 放在 `--source-output` 中，作为独立源码产物保留。`--output/evidence/` 含原 receipt、原 rustup manifest、命令输出和用于字节比对的官方分发档案；后两份二进制档案只是验证输入，不是应用运行依赖或新增源码。

报告中的状态范围如下：

| 字段 | 为真时实际证明的范围 |
| --- | --- |
| `standardLibraryInputBindingComplete` | 实际工具链、目标标准库与官方 source/std/compiler 分发身份及所检查的原始字节一致。 |
| `standardLibrarySourceMaterialsCollected` | 完整官方标准库源码档案和对应清单已真实保存。 |
| `upstreamNoticeSetComplete` | 本步骤要求的官方标准库 / 编译器通知集合已从真实分发输入核对并保存。 |
| `correspondingSourceComplete` | 此收集器始终保持 `false`；项目全部依赖、构建配置、其他库及最终 payload 的源码闭合仍由发行门槛独立核实。 |

`rust-src` 是标准库源码分发，不是整个 Rust 编译器源码。报告明确保留 `compilerSourceStatus`，不把组件源码存在、相同版本号或一个成功下载当作整个应用已具备完整对应源码。Windows 和 Linux 必须各自在实际 runner 上核对目标二进制；一端结果不能替代另一端。

## 已执行的验证范围

提交 `659cc9d7f882e3fd1d92cdb6f2360ed2c31ab735` 的
[完整应用 CI](https://github.com/NAMEWTA/huhucapture/actions/runs/45649036685)
已在 Windows 和 Linux 各自核对原 Rust 1.97.1 分发与实际 sysroot：两端各有
3,560 个标准库源码输入；目标库输入分别为 45、62 个；编译器与通知输入各
15 个。每端均保留 `rust-src` 75 份、`rust-std` 3 份、`rustc` 17 份原通知。
前三个范围字段为真，全应用 `correspondingSourceComplete` 仍为假。

该轮 Windows 安装在一个完整路径为 263 字符的深层通知文件处中断。紧凑目录
修正已使用该轮两端六份真实分发档案重放通知读取，190 份通知逐项与原报告
字节一致；在同一 Windows 安装目录下最长路径降为 206 个 UTF-16 代码单元。
这是原通知与新布局的核对；修正后的完整收集、NSIS 安装和卸载仍待原生 CI。

官方依据：[rustup components](https://rust-lang.github.io/rustup/concepts/components.html)、[Rust 1.97.1 标准库与 rust-src 分发构建](https://github.com/rust-lang/rust/blob/1.97.1/src/bootstrap/src/core/build_steps/dist.rs)、[分发 overlay 与 commit 元数据](https://github.com/rust-lang/rust/blob/1.97.1/src/bootstrap/src/utils/tarball.rs)、[官方版权清单生成](https://github.com/rust-lang/rust/blob/1.97.1/src/bootstrap/src/core/build_steps/run.rs) 和 [rustup manifest 读写结构](https://github.com/rust-lang/rustup/blob/1.29.0/src/dist/manifest.rs)。
