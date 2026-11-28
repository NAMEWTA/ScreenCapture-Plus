# Windows Qt 的实际源码构建

`scripts/build-windows-qt.py` 在真实 Windows x64 / MSVC 环境中，从主依赖锁的
固定源码生成独立 Qt 前缀。它用于解决已保留的 SDK 签名插件与原 SPDX 完整哈希
不能对应的问题，不修改原 SDK、签名或 SPDX，也不把新构建称为原厂资产的逐字节重现。

原始 `qwindows.dll`、`qschannelbackend.dll`、`qtuiotouchplugin.dll` 经 Windows SDK
SignTool 实际移除签名后，完整 SHA1 仍不匹配原 SPDX。官方
[SignTool `/s`](https://learn.microsoft.com/windows/win32/seccrypto/signtool)
承诺删除签名，没有承诺恢复原签名前全部 PE 字节。因此旧 SDK 与签名检查材料继续
保留，新前缀从真实源码重新构建并产生自己的来源记录；不忽略 checksum、证书目录
或任意文件区间来获得通过结果。

## 固定输入和真实执行

先在初始化过 MSVC v143 x64 的原生 runner 中，安装 CMake、Ninja、Python 和
`spdx-tools`、`reuse`。Qt 自己通过这些原生支持的工具生成源 SPDX 和验证 SPDX
2.3 JSON；此入口要求生成及验证依赖存在，缺失时失败，脚本不临时安装工具。
当前可明确固定 `spdx-tools==0.8.5`、`reuse[charset-normalizer]==6.3.0`，实际版本、程序及 Python
文件身份会进入原执行记录。

保留 `install-qt-action` 得到的原 Windows SDK，仅将其路径传给
`--reference-sdk`。原 Qt 模块检出继续使用已有恢复入口；所有构建输出进入另一个新目录：

```powershell
python -m pip install spdx-tools==0.8.5 "reuse[charset-normalizer]==6.3.0"
python scripts/restore-qt-module-sources.py `
  --platform windows --workspace .deps/qt-module-sources
python scripts/build-windows-qt.py `
  --source-workspace .deps/qt-module-sources `
  --reference-sdk "$env:QT_ROOT_DIR" `
  --workspace .deps/qt-runtime-windows --jobs 2
```

新的实际前缀为 `.deps/qt-runtime-windows/install`。只有脚本完整成功之后，后续
应用配置、Qt 部署及原生运行检查才能使用这个前缀；不能只把旧 SDK 中三个插件的
文件名替换成新库。完整应用 CI 先执行该构建，成功后才将后续配置、部署、SDK
字节校验、TLS 与安装 GUI 旅程全部切换到新前缀。独立组件回归仍可使用原厂 SDK，
它的结果不能代替完整应用针对新前缀的验证。正式包还须完成组合来源收集。

构建顺序和来源为：

| 顺序 | 原始源码 | 实际产物与作用 |
| --- | --- | --- |
| 1 | 主锁 `qtbaseRepository` / `qtbaseCommit` | Core、Gui、Widgets、Network、Schannel、Windows/generic 插件及原生部署工具 |
| 2 | `qt.sourceModules.qtsvg` | 与同一 qtbase 前缀链接的 SVG 模块与插件 |
| 3 | `qt.sourceTools.qttools` | 官方 Linguist 目标、lrelease、lconvert 和其 CMake API |
| 4 | `qt.sourceModules.qttranslations` | 用新构建的官方翻译工具生成并安装 `.qm` 及目录元数据 |

Qttools 源由新空 workspace 中的受控 Git 操作取得，完整 commit 与官方 release tag
对象均须匹配主锁。qtbase、qtsvg、qttranslations 输入检出必须原本干净、HEAD 与
origin 精确匹配；脚本不 reset、clean 或修补这些原输入。所有 source build receipts
使用同一 `source_receipts.py`，在配置前和构建结束后独立核对源树。

Qttools 的固定 tree 还包含 `src/assistant/qlitehtml` gitlink，指向
`180e3ebb22bb8d82a9bfe88bba46da8c28c9f2a2`。关闭 Assistant 构建不改变完整
原始源码的范围。[该提交的原 `.gitmodules`](https://github.com/qt/qttools/blob/2bcebd46216b38bf66775a5d42b7f72c6501e686/.gitmodules)
指定 `https://code.qt.io/playground/qlitehtml.git`。
`build/package/source_submodules.py` 在 Qttools 第一次检出后，读取同一固定
父提交的原 gitlink 和 `.gitmodules` blob，恢复其原始子仓库。后续子提交仍只能
来自已固定父 tree；不跟踪分支，也不预填尚未读取的子提交 SHA。

允许的官方仓库关系仅为 Qttools → qlitehtml →
`https://code.qt.io/qt/qttools-litehtml.git`；后一地址也见
[Qt 开发者对官方仓库的说明](https://lists.qt-project.org/pipermail/interest/2025-December/040397.html)。
实际 `.gitmodules` 出现其他地址、未声明的 gitlink、重复路径或不匹配的
checkout 时立即失败，不能用任意同名镜像代替。原生 runner 留存实际 commit /
tree 原始对象、完整 gitlink 列表、`.gitmodules` 原件、逐层 fetch / checkout
命令和结果。每个新子仓库在首次 checkout 前安装同一原始字节 policy，并在
整个构建结束后再次验证；既有目录或输入不会被覆盖。

离线 `tests/source_submodules_tests.py` 用真实 Git 对象和本地 fixture transport
验证多层固定提交恢复、原 LF/CRLF、完整 source export、原始证据留存，以及
地址、commit、policy 和既有输入异常时的拒绝。它不代表官方网络恢复或 Qt
原生编译已经通过；实际官方子树、嵌套对象和工具运行结果由 CI 保留。
标准相对 URL 按 [Git 的仓库目录规则](https://git-scm.com/docs/git-submodule)
解析后仍须精确匹配这些官方地址，原地址与解析结果同时保留；上游 branch 字段
仅作原始元数据记录，不能代替 parent tree 中的完整 commit。

### 原始 Git 字节的检出配置

此源码构建采用 `original-git-blob-bytes-v1`：在全新独立 Git 仓库第一次
checkout 之前，`build/package/source_checkout.py` 写入受控的
`.git/info/attributes`，内容为：

```gitattributes
* -text -eol -filter -ident -working-tree-encoding
```

仅设置 `core.autocrlf=false` 不足以保存原始字节；上游 `.gitattributes` 中的
`eol=crlf` 仍会转换文件，例如 Qt 的 `bin/qmake-and-qtpaths-wrapper.bat.in`。
Git 官方文档说明
[`info/attributes` 具有最高属性优先级](https://git-scm.com/docs/gitattributes)。
这里显式禁用工作树内容转换，使原 LF 和原 CRLF 都保持其 Git blob 字节，
与 `git archive` 的原始文件内容一致；不是将所有文件转换为同一种换行。

该 helper 拒绝已检出的工作树、非空目录、已有 policy 或链接到别处的 Git
管理目录，不修补旧 `.deps`。它不改上游 `.gitattributes`、Git index、commit
或 source hash。各真实 Qt/ICU producer 保留 policy 和 helper 的原字节，
构建前后再次核验 policy。原 source collector 的逐文件 Git 字节检查保持不变；
源码或 policy 被改动仍然失败。新构建报告明确记录此配置与原厂 SDK 的差异，
不将其宣称为原厂 SDK 的逐字节复刻。

`tests/source_checkout_tests.py` 用真实 Git 仓库验证普通 checkout 的转换失败、
原 LF/CRLF 与 `.bat.in`/`.cmd` 的完整保留、干净状态和 index/refs 不变，以及
ident、encoding、filter 与篡改拒绝。这些合同不代表原生 Qt 已重新编译成功。

固定 Qt 源表明 `windeployqt` 已属于
[qtbase 的 tools](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/tools/CMakeLists.txt)，
不是为该工具引入另一套 Qttools。Qttools 的
[lrelease 原始构建定义](https://github.com/qt/qttools/blob/2bcebd46216b38bf66775a5d42b7f72c6501e686/src/linguist/lrelease/CMakeLists.txt)
与 [qttranslations 的原始生成/安装规则](https://github.com/qt/qttranslations/blob/201bdab42f65751f1392b38a5f64ecccc9a0657d/translations/CMakeLists.txt)
直接参与真实构建，没有项目自写的 QM 转换器或改写输出校验和。

## 配置、来源和输出边界

使用原生 `Ninja Multi-Config`，实际构建、安装 `RelWithDebInfo` 与 `Debug`，明确
动态 CRT `MD` / `MDd`。Windows 产品需要的 Widgets、Network、图像、无障碍、
线程、OpenGL、Schannel 和 windeployqt 必须启用。原 SDK 的 Qt 配置说明、SPDX
和实际存在的 `config_qtbase.opt/summary` 原件进入记录。新构建采用真实本地工具
路径、输出前缀与源码配置，不能把厂商的旧 CMake/编译器路径说成当前执行命令。

Qt 内置图像/文本依赖继续使用固定 qtbase 内的真实源码。此 Windows TLS 路径使用
项目实际部署的 Schannel；不从旧 SDK 的 OpenSSL/PostgreSQL 私有构建路径取输入。
Qttools 只请求 Linguist 所需工具，关闭 QDoc、Designer、Assistant 等无关工具。
环境中旧 Qt 的 CMake 路径、插件路径及 host/toolchain 覆盖不参与新构建，后续模块
只从新前缀解析 Qt；没有向原 SDK 目录写入。

每个模块的配置、构建和安装命令还将该模块的 `build/bin` 放在受控 `PATH`
首位，其后保留新的 `install/bin` 和原有编译器 / 系统路径。Qt 的
[工具输出规则](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/cmake/QtToolHelpers.cmake)
会将非主配置工具放入 `bin/Debug`，而匹配的 `Qt6Cored.dll` 位于 `bin`；
[Windows DLL 搜索](https://learn.microsoft.com/windows/win32/dlls/dynamic-link-library-search-order)
不会自动包含可执行文件的父目录。这样 Debug 的 `rcc` 在其依赖尚未安装时，
可以加载本轮刚构建的库，不需要借用原 SDK 或更改动态 CRT。该路径只作用于
当前模块的真实命令，不修改调用者环境；逐命令执行记录保留实际 `PATH`。
`PATH` 是既有 schema 的可选执行环境字段，只记录本次子进程实际收到的值。
旧记录中缺失的搜索路径保持未知，原 receipt / 执行记录及其哈希保持原样；
环境字段增加本身不改变对应源码完整性的判定。

每个 `<module>-source-receipt/` 保存配置前完整源清单、实际工具身份、命令参数、
逐命令退出码/stdout/stderr、CMake/Ninja 配置和成功后产生的文件哈希。共享前缀
只把本轮新生成或改变的文件计入该模块输出。配置选定的实际安装前缀也必须与
预期目录一致。任何命令非零、原源码变化或安装输出变化均失败，已有日志留存。

结束时逐项核对新安装文件与 Qt 自己生成的四份 SPDX 文档中完整 SHA1，包括
多配置共有文件；不能用新来源记录遮蔽签名后的字节变化。还检查实际 x64 PE
库/工具、两个配置的 Windows/Schannel/generic 插件及产品使用的翻译文件。
最后真实运行 windeployqt、lrelease 的版本命令和 lconvert 的 `-help`；固定
Qttools 源码的 lconvert 不支持 `-version`。此处仅是工具运行，
应用 GUI、TLS、安装与物理桌面能力仍需各自原生测试。

报告记录新 QM 与原 SDK QM 的实际逐字节比较，给出相同和不同/缺失路径；它不会
把新配置生成结果自动标为旧 314 个文件已重现。所有实际生成源码（包括翻译的
English fallback 输入和 catalogs 元数据）均保留在构建证据中。

## CI 保留与验收

完整应用工作流的 Windows job 使用上面入口；新 Qt 的实际构建时间包含在其
240 分钟原生 job 预算中。SPDX 全文件校验和构建命令任一失败，就不会进入应用
构建。后续材料收集同时归档 qtbase、qtsvg、Qttools 和 qttranslations 的原源码，
原字节校验、官方部署重放及完整安装旅程仍执行。

此前签名插件诊断仅针对原厂 SDK。替换输入后它不再作为新 SDK 的生产验证项：
新前缀来自实际 MSVC 构建，无原厂签名，使用 Qt 本次生成的 SPDX 校验每个完整文件。
`probe-qt-sdk-signing.py` 及已有原始证据继续保留，不能据此宣称旧 SDK 问题已修复。

`windows-qt-source.yml` 仍提供独立手动诊断入口。两个入口均保留：

- `.deps/qt-runtime-windows/build-info.json`
- `.deps/qt-runtime-windows/control/`
- `.deps/qt-runtime-windows/*-source-receipt/`
- `.deps/qt-runtime-windows/install/` 中真实前缀（需要下游构建时）
- 固定 Qt 原源码检出/源码档案及原 SDK 来源材料；归档时排除 Git 管理目录。

`buildComplete` 只在所有真实命令、输出、SPDX 和工具检查通过后设为 true。
`correspondingSourceComplete` 始终 false，避免把四模块构建误称为应用整体源码
闭包。应用本身、Rust/标准库、最终包变换、系统库以及 Linux runtime 静态来源
仍由 [主源码收集器](source-bundles.md)独立计算。

本地离线合同 `python tests/windows_qt_builder_tests.py` 覆盖新旧 Qt 路径隔离、固定
来源输入、动态 CRT 配置、完整 SPDX 文件比较和错误路径拒绝。它不编译 Qt，不
证明 Windows 新前缀已经可用；实际原生 CI 构建及完整应用复验仍是必要门槛。

构建方法依据 [Qt Windows 源码构建说明](https://doc.qt.io/qt-6/windows-building.html)、
[Qt configure 与独立输出目录](https://doc.qt.io/qt-6/configure-options.html)及
[Qt SPDX 生成说明](https://doc.qt.io/qt-6/sbom.html)。
