# 原生依赖的对应源码材料

`scripts/collect-native-source-materials.py` 收集当前原构建 runner 上的
libportal、PipeWire、x264 和 FFmpeg 输入。Windows 只收集实际使用的 FFmpeg
检查/封面 DLL；Windows 原有 Media Foundation 编码不由此脚本替代。
这不是新的依赖解析器：仓库、提交、补丁和配置继续只来自
`build/dependencies.lock.json` 与现有四个 builder。

## 运行位置和输入

必须在同一次依赖构建、产品 CMake 安装完成之后，打包改写 RPATH 或 strip 之前运行。
需要保留原 builder 的 source、build、install 目录与原 `build-info.json`。
只恢复开发 SDK 的安装前缀不够，缺少原源码或配置时收集失败。

Linux 原有默认工作目录可直接使用：

```sh
python3 scripts/collect-native-source-materials.py \
  --installed artifacts/install/linux \
  --portal-workspace .deps/portal-validation \
  --pipewire-workspace .deps/pipewire-validation \
  --media-workspace .deps/media-validation \
  --output artifacts/runtime-materials-native-linux \
  --source-output artifacts/corresponding-sources-native-linux
```

Windows 使用同一入口和实际 Windows 安装目录：

```powershell
python scripts/collect-native-source-materials.py `
  --installed artifacts/install/windows `
  --windows-media-workspace .deps/video-inspection-windows `
  --msys C:/msys64 `
  --output artifacts/runtime-materials-native-windows `
  --source-output artifacts/corresponding-sources-native-windows
```

完整应用配置可以是 `RelWithDebInfo`；原生 FFmpeg 输入仍是 builder 真正生成的
`install/Release` / `build-Release`，必须使用动态 `MD` CRT。
脚本用同一 MSYS 的 `cygpath` 读取原 POSIX configure 路径，不手工猜测盘符转换。
它不执行 configure、编译、安装或网络恢复。

两个输出必须是彼此独立的新空目录，不能位于原安装树或任何依赖工作目录内，
也不能包含这些输入目录。失败保留已有输入与已收集证据，不删除旧结果。

## 源码与补丁核验

原 checkout 的 HEAD 和 origin 必须等于依赖锁，原构建 receipt 中的有序补丁列表
必须逐项一致，每个本地补丁的完整 SHA256 也必须匹配。例如旧 FFmpeg receipt
若仅含第一个严格 MOV 补丁，而当前锁还要求 MPEG 音频补丁，就会被拒绝，
不能因为 FFmpeg 版本号相同而沿用。

收集器创建自己的临时 bare Git 仓库，只通过 Git 的共享对象机制读取原 checkout
已存在的对象。它在独立索引中读取固定提交并逐个执行 `git apply --cached`，取得
预期补丁后 tree；第二个独立索引从该 tree 开始，读取原工作目录的实际内容。
新增补丁文件即使匹配 `.gitignore` 也属于预期 tree，不会漏验。

原 source 目录不应承载编译输出，现有 builder 都在兄弟 build 目录编译。
因此源码检查还使用不应用 ignore 规则的 `git ls-files --others`，拒绝所有额外
source 文件，包括被忽略的新头文件。实际 source 如出现新的合法生成文件，必须
先保留失败路径并审查上游生成链，不能加入一个笼统的 ignored-files 豁免。
已有源码的任意未声明修改、遗漏补丁文件或未收集的 submodule 同样失败。

若实际索引与固定补丁树不同，失败时另存
`families/<family>/source-differences.json`：记录组件、Git 版本、实际转换设置、
两棵树标识和差异总数，最多保留前 64 个路径的模式与 blob 标识。它还保留原工作
文件的大小与 SHA256，并对总大小不超过 16 MiB 的普通文件补充原字节的 Git blob
标识，便于区分原内容变更与索引转换；不导出未知改动正文，也不自动接受行尾差异。
此诊断不绕过原树相等门槛，不产生成功源码档案，命令仍返回非零。

只有临时仓库接收新对象、索引和 archive 属性；原工作树、HEAD、config、index
不会被 checkout、reset、clean、add 或 apply 写入。所有原 Git 读取禁止可选锁，
结束时复核原源码逐文件清单和控制文件未变。自定义 clean/smudge filter 或文本
编码转换须另行审查；不因 Windows 全局存在但未使用的 Git LFS 配置而误报。

完整源码由真正的 `git archive` 生成。在临时仓库中关闭 `export-ignore` 和
`export-subst`，随后流式核对档案的每个成员、Git blob SHA1、可执行模式和完整
路径集合，防止 archive 属性省略文件或替换内容。报告同时保留实际工作目录
SHA256 和 Git 的行尾转换设置；Git 的规范源码档案不等于 Windows CRLF checkout
逐字节副本。原许可证与作者通知直接取自已验证 Git blob，不改写原文。

## 真实配置与运行文件

| 原构建 | 收集和核对的材料 |
| --- | --- |
| libportal / PipeWire | 原 Meson 源码/构建/安装目录，完整 `meson-info/*.json`，实际 options、project version、安装映射，`cmd_line.txt`、`build.ninja`、编译命令及实际生成源码 |
| Linux x264 | 原 receipt 的完整 configure 参数、`config.mak` 中的实际源目录/前缀和 shared profile，生成的配置头与 pkg-config 文件 |
| Linux FFmpeg | 原 configure 参数与 `FFMPEG_CONFIGURATION` 一致性，实际源目录/前缀、配置头、组件开关、版本头及生成配置 |
| Windows FFmpeg | 实际 Release/MD 配置、原 `configure-fixed.sh` / `build-fixed.sh`，DLL 和导入库的原构建时 SHA256，以及锁定 NASM/Make 档案标识 |

保留的 builder 和原 lock 字节与材料一起归档，不产生第二份版本源。
Meson introspection 的 compiler 参数是 IDE 提示，不能冒充真实编译命令；材料中
同时保留实际 Ninja 与 `compile_commands.json`。Meson 安装可能改写 RPATH，故不
将 build 目录 ELF 与 install 前缀直接相等作为前提，也不自行编写 ELF 归一化。

Linux 的 CMake 安装树必须先包含实际 x264 运行库。固定 CMake 的依赖解析不会将
应用的 `RUNPATH` 继承给 FFmpeg 的间接依赖；它可能从 `ldconfig` 找到系统 x264，
再由 Qt 的系统路径过滤规则排除。安装脚本因此从与 `libavcodec` 相同的实际
pkg-config 库目录定位 x264，禁止默认系统搜索，在 Qt 部署之后执行独立安装脚本，
并用官方 `FOLLOW_SYMLINK_CHAIN` 保留共享库及版本符号链接链。Qt 生成的部署
脚本及收集器的严格重放合同保持不变。这里不修改 ELF 字节，也不把后续 linuxdeploy 的
补入结果当作此前原生收集已成功。完整大小、SHA256、源码与构建记录仍由同一个
收集器核验；找不到实际运行库仍须失败。

该接线依据固定 [CMake 3.31.6 的 Linux 解析顺序与库链安装规则](https://github.com/Kitware/CMake/blob/v3.31.6/Help/command/file.rst)。
本地使用已构建的真实应用与 SDK 库确认了解析差异；完整原构建 runner 上的收集
结果仍须由修复后的流水线验证，不能由单库安装检查推导四个家族全部完成。

FFmpeg 的固定 configure 会把整个 shell 环境写入 `ffbuild/config.log`，因此此日志
明确不复制。保留受控命令、实际 `config.mak` / 配置头等需要的配置，不将 runner
的完整环境变量作为源码材料发布。其他任意构建日志也不自动打包。

实际产品先通过现有安装树与嵌入构建身份审计，并要求依赖锁 SHA256 相同。
脚本按本项目真实部署范围选出原生库、PipeWire 客户端模块/SPA 插件与
`client.conf`，逐项核对原前缀中的完整大小和 SHA256；文件名只用于定位候选。
Windows 还必须匹配原 builder 写入 receipt 的 DLL/导入库哈希。

Linux 当前 builder 没有记录构建时输出哈希。本阶段证明的是同一个 runner 上，
现存固定源码、原构建记录、真实配置、原安装前缀与产品 payload 的对应关系；
它不是独立签名，不证明前缀文件从未被替换，也没有重新编译以证明可重复性。
报告中的 `buildTimeOutputHashesVerified` 在 Linux 为 `false`，
`reproducibleRebuildPerformed` 始终为 `false`。

## 输出与发行边界

普通运行包只加入 `--output` 下的 `notices/` 和 `native-materials.json`。
`--source-output` 的完整每组件源码 tar、补丁、builder、原 lock/receipt、实际配置、
逐文件来源清单和 `native-sources.json` 作为独立 CI 源码材料产物保存。
报告中源码档案和 configuration 的相对路径以 source output 为根，notice 路径以
runtime output 为根。进入收集后失败时，两个报告保留失败说明，命令返回非零；
安装身份、目录或其他前置审计失败时不创建新的材料结果。

`nativeSourceCollectionComplete` 只表示本入口要求的原生家族已完成源码/补丁/配置
收集；`runtimeInputBindingComplete` 只表示对应实际运行文件完成字节绑定。
两者都不将其他家族的缺口自动补齐。`correspondingSourceComplete` 始终为
`false`：Qt SDK 的 ICU、翻译的精确来源绑定、Rust/工具链、系统 source package、
AppImage runtime、项目与构建工具的完整组合仍须分别证明。

首次本地验证使用真正的 Git 索引、patch、archive 操作检查新增/删除文件、
ignore、export 属性、CRLF 规范化和整个原工作树/Git 文件保持不变；另用真实
已恢复 SDK 的 ELF 检查完整字节绑定及单字节错配拒绝，并用实际旧构建 receipt
核对缺少第二补丁时拒绝。这些是收集合同检查，不代表已在原 Linux/Windows
runner 完成四家族源码收集。完整正向验收必须由上述实际 CI 命令产生材料。

依据为 [Git 的独立索引与只读对象共享](https://git-scm.com/docs/git)、
[Git archive 的属性行为](https://git-scm.com/docs/git-archive)、
[Meson 官方 introspection 格式](https://mesonbuild.com/IDE-integration.html)、
固定 [FFmpeg configure](https://github.com/FFmpeg/FFmpeg/blob/946fcce07b6dcd0331c8cc609192aeff5e1924f8/configure)
（Git blob `374bd5c1512943439480825f32e448e7871eb61c`）和
[x264 configure](https://github.com/mirror/x264/blob/3a21e97bf23676a0bf4616df8bc2207c9fd7b1d3/configure)
（Git blob `71d3a74a0c20e75808eddf95e86a9b4d03b22af4`）。
