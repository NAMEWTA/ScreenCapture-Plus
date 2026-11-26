# 独立的旧 Windows 桥接发行

[Issue #5 §13.3 / §13.5](https://github.com/NAMEWTA/huhucapture/issues/5) 明确要求先发布一次旧架构更新桥接，再交付 Qt 双平台完整包。桥接沿用原 Windows 构建与五项资产，使用受控完整包下载契约 fixture 验证新能力；它不受尚未完成的 Qt 对应源码收集或 Linux 安装门槛阻塞。

`windows.yml` 继续承担原 Release / Debug、核心、原生渲染、托盘启动、真实 GUI / 剪贴板、旧 latest 下载和桥接 HTTP / About 检查。普通 push、PR、已有 release 事件都不发布新资产。独立 [windows-bridge-release.yml](../.github/workflows/windows-bridge-release.yml) 只接受手动调度，且只能在规范仓库的当前 `main` 上执行。

## 显式声明与实际输入

独立桥接已在 main 的 PR #8 选择 v0.0.6，统一更新 `build/version.json` 及其生成文件，并提供匹配的发行说明。以下是实际 `releases/bridge-publish.json`；声明和合并存在不代表资产已经公开：

```json
{
  "schemaVersion": 1,
  "kind": "legacy-windows-bridge",
  "tag": "v0.0.6",
  "notes": "releases/v0.0.6.md",
  "previousRelease": "v0.0.5"
}
```

`tag` 必须等于当前唯一版本，说明文件必须存在且非空，版本必须高于声明的旧稳定版。没有这份独立声明时，桥接入口直接拒绝；不会因为仓库还保留 `releases/publish.json` 的旧 `v0.0.5` 声明就再发一次。

该提交进入 `main` 后，先等待它的原 Windows workflow 全部成功。发布调度的 `source_commit` 填当前 main 的精确 40 位提交，`windows_run` 填同一提交的成功 Windows run。PR 关联提交、另一个 SHA、进行中或失败的 run、跳过 Debug / GUI / 下载步骤的 run 都不能代替它。发布流程直接下载原 `windows-x64` 和 `windows-render-checks` artifacts，不重新构建或混入另一轮符号。

## 五项资产与桥接证据

只允许以下五项文件，额外的 Qt manifest、安装器或 Linux 包也会使入口拒绝：

| 资产 | 核对内容 |
| --- | --- |
| `huhucapture.exe` | 不超过旧更新器的 64 MiB 上限；真实 Windows AMD64 可执行 PE32+、原品牌和 `X.Y.Z.0` 文件版本 |
| `huhucapture-vX.Y.Z-windows-x64.zip` | 相同 EXE 和 build-info 原字节、六种语言、现有许可及通知；没有 Qt DLL 混入单文件旧产品 |
| `huhucapture-vX.Y.Z-windows-x64-symbols.zip` | 同次构建的 PDB 集合，包含 `huhucapture.pdb` |
| `build-info.json` | 规范仓库、源 SHA、版本/tag、Release、x64、原 MSVC v145 / 静态 CRT 和当前依赖锁 |
| `SHA256SUMS.txt` | 其余四项精确长度之外的完整 SHA256，拒绝重复、缺失或未知文件 |

`scripts/test-legacy-update.py` 的原生报告记录实际源 SHA、唯一版本、生产 EXE / probe 的大小及 SHA256、Debug / Release 配置和受控 ZIP 的字节身份。发布器要求两种配置的所有既有下载、拒绝、取消与排空 case；Release 还必须完成真实 About 页面关闭/重建、实际 Downloads 文件和六语最小窗口 PNG。

受控 ZIP 超过 64 MiB，包含明确注明非产品的 Qt 条目。它验证清单选择、完整正文流式写入、所选 `contentRoot`、依赖/路径拒绝、哈希、进度、工作集预算和任务寿命。报告仍保留 `transportFixtureIncluded: true`、`actualCompletePackageRun: false`；发布器不把它改写成可安装 Qt 包或对应源码已闭合。桥接发行通过的是 Issue 规定的旧 Windows 生产与新下载契约门槛。Qt 首发另用本次真正完整 aggregate 验证新旧两个关于页的实际包下载，见 [Qt 打包](qt-packaging.md)。

## 一次发布、不中途覆盖

`publish-release.ps1` 现在仅转交这个显式 bridge-only 入口；它不恢复旧的自动发布作业，也不接受 Qt 资产。`publish-bridge-release.py` 在任何写操作前核对当前 main、声明、原生 CI、五资产和真实证据，要求 `previousRelease` 仍是实际 latest 且仍带旧裸 EXE。已存在的目标 tag、Release、draft 或已经记录过的桥接发行均拒绝。

通过后原子创建新 tag，在未公开 draft 中上传五个原文件，逐项核对 GitHub 返回的状态、名称、大小及 SHA256，再次核对本地文件、main、tag 和旧 latest，最后公开为新 latest。不使用 `--clobber`，不移动 tag，不自动修复或删除已有 draft。中断留下的新 tag/draft 必须由维护者核对后单独处理。

桥接和 Qt 正式发布 workflow 共享 `huhucapture-stable-release` 串行组，并禁止取消正在发布的作业。Qt 正式入口还检查独立桥接已经实际发布；桥接入口不要求反方向的 Qt 完整候选。因此可以先完成桥接过渡，再在同一唯一版本源推进 Qt 的后续 minor 版本。

Qt 成为 latest 后，跳过桥接的 v0.0.5 不会自动回溯历史发行。桥接与 Qt 发行说明、README 必须明确提供手动下载完整安装包 / ZIP 的路径，不能把旧 EXE 挂到 Qt 新 tag 冒充兼容。

`tests/bridge_release_tests.py` 仅验证声明、身份、路径、预算和错误证据拒绝；它不执行 Windows 或发布动作。原生桥接/安装结果只能来自相应原环境的实际 workflow，不能以这些离线检查代替。
