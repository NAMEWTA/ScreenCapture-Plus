# 更新下载与文件边界

## 旧 Windows 到完整包的过渡

[Issue #5 第 13 节](https://github.com/NAMEWTA/huhucapture/issues/5) 将旧 Windows 下载桥接（P11a）与 Qt 完整包交付（P11b）分开验收。正式 v0.0.5 仍只识别 `huhucapture.exe`；新的桥接实现保留这个回退，同时能下载新 Qt 的完整便携 ZIP。代码存在不表示桥接版已经发布，也不表示 Qt 安装、升级、卸载门槛已经通过。

旧“设置 → 关于 → 下载最新版本”由 [Update](../src/services/Update.cpp) 保持应用所有权和原有交互；[LegacyUpdateTask](../src/platform/windows/LegacyUpdateTask.h) 只承担旧程序所需的 WinRT HTTP、Windows 目录与文件版本适配。它没有给旧 EXE 引入 Qt 运行库。后续 Qt 迁移使用相同契约；本分支只包含桥接所需的 [UpdateManifest](../src/core/UpdateManifest.h)、[UpdatePolicy](../src/core/UpdatePolicy.h) 与 [包类型边界](../src/core/UpdatePackageHeader.h)，集中维护发行身份、依赖清单、版本和包预算规则。旧 UI 和这个过渡适配在 P13 一起移除，不是长期维护的第二个产品。

## 选择完整包与旧 EXE 回退

桥接先验证本仓库 latest 正式稳定 Release 的规范 tag、页面、资产名、长度、SHA256 和规范下载 URL。

- 有 `release-manifest.json` 时，依次取得并校验 manifest 和 `build-info-windows-x64.json`，选择 `windows` / `x64` / `portable-zip`。旧版原本是便携程序，因此不会未经选择切换为安装器管理的安装形态。需要图形安装器时，发行页面仍提供明确的安装包入口。
- manifest 中选中的包，必须与 GitHub API 资产、构建身份、`contentRoot` 和所选目录下的产品、Qt Core、平台插件清单一致。开发包、脏源码、错误仓库、架构、版本、路径、长度或 hash 均拒绝。
- **仅当 Release 完全没有 manifest 资产时**，才允许回退到旧的 `huhucapture.exe`。该路径保留独立 64 MiB 预算，验证实际 x64 PE 程序及 `X.Y.Z.0` 文件版本与 tag 一致。manifest 存在但缺失正文、损坏或校验失败时，不会悄悄回退。
- 关于页的显式下载可以再次下载同版本；后台检查只提示更高版本，且只下载元数据。后台沿用原空闲检查、UTC 日期记录、延迟提示和明确确认，关闭或拒绝确认不下载；确认默认选择“否”。

完整便携包使用 `huhucapture-vX.Y.Z-windows-x64.zip`，预算为 2 GiB；其他新 Qt 安装形态由共享策略分别设限。旧 64 MiB 限制不会错误套用到完整包，也不会把安装器改名成旧 `huhucapture.exe` 伪装兼容。

## Downloads、流式写入与提交

目标通过 Windows `FOLDERID_Downloads` 解析，遵循系统实际移动或重定向后的下载目录，再解析为实际目录；失败时不静默改存程序目录或媒体归档。系统依据见 [SHGetKnownFolderPath](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath)。

网络在一个应用拥有的 MTA 工作线程运行。WinRT [ResponseHeadersRead](https://learn.microsoft.com/en-us/uwp/api/windows.web.http.httpcompletionoption) 在收到响应头后返回，随后使用实际 `IInputStream::ReadAsync`、64 KiB 缓冲及增量 SHA256，将包直接写入排他创建的 `.part`。元数据分别受 1 MiB / 8 MiB 上限约束，不把整个包读入 `GetBufferAsync`。请求关闭自动解压和自动跳转；显式验证 HTTPS 来源、凭据、片段、正文长度、编码和最多五次跳转。latest 不接受换仓库的跳转；资产只接受规范 GitHub 地址及既定 GitHub 资产主机。

每个元数据请求最多 30 秒，包请求总预算为一小时，读写活动有 30 秒空闲上限；跳转不会重置总预算。取消和超时直接作用于当前真实 WinRT 异步操作。进度按时间节流，旧 UI dispatcher 最多保留一份待送快照，不为每个网络分片积累界面事件。

暂存、身份验证与发布复用 [FileIdentity](../src/platform/FileIdentity.h) 和 [FileCommitter](../src/platform/FileCommitter.h)：不跟随最终组件的符号链接或重解析点，不截断被替换的临时文件，flush、长度、hash、类型检查成功后才排他提交。Windows 的最终无覆盖移动保留 write-through 行为。已有同名文件使用 `-001` 等新后缀，旧裸 EXE 的本地文件名仍为 `huhucapture-vX.Y.Z-windows-x64.exe`。失败清理只针对本任务仍匹配身份的 `.part`；一旦最终名称发布，该文件就属于用户，不因晚到取消或界面销毁而删除。

包头检查是有界的类型检查，**不等于已经解包并验证全部运行库**。实际完整包的产品身份、CPU 架构、依赖、Qt 插件、安装和运行检查由后续 Qt 打包流程与 [Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5) 的发行门槛完成，再从同一产物生成 manifest。不能凭一份字段正确的 JSON 或测试 ZIP 宣称产品可以安装。

## 完成说明与用户数据

下载只保留文件并显示实际位置、打开下载文件夹和对应说明，不自动执行、替换或重启。关于页重新打开时观察同一任务；长说明使用已有 DirectWrite 排版，完整显示在实际内容宽度内。

完整 ZIP 迁移时，先退出旧程序，将**整个包**解压到一个新目录。保留旧目录和原配置；如需延续 EXE 旁的便携 `config.json`，在首次启动新包之前复制原文件到新 EXE 旁，保留旧原件。不要只取出新 EXE，也不要删除现有 Documents 媒体目录。新配置服务的旧格式读取、备份、未知字段保护与冲突规则必须遵守 [Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5)，并由实际 Qt 版本说明提供。系统安装形态及旧系统支持范围按对应完整包说明处理。

旧 EXE 回退完成后，仍按原方式退出程序、将下载文件改成原可执行文件名并手动替换。更新任务不会读写或清空媒体归档；除原有更新日期记账外，不改变用户配置的保存位置或格式。

桥接只有仍是 latest 时才能被未升级的 v0.0.5 发现。Qt 完整包成为 latest 后，v0.0.5 不会自动回溯历史桥接 Release。跳过桥接的用户应直接从正式发行页下载完整安装包或 ZIP；不得把旧 EXE 再挂到新 tag 上假装已经完成迁移。

## 页面、进程与失败边界

| 情况 | 实际结果 |
| --- | --- |
| 关闭或重建关于页 | 只退订控件回调，应用任务继续，重开恢复当前真实状态 |
| manifest 存在但错误或不可获取 | 明确失败，不能降级到同一 Release 的裸 EXE |
| 正文过长、过短、hash 或旧 EXE 版本不匹配 | 不提交成品，清理本任务仍拥有的暂存，保留已有下载 |
| Downloads 不可用、磁盘或文件提交失败 | 类型化失败，允许重试，不改存其他目录 |
| 文件已发布而持久性结果不确定 | 保留实际文件及可定位路径，明确结果不确定，不冒充可靠完成 |
| 应用退出 | 停止提示和订阅，取消真实网络操作；旧消息循环退出后的 `App::dispose` join 工作线程，再拆除 COM |

正常运行的 UI 不等待网络操作；只有旧进程最终关闭屏障执行取消后的 join。后续 Qt 宿主使用自身的异步关闭回执；本分支不构建 Qt 宿主。不存在一个页面拥有的网络任务，也没有脱离应用生命周期的后台协程。

## 验证入口与证据范围

旧 Windows 的正常构建、Debug、真实 GUI、latest 下载和原裸 EXE 资产回归继续保留；旧 workflow 不再自动公开发行：

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/build.ps1 -Configuration Debug -SkipRestore
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/test-windows.ps1 -Update
pwsh ./scripts/test-legacy-update.ps1 -Gui
pwsh ./scripts/test-legacy-update.ps1 -Configuration Debug
```

桥接 probe 链接生产 WinRT 适配与相同文件提交源码，没有模拟 HTTP 成功回调。Python 标准库 HTTP 服务发送真实旧生产 EXE 和超过 64 MiB 的明确传输夹具，验证实际字节、增量进度、工作集增量、重名、取消、异常元数据、正文边界、hash、缺依赖、路径和错误跳转。独立 GUI harness 可经过同一应用服务下载到实际 Downloads，关闭/重建页面，再输出六语和最小窗口的 PNG。产品本身没有通过 UI、CLI、环境或配置替换更新源的入口；测试 origin 只在独立 harness 里显式注入且限于字面量 loopback HTTP。

**默认 ZIP 夹具不是可安装产品，测试报告会明确标出这点。** 它用于 P11a 的受控完整包下载契约，结合原 Windows 生产门槛可以支持独立桥接发行；不能记为 Qt 成品、对应源码或安装验收。Qt 首发前还要在包含 Qt 打包验证器的整合分支提供本次真正完整 aggregate 及原始 manifest / build-info。本旧架构分支没有该验证器，显式请求下述模式会拒绝，不能用受控 fixture 冒充：

```powershell
pwsh ./scripts/test-legacy-update.ps1 -Gui `
  -RealPackageDirectory artifacts/release-candidate `
  -RequireRealPackage
```

该模式要求两平台最终正式 aggregate，调用同一打包验证器核对四种包、符号、computed-complete 来源、当前源 SHA/唯一版本与完整 checksum 集。随后直接服务同一实际资产字节，不会把 `developmentOnly` 或 `sourceDirty` 改成 `false`，也不会在目录缺少真实包时当成通过。报告绑定原 manifest SHA、包 SHA、实际 received/total 和 About 旅程；完整包下载仍不代替干净机器安装、旧配置迁移、升级与卸载的实际验收。

按 Issue #5 §13.3/13.5，P11a 过渡发行独立使用原有 Windows 构建、五项资产与下载门槛，并用受控完整包 fixture 核对新契约；不必等待 Qt 首发或 Linux 安装验收。[独立桥接入口](bridge-release.md) 核对显式声明、当前 main、同 SHA 的成功 Windows run 和原始 Debug / Release 报告，报告仍区分受控 fixture 与真正 Qt 成品。Qt 双平台成品的实际安装、升级、卸载及首发 latest 烟测属于 P11b/P12 的独立必要门槛。

共享规则和包类型边界可在标准 C++20 下运行；Qt HTTP 回归也会继续运行同一规则。Windows probe、真实关于窗口、完整包契约和安装结果必须在对应环境分别记录。Linux 上纯规则或 Qt HTTP 测试成功，不能记为 Windows 桥接通过；各自未满足 Issue 对应的必要门槛时，不发布桥接版或新 Qt 正式版。
