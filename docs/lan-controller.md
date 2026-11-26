# 局域网应用控制层

`src/app/LanController` 是 [Issue #5](https://github.com/NAMEWTA/huhucapture/issues/5)
P10 的应用装配层，持有唯一的 `TransferService`，复用 [共享界面](lan-interface.md)、
[现有协议服务](transfer-service.md) 和宿主传入的同一个 `MediaArchiveService`。
它不另建运行时、历史目录或剪贴板服务。

## QApplication 公开接口

宿主先成功加载共享 `SettingsStore`，把归档的 `shared_ptr`、该 store 的引用和
明确的用户私有配置根目录交给控制器，设置 `DesktopTranslator`，再调用
`initialize()`。私有根目录由应用统一按 AppData/XDG 或明确的便携模式决定；
它不从设备名、媒体名或远端路径推导。

没有 LAN 配置时保持关闭，不生成证书、不启动 LAN 工作线程、TCP、UDP 或
Rust runtime。打开设置/设备窗也不隐式启用。此前明确保存的启用设置可在重启后
恢复。无效或未来格式的 LAN 配置保留并报告，不以空默认值覆盖。

| 接口 | 宿主用途 |
| --- | --- |
| `openTransfers(exactEntries)` | 历史条目或已归档编辑输出发起发送；保留精确媒体身份 |
| `openSettings()` | General 页的局域网入口 |
| `availabilityChanged(bool)` | 更新历史页实际发送能力 |
| `archiveChanged()` | 通知同一个 `MediaWorkspace` 刷新历史 |
| `revealFileRequested(path)` | 宿主检查实际归档身份/租约后定位文件 |
| `operationFailed(message)` | 显示本地化实际失败，不记录身份 JSON 或 PIN |
| `shutdown()` / `shutdownFinished()` | 异步停止并等待真正完成，再销毁应用服务 |

控制器维护全局接收确认窗；历史和任务窗都关闭时仍会提示实际 offer。归档变化
只来自 `receivedFiles.published`。已经结束的接收确认窗关闭，结果仍由原服务的
有限活动记录和真实历史归档承担。关闭未答复的确认窗必须实际拒绝。

## 身份、权限和信任

`IdentityStore` 只在一个有界 worker 上读写 `lan/identity.json` 和独立的
`lan/trusted-peers.json`。首次明确启用且身份不存在时调用固定 LocalSend 的
`generate_self_signed` 创建 RSA-2048。已有文件必须读取并校验：损坏、证书/
私钥不匹配、指纹不符、未知信任 schema、权限不安全和磁盘内容变化都保留原件
并失败。有信任记录而身份缺失也不会自动生成新身份。

`huhu_identity_validate` 复用官方证书签名/有效期验证及 Rustls 的私钥加载、
`CertifiedKey::keys_match`，要求实际 RSA 公私钥匹配并重算 DER SHA-256。该调用
不创建 runtime/socket。依据是固定
[LocalSend cert.rs](https://github.com/localsend/localsend/blob/44f48f18a5503c43873570ff489d27ff1983e198/packages/core/src/crypto/cert.rs)
和 [Rustls 0.23.46 signer.rs](https://docs.rs/rustls/0.23.46/src/rustls/crypto/signer.rs.html)，
不自行实现 RSA 或证书解析。

首次身份写入复用排他暂存和 `FileCommitter::commitFileNoReplace`，不覆盖碰巧
出现的同名文件。读取复用不跟随最终链接的原生句柄，核对打开对象及命名对象
身份。`Platform::isPrivateStorage` 承担读取前的权限差异检查：Linux 要求当前 uid
且 group/other 无权限；Windows 检查实际 owner/DACL，允许本用户、SYSTEM、
Administrators，以及 Qt 为其他主体保留的只读属性、READ_CONTROL、SYNCHRONIZE
元数据权限，拒绝其他主体读取内容、列目录、修改、执行或接管 ACL。不能证明
权限的存储不默认为私有；此边界不防御同一用户的恶意进程或机器管理员。

目录创建使用 [Qt `QDir::mkpath` 的权限参数](https://doc.qt.io/qt-6/qdir.html#mkpath)。
Windows 判断核对了
[固定 Qt 6.12.1 QNativeFilePermissions](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/io/qfilesystemengine_win.cpp)
生成的 ACL。仅切换 Windows 只读属性的 `QFile::setPermissions` 不能冒充私钥
DACL 保护。这里调用已有 Qt/OS API，不复制 Qt 文件引擎源码。

新建文件的所有者不能仅由父目录的 DACL 推断。Windows 的
[TokenOwner](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-token_owner)
可为令牌中的用户或组；没有显式所有者时，新文件使用该默认值。
固定 Qt 的 [QTemporaryFile 引擎](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/io/qtemporaryfile.cpp)
和 [QSaveFile](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/io/qsavefile.cpp)
在 Windows 临时文件创建时也使用默认安全描述符。因此首次身份/信任发布与
已有信任的原子替换都在写入任何内容前调用 `preparePrivateStagingFile`。

这个平台入口只接受调用方刚创建并仍持有的空常规文件，借用 CRT fd，不能关闭
调用方的对象。Windows 先核对原 DACL 已满足严格私有数据权限及只有一个链接；
需要时通过 [ReOpenFile](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile)
取得同一对象的所有者元数据句柄，以 `SetSecurityInfo` 仅设置当前 TokenUser
所有者，随后再次执行完整 owner/DACL 检查。它不修改 DACL、不提升权限或改变
进程令牌，也不按路径重新打开其他文件。Linux 不改变任何权限，要求当前 uid、
无 group/other 权限，允许新具名文件或 QSaveFile 使用的未链接 O_TMPFILE。
非空、多链接、类型或权限不符的对象在写入之前失败。

已经保存的身份及信任只走原严格读取检查，不在读取失败后修补其所有者或 ACL。
首次写入继续使用原无覆盖发布；更新继续由 QSaveFile 原子替换。操作系统给出的
真实文件阶段和系统错误保留到 `IdentityFileDiagnostic`，另保留原 QFile 错误枚举。
`nativeStatus` 继续表达 Rust ABI 状态；没有原始系统值时不制造 Win32 错误码。
诊断不记录路径、身份 JSON、密钥、指纹或安全标识符。

信任只绑定用户在发送前明确核对的证书指纹：最多 128 个身份、每个身份 8 个
端点、256 KiB，按最近明确确认淘汰最旧记录。淘汰后须重新确认。已知证书换 IP
仍是同一身份；新证书出现在先前端点时显示旧、新指纹。接收始终逐次确认。
信任文件保留未知顶层/同身份字段，使用进程锁、磁盘快照和 `QSaveFile` 原子
替换；磁盘已改变时不覆盖。PIN、token 和私钥不进入信任、活动快照或错误消息。
当前没有隐式重置损坏身份流程；恢复原文件、明确更换身份、改名和换地址分别处理。

## 网卡、端口与发现

清单复用 [QNetworkInterface](https://doc.qt.io/qt-6/qnetworkinterface.html) 的真实
IPv4、名称、硬件地址、up/running 标志和接口类型。初始不预选网卡；VPN、
loopback、虚拟和未知类型都必须显式选择，不根据网卡名称自动开放网络。
多个网卡具有相同 IPv4 时，地址绑定无法证明网卡隔离，因此不允许该歧义选择。

配置保存所选网卡名称、硬件地址及地址集合。同一网卡的 DHCP 地址全部变化时
跟随该网卡的新地址；仍存在选中别名时不扩展到其他别名。新网卡不会替代消失的
旧选择。不可用的原选择可明确取消；应用时再次检查真实可用性。传输进度刷新
不会覆盖尚未提交的设置编辑。

启用期间每 5 秒读网卡状态。所选地址变化后停止并 join 旧服务，用相同身份重建
监听和发现。任务保留真实终态，用户可重试；不声称透明续传。无可用所选网卡时
停止监听，等待原网卡恢复。唤醒后的下一次实际清单读取可触发重建，但不能据此
宣称完成了实体休眠、热插拔或 DHCP 测试。

首选 TCP 53317。只有来自 OS `AddrInUse` 的结构化 `HUHU_ADDRESS_IN_USE` 才
尝试端口 0 的实际备用端口，最多 4 次；不通过英文错误字符串猜测。实际端口
显示在设置页并公告，持久首选端口仍独立保留。重启先尝试首选端口。

设备窗打开和启用期间每 30 秒，经同一个 `TransferService::refreshDiscovery`
调用已有 `DiscoveryHandle::announce`；窗口也有刷新图标。没有 /24 或公司子网
扫描。公告复用有界 operation/终态预算，最多一个正在公告的 operation；
`DiscoveryRefreshed` 不是“所有邻居已响应”。组播失败停止周期重试并提供手动
IPv4/端口，网络重建后恢复。界面按证书合并多端点，保持选择；冲突指纹要求
重新 TLS 探测。

设置只经应用唯一 `SettingsStore` 更新 `lan` 节并保留未知字段。保存开启失败
不会开始监听；保存停止失败仍停止网络，并明确说明下次启动可能恢复旧设置。
任务/设置窗的普通关闭不停止网络。

监听器发生实际运行错误时保留失败状态并停止服务，不在同一错误上反复创建
runtime。用户可在设置中重新应用；运行中的所选网卡变化继续走上述受控重建流程。

## 准备、取消与资源上限

使用 Qt 单线程 `QThreadPool` / `QtConcurrent`，最多一个身份或媒体准备 job，
无无限等待队列。媒体描述沿用 64 个文件、128 KiB 的上限。先保存用户确认的
公开指纹，再执行完整 `validatedLease`、`PreparedMedia::open`，原生打开后
再次检查历史 revision；不能发送悄然替换的条目。

完成回调上下文是控制器。确认后即使窗口立即销毁，准备仍可进入原服务 `send`；
窗口只接收可选的真实 admission 通知。控制层立即释放额外租约，原服务持有到
真实终态。明确停用或退出请求 stop token；旧 generation 的准备不会在新 listener
上复活。最终析构等待纯 worker 并 join 原服务，worker 不等待 Qt 窗口回调，
没有 detach 或固定超时后假称释放。

宿主可以在 `availabilityChanged`、`busyChanged`、`operationFailed` 或
`shutdownFinished` 的直接连接中销毁控制器。控制层在继续访问状态之前检查
QObject 寿命；真实停止完成通知始终在当前停止分支的最后发出。明确关闭 LAN
即使遇到配置写入冲突，也会立即请求取消正在准备的媒体，再等待原服务停止。

## 实际验证与剩余门槛

`qt_transfer_ui` 检查设备、接收、完成、身份变化和中/俄设置的真实组件与 PNG。
`qt_identity_store` 单独运行真实身份生成、连续信任保存/重开及损坏文件保留，
不要求先启动网络监听器；同一合同也保留在完整控制器旅程中。
`qt_private_storage` 使用真实新空文件和 QSaveFile 临时对象，检查私有准备、
借用句柄仍可使用、原目标保留及替换后权限，拒绝非空或其他不合约对象；Windows
还在 QSaveFile 的独占写句柄仍打开时实际检查所有者元数据访问。

新增 `qt_lan_controller` 使用两个生产控制器、真实私有 RSA 文件、真实网卡、
QTcpServer 实际占用端口、实际 TLS PNG 传输、销毁发送窗后的继续、全局接收
确认/拒绝、共享归档、连续两次信任保存后重开、损坏身份保留和配置冲突。相同证书的实际
127.0.0.2 端点验证设备合并和换 IP；公告记录实际成功或组播不可用结果。
回归还覆盖配置冲突时的真实停网，以及宿主在状态、错误和已完成停止回调中同步
销毁控制器，不以断开连接掩盖服务仍访问已释放对象的问题。
这些目标的通过状态以对应提交的实际 CI 日志和产物为准；本地编译及源代码复核
不能代替 Windows 令牌、权限或真实传输结果。

物理 Windows↔Linux/同平台双机、网卡/VPN、防火墙、DHCP/休眠/热插拔、完整
PNG/GIF/MP4 端到端与持续负载性能仍按 P10a/P12 分别提供证据。通用 PROTOCOL
仍不能完整区分连接失败与 TLS 验证失败，界面使用诚实的合并诊断，不解析英文。
固定官方 LocalSend v1.19.0 互通仍属于独立 P10b。
