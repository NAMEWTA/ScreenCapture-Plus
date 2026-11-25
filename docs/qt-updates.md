# Qt 完整包更新任务

本文描述 [Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5) 第 13.4 节的新 Qt 下载服务。旧版 Windows 单 EXE 下载行为仍见 [旧更新器说明](updates.md)；本服务不把 Qt 的裸 EXE 当作完整安装包，也不自行安装或启动下载结果。旧版到桥接版的发行，以及干净机器上的安装、升级、卸载，仍须分别通过发行门槛。

## 应用拥有任务，页面订阅状态

[UpdateService](../src/services/update/UpdateService.h) 由应用持有，接收实际 Downloads 位置、当前版本和已经识别的安装形态。Downloads 来自平台的已知文件夹，不能从当前目录或用户主目录拼接推测。安装形态由宿主验证安装标记；DEB、AppImage、Windows 安装器和便携 ZIP 对应不同资产。

构造服务不发起网络请求。检查和下载共用一个延迟启动的工作线程，该线程创建并拥有 `QNetworkAccessManager`、应答、计时器、临时文件及哈希状态。文件写入、校验和提交均不占用界面线程。关于页只触发服务、订阅值快照，并显示实际完成路径及相应安装说明。关闭或重建页面会断开该订阅，不取消下载。进度通知节流，跨线程邮箱最多保留一份待发布快照，不按网络分片无限积累界面事件。

`check()` 只在稳定版本高于当前版本时报告有更新；`download()` 表示明确下载 latest 稳定版，相同版本也可以再次下载。两者都校验同一条元数据链，检查时不取得包正文。任务进行中拒绝重复启动。退出应用时调用 `shutdown()`，取消未完成的请求、清理本任务临时文件、释放真实网络对象并等待线程结束，之后才发出 `shutdownFinished()`；宿主可以在该信号中立即销毁服务。

## 来源和完整性链

生产路径固定为本仓库的 GitHub `releases/latest` API，以及该 Release 下的规范资产 URL。来源常量来自 [Brand.h](../src/app/Brand.h)。不从页面、配置、对端设备或下载文件读取可替换的更新源。HTTP 请求不使用缓存，请求未压缩正文，限定应答大小、超时和跳转次数。

验证顺序为：

1. GitHub latest 应答必须是正式稳定 Release，仓库页面、`vX.Y.Z` tag 和资产 URL 必须属于 `NAMEWTA/huhucapture`。版本必须是规范的三个十进制分量，不能含预发布后缀或多余前导零。资产名称唯一且不能带路径；GitHub 给出的 `digest` 必须是 `sha256:` 加 64 位小写十六进制。
2. 根据 API 的长度和 SHA256 下载 `release-manifest.json`，验证 schema、产品、仓库、tag、版本、源提交及所选平台、架构、安装形态。清单和 build-info 若包含 `developmentOnly` 或 `sourceDirty`，值必须是 JSON 布尔值 `false`；`true`、字符串、数字和 `null` 都会被拒绝。选中的包和 build-info 必须同时存在于该 Release，且名称、长度、哈希、规范 URL 一致。
3. 根据 manifest 和 API 共同声明的长度、SHA256 下载平台 build-info，验证同一构建身份、依赖清单、文件清单和包描述。清单至少包括产品可执行文件、Qt Core 和对应平台插件；Linux 还必须有 Wayland 插件。依赖至少列出 `qt`、`gifski`、`localsend`、`ffmpeg` 的版本、实际许可证及 HTTPS 来源。许可证字段接受实际 SPDX 表达式，包括锁定依赖所用的 GPL / AGPL；不能为了通过校验把许可证改写为不真实的宽松许可。
4. 流式下载所选完整包，核对收到的字节数、SHA256 和包类型头，再排他提交。manifest 和 build-info 的 SHA256 将完整包绑定到已验证构建的身份和依赖清单。

Qt 更新器不实现另一套 ZIP、DEB、AppImage 或安装器解包引擎。包类型头可拒绝明显伪装内容；依赖清单和包哈希提供同一构建的绑定。这些检查不能代替发行流程对实际包内容、可执行文件版本、CPU 架构、运行库、Qt 插件和安装行为的验证。发行方必须先完成真实完整包的检查，再从那个包及部署目录生成清单。不能用一份填写了正确字段的 JSON 证明随意生成的文件可安装。

跳转最多五次。生产环境仅接受 HTTPS：规范 GitHub 资产 URL，以及 GitHub 的 `release-assets.githubusercontent.com` / `objects.githubusercontent.com` 资产服务，不接受其他主机、明文降级、凭据或片段。重定向后的内容仍受原资产长度和哈希约束。latest API 不接受替换仓库的跳转。具体 API 依据见 [GitHub Release asset 文档](https://docs.github.com/en/rest/releases/assets?apiVersion=2022-11-28)，手动跳转及读缓冲区依据见 [QNetworkRequest](https://doc.qt.io/qt-6/qnetworkrequest.html) 与 [QNetworkReply](https://doc.qt.io/qt-6/qnetworkreply.html)。

## 唯一发布清单格式

清单均为 UTF-8 JSON。下列字段中的省略符只是格式说明，生成器必须填入真实值。版本来自唯一输入 `build/version.json`，tag 由它生成；根目录 `VERSION` 是受校验的生成输出。`sourceCommit` 是 40 位小写 Git 提交 SHA。同一 Release 内所有资产使用同一身份。

`release-manifest.json`：

```json
{
  "schemaVersion": 1,
  "product": "huhucapture",
  "repository": "NAMEWTA/huhucapture",
  "tag": "vX.Y.Z",
  "version": "X.Y.Z",
  "sourceCommit": "<40 lowercase hex>",
  "sourceDirty": false,
  "assets": [
    {
      "platform": "windows",
      "architecture": "x64",
      "packageKind": "portable-zip",
      "name": "huhucapture-vX.Y.Z-windows-x64.zip",
      "size": 123456789,
      "sha256": "<64 lowercase hex>",
      "minimumSystem": "Windows 10 1809",
      "buildInfo": {
        "name": "build-info-windows-x64.json",
        "size": 12345,
        "sha256": "<64 lowercase hex>"
      }
    }
  ]
}
```

`assets` 中每个受支持安装形态只出现一次。`minimumSystem` 来自实际验证的发行基线；它是说明字段，不能替代安装器的系统兼容性检查。资产名和预算如下；所有大小都是正整数，清单和包均以字节计数。

| 平台 | 架构 | packageKind | 规范文件名 | 下载硬上限 |
| --- | --- | --- | --- | --- |
| windows | x64 | installer | `huhucapture-vX.Y.Z-windows-x64-setup.exe` | 1 GiB |
| windows | x64 | portable-zip | `huhucapture-vX.Y.Z-windows-x64.zip` | 2 GiB |
| linux | amd64 | deb | `huhucapture-vX.Y.Z-linux-amd64.deb` | 1 GiB |
| linux | amd64 | appimage | `huhucapture-vX.Y.Z-linux-amd64.AppImage` | 2 GiB |

latest API 和 release manifest 各最多 1 MiB，平台 build-info 最多 8 MiB；API 最多 128 项资产，manifest 最多 16 项安装形态，build-info 最多 128 项依赖、8192 个文件和 16 项包。包正文以 64 KiB 分块读写，网络应答读缓冲区为 256 KiB。各请求空闲超时 30 秒；单个元数据请求总时限 30 秒，单个包请求总时限一小时，跳转同样受五次上限限制。

`build-info-windows-x64.json` 或 `build-info-linux-amd64.json`：

```json
{
  "schemaVersion": 1,
  "product": "huhucapture",
  "repository": "NAMEWTA/huhucapture",
  "tag": "vX.Y.Z",
  "version": "X.Y.Z",
  "sourceCommit": "<40 lowercase hex>",
  "sourceDirty": false,
  "developmentOnly": false,
  "platform": "windows",
  "architecture": "x64",
  "dependencies": [
    {"name": "qt", "version": "<locked version>", "license": "<actual SPDX expression>", "source": "https://<canonical upstream>"},
    {"name": "gifski", "version": "<locked version>", "license": "<actual SPDX expression>", "source": "https://<canonical upstream>"},
    {"name": "localsend", "version": "<locked version>", "license": "<actual SPDX expression>", "source": "https://<canonical upstream>"},
    {"name": "ffmpeg", "version": "<locked version>", "license": "<actual SPDX expression>", "source": "https://<canonical upstream>"}
  ],
  "files": [
    {"path": "huhucapture.exe", "size": 123456, "sha256": "<64 lowercase hex>"},
    {"path": "Qt6Core.dll", "size": 234567, "sha256": "<64 lowercase hex>"},
    {"path": "plugins/platforms/qwindows.dll", "size": 345678, "sha256": "<64 lowercase hex>"}
  ],
  "packages": [
    {"name": "huhucapture-vX.Y.Z-windows-x64.zip", "packageKind": "portable-zip", "size": 123456789, "sha256": "<64 lowercase hex>"}
  ]
}
```

`files` 要包含实际部署目录的完整清单，示例仅展示必需类型。路径必须是唯一的相对路径，使用 `/` 分隔，禁止根路径、空分量、`.`、`..`、反斜杠和冒号。Windows 比较路径时忽略大小写。`size` 允许空文件为零；产品及必需 Qt 库、插件必须非空。实际打包器同时记录该平台两种包的完整树，因此给路径加 `portable-zip/`、`installer/`、`deb/` 或 `appimage/` 前缀，并在相应 `packages` 条目增加 `contentRoot`。服务只从所选包的 `contentRoot` 内寻找必需运行库；另一个包中的文件不能补足当前包的缺项。`packages` 的名称、大小和哈希必须与 manifest 及 GitHub 资产一致。调试符号可以另行发布，但不作为用户默认下载项。

生成顺序为：准备并验证部署目录 → 生成安装包并计算包哈希 → 生成平台 build-info 并计算其哈希 → 生成 release manifest。这样不形成哈希循环。只有同仓库、同 Release 的资产才能被引用；清单不携带任意下载 URL。

## 文件提交与失败语义

文件操作复用 [FileCommitter](../src/platform/FileCommitter.h) 和 [FileIdentity](../src/platform/FileIdentity.h)。服务在实际 Downloads 目录排他创建随机 `.part`，取得文件身份后打开同一个空文件，流式写入正文。关闭文件前检查类型头、大小及打开句柄和路径是否仍指向同一对象。完整文件使用 `commitFileNoReplace` 发布；重名时依次尝试 `-001`、`-002` 后缀，不修改已有下载。

失败或取消只清理当前任务仍拥有的临时文件身份。已提交的最终文件不属于清理范围。若发布后目录持久化不能确认，服务报告失败并保留实际文件路径，关于页允许查看该路径，不能删除已经发布的内容或显示无条件成功。清空媒体历史与更新下载相互独立。

下载完成后的说明由安装形态决定：Windows 安装器需用户退出旧程序后手动运行；ZIP 需解压并替换完整便携目录；DEB 由用户使用包管理器安装；AppImage 由用户退出旧程序后替换相应文件。服务不自动执行安装器、解包、改系统文件、重启应用或删除旧版本。

## 可重复验证与证据边界

[qt_updates](../tests/qt/update_service_tests.cpp) 使用真实 `QTcpServer` 和 `QTcpSocket`，经过生产服务的异步网络、临时文件、SHA256 和平台提交路径。它验证当前稳定版重复下载、真实中文 Downloads 路径、同名文件保留、页面订阅对象销毁后继续、缺依赖清单、错误仓库或架构、错误正文哈希、截断、跨来源跳转、超预算元数据、受控同来源跳转，以及退出时的资源释放和临时文件清理。开发构建测试会重新计算整条 HTTP 元数据链的正确哈希，并验证清单或 build-info 的开发、脏源码及非法类型标记仍在包正文 GET 之前被拒绝。

测试专用来源只能由宿主代码显式构造为带端口的 `http://127.0.0.1` 或 `http://[::1]`。它只将规范 GitHub URL 的请求 origin 替换为真实本地 HTTP 服务器，清单里的仓库、tag、规范 URL、长度、哈希和包身份规则仍完整执行。生产应用不暴露这个参数，也不读取环境或配置中的任意更新 URL。

该自动化测试中的 ZIP 是有真实 ZIP 结构、文件 CRC 和注释的传输契约样本，条目明确标明不是可安装产品。它证明下载任务和文件边界，不能证明 Windows / Linux 实际发布包可运行。首个 Qt 包进入 latest 前还须用受控 HTTP 下载本次实际完整包；发布后须另行跑真实 GitHub latest 烟测。安装、升级、卸载、外部网络跳转和干净机器依赖验证的结果应分别记录，不能从本地传输测试推断它们已经通过。
