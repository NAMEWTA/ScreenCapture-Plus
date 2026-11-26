# Linux Qt 与 ICU 的实际源码构建

`scripts/build-linux-qt.py` 在真实 Linux amd64 主机上，从固定的原始源码构建
ICU 73.2 和 Qt 6.12.1 的五个模块，写入一个全新的共享库前缀。该前缀有自己的
真实配置、工具、执行日志、安装文件和来源记录；它不是对原厂 SDK 字节的重现。
原 SDK 继续作为版本、配置和翻译比较的只读输入。

## 构建输入与主机准备

Qt 模块使用主依赖锁中的既有固定 commit。ICU 使用官方
[`release-73-2`](https://github.com/unicode-org/icu/tree/680f521746a3bd6a86f25f25ee50a62d88b489cf)
源码，完整 commit 和轻量 tag 对象均为
`680f521746a3bd6a86f25f25ee50a62d88b489cf`。恢复时实际读取远程 tag，验证它
确实指向锁定 commit，不在本地创建一个 tag 来代替上游证明。ICU 版本继续为
原 SDK 使用的 73.2；不能用发行版的另一个 ICU 版本满足这项输入。

Ubuntu 的原生开发依赖包括下列包；应用、Portal、媒体和安装旅程原有的测试与
运行依赖仍需保留。这是按 Qt 固定源码的依赖探测和
[官方 Linux 要求](https://doc.qt.io/qt-6/linux-requirements.html)准备的安装清单，
不是这些包已经安装或完整源码构建已通过的声明。

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential git cmake ninja-build pkg-config binutils \
  libdbus-1-dev libglib2.0-dev libglib2.0-dev-bin libatspi2.0-dev \
  libssl-dev zlib1g-dev libzstd-dev libbrotli-dev \
  libfontconfig1-dev libfreetype-dev libgtk-3-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxrender-dev libxfixes-dev libxi-dev \
  libsm-dev libice-dev \
  libxcb1-dev libxcb-cursor-dev libxcb-glx0-dev libxcb-icccm4-dev \
  libxcb-image0-dev libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render0-dev \
  libxcb-render-util0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev \
  libxcb-util-dev libxcb-xfixes0-dev libxcb-xkb-dev libxcb-xinerama0-dev \
  libxkbcommon-dev libxkbcommon-x11-dev \
  libgl-dev libegl-dev libgbm-dev libdrm-dev libvulkan-dev \
  libwayland-dev libwayland-bin wayland-protocols
```

Python 工具必须包含 REUSE 的字符编码支持。Qt 自身生成并验证 SPDX 2.3 JSON，
缺失依赖时构建失败，不跳过来源验证：

```sh
python -m pip install spdx-tools==0.8.5 'reuse[charset-normalizer]==6.3.0'
python scripts/restore-qt-module-sources.py \
  --platform linux --workspace .deps/qt-module-sources
python scripts/build-linux-qt.py \
  --source-workspace .deps/qt-module-sources \
  --reference-sdk "$QT_ROOT_DIR" \
  --workspace .deps/qt-runtime-linux --jobs 2
```

`--reference-sdk` 指向保留了原始 SPDX、`config_qtbase.opt` 和
`config_qtbase.summary` 的原 Linux SDK。`--workspace` 必须是尚不存在的独立
目录，不能嵌套在源码或原 SDK 中，也不能包含它们。失败后的目录和日志保留；
下一次执行必须选择另一个新目录，不能覆盖原记录。

Qt 模块、Qttools 与 ICU 共用
[原始 Git 字节检出配置](windows-qt-source-build.md#原始-git-字节的检出配置)。
`core.autocrlf=false` 本身不会覆盖上游 `eol=crlf`，Linux 上同样可能转换 ICU 的
`vendor/double-conversion/upstream/msvc/testrunner.cmd`。恢复入口在新空仓库
第一次 checkout 前安装 `.git/info/attributes`，关闭 text/eol/filter/ident/
working-tree-encoding 内容转换，原 LF 和原 CRLF 分别保持原始字节。实际
producer records 保留 policy 和 helper，构建前后核验且不修补旧输入；严格的
Git blob 比较仍保持不变。这是明确记录的新源码构建配置，不代表重现原 SDK。

Qttools 同时通过共享恢复入口读取固定父 tree 中的 qlitehtml gitlink 和原始
`.gitmodules`，逐层恢复官方子仓库、保持原始字节并保留 Git 对象与真实命令记录。
嵌套子提交来自各级原 tree；其他地址或不完整 checkout 失败。关闭 Assistant
不省略这部分完整来源，详见 [Windows 入口的共享源码恢复规则](windows-qt-source-build.md#固定输入和真实执行)。

## 六个真实来源生产者

| 顺序 | 固定源码 | 主要实际输出 |
| --- | --- | --- |
| 1 | `qt.sourceDependencies.icu` | ICU 73.2 的 uc、i18n、data 共享库、头文件和 pkg-config 信息 |
| 2 | `qtbaseRepository` / `qtbaseCommit` | Core、Gui、Widgets、DBus、Network、OpenGL、XCB/Wayland 客户端、TLS 与原生工具 |
| 3 | `qt.sourceModules.qtsvg` | SVG 模块与图像插件 |
| 4 | `qt.sourceModules.qtwayland` | Wayland compositor 模块及其实际安装输出 |
| 5 | `qt.sourceTools.qttools` | 官方 Linguist 工具及 CMake API，包括 lrelease 和 lconvert |
| 6 | `qt.sourceModules.qttranslations` | 用新构建工具从原始 TS 生成的 QM 与目录元数据 |

ICU 按[官方独立构建目录方式](https://unicode-org.github.io/icu/userguide/icu4c/build.html)
执行 `icu4c/source/runConfigureICU Linux`，固定共享库、关闭静态库、测试和示例，
实际 `make` 后安装到同一个新前缀的 `lib/`。关闭测试是构建范围说明，不是 ICU
测试已经通过。每一步退出码、stdout/stderr、工具身份和实际生成的 Autoconf/
Make 配置文件都进入它自己的 `NativeBuild` 记录。

Qt 使用 GCC/G++、Ninja、`RelWithDebInfo`、共享库与 `lib/` 安装目录。Widgets、
XCB、Wayland、DBus、桌面 OpenGL、OpenSSL runtime 和 ICU 必须启用。原 SDK
的文本/图像库选择继续保留：JPEG、PNG、PCRE、HarfBuzz、DoubleConversion
使用 Qt 内置源码；zlib、FreeType、Fontconfig 使用系统库。Qttools 只请求
Linguist，不引入 QDoc、Designer、Assistant 等额外应用。

配置前清除旧 Qt 的 CMake、host、插件和工具链覆盖，以及继承的 loader、头文件、
库路径、pkg-config sysroot 和编译器参数。新的 ICU include 和三个库路径显式
传给 CMake。配置结束后读取真实 `CMakeCache.txt`，核对必须启用的 feature、
版本化 ICU 73.2 文件及所有后续模块的 `Qt6*_DIR` 都指向新前缀；不能只因为
命令行写了 `ON` 就记录相应能力已生效。

新构建仍会与原 SDK 的厂商编译器、构建路径和部分可选系统集成不同。报告保留
实际 feature cache 和这些差异，不把当前配置伪称为旧 SDK 配置。五个 Qt 模块
均启用 Qt 原生源 SPDX 生成及验证，并逐文件核对安装输出与生成 SPDX 的完整
SHA1。ICU 使用自己的真实源码构建 receipt；不捏造 Qt SDK 的 ICU SPDX 来源。

## 输出验证与下游使用

最终核对实际 amd64 ELF 库、XCB/Wayland/OpenSSL/SVG 插件、原生工具和产品使用
的翻译文件。`readelf` 必须看到 QtCore 依赖 `libicuuc.so.73` 和
`libicui18n.so.73`；对 QtCore、XCB、Wayland 和 OpenSSL 插件运行 `ldd`，
不得缺少依赖，所有解析到的 Qt/ICU 库都必须来自新前缀。随后真实运行 qmake6、
lrelease、lconvert。工具运行和链接检查不能代替应用 GUI、Portal 授权、采流
或录制测试。

只有脚本成功后，后续应用构建与打包才切换到新前缀。例如在同一 shell 中：

```sh
qt_source_prefix="$PWD/.deps/qt-runtime-linux/install"
export QT_ROOT_DIR="$qt_source_prefix"
export CMAKE_PREFIX_PATH="$qt_source_prefix"
export PATH="$qt_source_prefix/bin:$PATH"
export LD_LIBRARY_PATH="$qt_source_prefix/lib"
export PKG_CONFIG_PATH="$qt_source_prefix/lib/pkgconfig"
```

应用所需的 Portal 和媒体依赖前缀再按原入口追加；旧 Qt SDK 前缀不得重新加入。
Linux 上也可对新前缀分别构建应用 Debug 和 Release，但这不把此 Qt 依赖构建
描述成两套不同配置的 Qt 库。

必须保留以下输出，并在完整应用来源汇总时使用实际 fragment：

- `.deps/qt-runtime-linux/build-info.json` 与 `control/`；
- 六个 `*-source-receipt/` 中的原始执行、配置、源清单和输出哈希；
- `.deps/qt-runtime-linux/source-plan-fragment.json`，直接引用六份原 receipts；
- 新 `install/` 前缀、固定原源码检出和原 SDK 证据，源码归档排除 Git 管理目录。

只有全部真实命令、配置、SPDX、ELF、加载与工具检查成功，报告的 `buildComplete`
才为 true。`correspondingSourceComplete` 始终 false；应用、Rust 与标准库、
静态依赖、系统库、最终安装包变换等整体闭包仍由
[主来源收集器](source-bundles.md)判断。新翻译与原 SDK 的逐字节比较单独报告，
不同或缺失的文件不会自动归类为原 SDK 已重现。

离线检查 `python tests/linux_qt_builder_tests.py` 覆盖固定 ICU 输入、新旧路径隔离、
实际配置缓存拒绝、五模块 SPDX 完整哈希和非 ELF 输出拒绝。它不编译 Qt/ICU，
也不能生成可供发行的成功凭据。真实原生 source build、完整应用和安装旅程的
CI 证据仍是继续合并和发布的必要输入。
