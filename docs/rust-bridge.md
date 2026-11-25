# Rust 聚合库与原生 ABI

## 构建和复用边界

`src/platform/rust/Cargo.toml` 是自有 `huhucapture-native` crate，属于
`third_party/localsend/Cargo.toml` 的同一 workspace。产物名称为
`libhuhucapture_native.a` / `huhucapture_native.lib`。Qt 程序只链接这个 Rust
staticlib；不另外链接 `gifski.lib` 或另一个 LocalSend staticlib。

聚合库直接依赖固定的
[gifski 1.34.0](https://github.com/ImageOptim/gifski/tree/1060eab4500a20f27e2fa3ab7e85473d0e921cbd)，
关闭默认 feature，仅启用与旧产品相同的 `gifsicle`。实际编码继续使用上游
`src/c_api.rs`、`src/lib.rs` 的队列、线程与 finish 结果；没有复制第二套编码器。
`third_party/gifski/gifski.h` 与 LICENSE 保持上游字节，来源见其 `upstream.json`。
`upstream-cargo.lock` 是固定上游的原始锁文件，用于检查编码依赖版本，并不是
另一套生产构建。网络实现复用 [LocalSend core](localsend-core.md) 的官方
client/server/discovery、TLS 身份、证书 pinning、报文和受控流接缝。

从 `third_party/localsend` 目录执行，以便 rustup 读取固定的 Rust 1.97.1：

```sh
cargo test -p localsend --features discovery --locked --no-fail-fast
cargo test -p huhucapture-native --locked --no-fail-fast
cargo rustc -p huhucapture-native --release --locked -- --print native-static-libs
```

首次加入聚合成员时，需要真实 Cargo 解析并收回 Windows/Linux 的锁文件，然后
恢复 `--locked`。不能把 core 阶段的旧锁称为已经验证的聚合锁，不能补造 checksum。
编码依赖以固定上游锁中的实际版本为基准；版本漂移需要独立说明及编码对照。
最终 C++ 链接使用该次构建打印的 `native-static-libs`，不猜测系统库列表。

gifski 的 AGPL-3.0-or-later、LocalSend 的 Apache-2.0 及依赖许可证各自保留。
自有桥代码使用项目许可；它不把最终组合产物重新标为 MIT-only。正式发行还需
对应源码、完整依赖与许可证材料。旧 Windows 产品的 gifski 构建在 P13 前保留。

## Windows CRT 与调试限制

新 Qt Release 使用 `/MD`，Rust 使用动态 CRT，不能继承旧生产的 `+crt-static`。
Cargo 的 dev/debug profile 不是 MSVC `/MDd`：固定 Rust 的
[libcore](https://github.com/rust-lang/rust/blob/1.97.1/library/core/src/ffi/mod.rs)
为非 `crt-static` 链接 `/defaultlib:msvcrt`，静态时才切换 `libcmt`；没有由
debug profile 选择 `msvcrtd` 的分支。实际使用的
[cc 1.4.0](https://github.com/rust-lang/cc-rs/blob/cc-v1.4.0/src/lib.rs)
也按 `crt-static` 选择 `-MD` / `-MT`，不会自动切换到 `-MDd`。

因此当前同进程 ABI 门槛先验证 Windows Qt Release `/MD`。可使用 Release Qt
配合 C++ / Rust 调试符号进行调试；这不等于 Qt Debug `/MDd` 已通过聚合验证。
Qt Debug 的既有独立组件测试继续执行，Windows `/MDd` 聚合仍是单独的未完成
工具链门槛。不得用忽略 LNK4098、`/NODEFAULTLIB` 替换运行库或跨 allocator
释放来掩盖冲突。Linux 的 Debug/Release 不存在这一个 MSVC CRT 选择问题。

## 线程、拥有关系和终态

公开接口在 `src/platform/rust/include/huhucapture_native.h`。输入文本是 UTF-8
指针加长度，在调用内复制或解析；不保存借用指针。所有 Rust 对象由匹配的
`*_free` 释放，数据块写入 caller 提供的内存。opaque 句柄必须类型正确、有效
且只释放一次。C ABI 检查空指针、长度和状态；不能把任意失效地址变成安全指针。

一个引擎使用两个 Tokio worker、最多四个阻塞 IO worker。控制事件队列固定
64 项；最多同时接受一个发送任务和两个手动发现任务。发送/手动发现先预留
终态位置再返回 operation ID；接收会话也预留终态位置。进度和设备刷新是可以
合并的快照，并为接收文件交接留下队列空间。不得通过无限任务或无限消息队列
掩盖消费端阻塞。

没有 Rust→Qt/UI 的业务回调。服务工作线程 poll 并复制必要状态到 GUI；GUI
不消费文件块，不做哈希、证书生成或磁盘提交。生命周期操作 `stop/free` 要求
调用方先结束自己的并发 API 调用；普通控制调用可并发，单个 receive 只有一个
消费者。创建、读取和停止应离开 GUI 线程。

`stop` 取消会话、停止监听/发现、join 跟踪任务和事件 worker，最后销毁 runtime，
等待已提交给阻塞池的 OS 文件操作。它不使用 `shutdown_timeout` 或后台 detach。
取消已经发出的文件读取时，使用 Tokio 的
[`File::into_std`](https://github.com/tokio-rs/tokio/blob/tokio-1.53.1/tokio/src/fs/file.rs)
等待 in-flight 操作，再允许同一 lease 被复用。停止后终态仍可 drain，最后一个
事件是 `STOPPED`，空队列随后返回 `CLOSED`。系统文件操作本身阻塞时，停止需要
等待它实际结束；不能一边报告释放完成一边保留访问旧句柄的线程。

项目导出函数在 ABI 边界捕获 Rust panic，异步业务 worker 的 panic 转为明确
终态并请求停止。gifski 保持上游 C API；传入的像素尺寸、跨度、时间戳与回调
生命周期仍须遵循其头文件。C++ 异常不得越过 upstream C 回调，finish 返回值
必须进入录制结果。

## 发送和接收

发送先打开选定的普通文件并取得稳定 OS 句柄，拒绝最终路径上的符号链接 /
reparse point。Windows lease 允许重命名/删除共享，拒绝同时写入共享；Linux
使用 `O_NOFOLLOW` 并在打开后验证普通文件。该接缝不声称能防止同一用户修改
Linux 文件内容，应用必须让用户媒体在 lease 期间保持不可变，并保留自己的历史
删除租约。哈希和发送都使用同一打开对象，不在后台重新打开可能已被替换的路径。
长度变化、短读、超长、读取错误和取消有不同结果；队列最多四块、每块 512 KiB。

peer 参数必须带真实 TLS 发现得到的 fingerprint，alias 不参与认证。传输请求
使用 pinning；未认证的发现仅用于取得真实证书身份，不能直接当作用户长期信任。
新接收必须经过显式确认。选择 LAN 网卡、网络类型许可、持久身份、历史删除租约
以及任务 UI 都由应用服务负责，bridge 不另建平台业务规则。

接收顺序是：消费 `RECEIVE_FILE` → caller 写排他暂存 → read 到 `END` →
`verify` 返回实际长度、SHA 和取消许可 → 完整 codec/probe 检查 → 发布前检查
取消 → 共享 FileCommitter 无覆盖持久提交 → `complete(OK, kind)`。
`END` 只表示字节流结束。提前的成功 complete 返回 `INVALID_STATE`，不会消耗
正确完成机会，也不会让 core 返回 200。校验超时可以继续等待；失败不能变为
成功。发送端漏传可选 SHA 时，`sender_checksum_matched` 为 0，不假称完成比对。

完整解码、时间戳归档名和历史索引不属于 Rust 协议层。`DURABILITY_UNKNOWN`
表示真实目标可能已发布，C++ 服务必须保留路径并报告失败。取消晚于原子提交时
也保留已经发布的媒体。未取出的文件事件、未完成就释放的 receive 都会拒绝
成功回执。接收方取消会同时处理 core 的 Pending 和 Active 会话，防止决定与
激活交错后出现晚到的“复活”会话。

## 事件数据

`HuhuEventView` 的 kind/status/operation 是固定宽度字段。json 切片只在 event
未释放时有效；文件块不放在 JSON 或 GUI 队列。以下键区分大小写：

| kind | JSON | 语义 |
| --- | --- | --- |
| RECEIVE_OFFER | `sessionId, peerIp, alias, fingerprint, files` | files 为官方 FileDto 数组；fingerprint 来自客户端 TLS 证书 |
| RECEIVE_FILE | `sessionId, fileId, file` | 通过 take_receive 一次性移动消费者所有权 |
| RECEIVE_END | `sessionId, files` | files 是 fileId→状态码；整体 OK 要求所有接受项均完成 |
| SEND_PROGRESS | `fileId, phase, bytes, total` | phase 为 hashing/sending，快照可合并 |
| SEND_END | `files, error, httpStatus, cleanupError` | files 的每项含 id/fileName/size/accepted/status；没有错误时可为 null |
| DISCOVERY_CHANGED | 状态快照提示，可含 `multicastAvailable,error` | 重新查询 devices；可合并，不承载唯一终态 |
| DISCOVERY_END | `address,port,alias,fingerprint,version` 或 error/httpStatus | 一次手动 TLS 探测的终态 |
| LISTENER_ERROR | `error,httpStatus` | 监听不可继续，整体引擎进入停止状态 |
| STOPPED | 空对象 | 内部任务与 OS 资源已经 join/释放，最后一个排队事件 |

SEND_END 的 `HUHU_PARTIAL` 表示接受项全部发送成功，但接收方拒绝了部分选择项；
不能把它显示成所有文件成功。准备失败或 panic 时 SEND_END 可能只有 error 和
httpStatus。每个已获准的 send/discover operation 恰有一个 END；拒绝的 admission
没有 operation，也没有伪造成功事件。取消远端会话失败会单独记录 cleanupError，
不会覆盖原来的传输失败。

## 验证现状与剩余门槛

`src/platform/rust/tests/bridge.rs` 通过导出 ABI 操作真实 loopback TLS、官方身份、
真实文件和有界读取，验证接收取消、重新使用会话/lease、提前回执、校验后归档
错误、停止与终态、手动 TLS 发现。归档错误用例显式注入应用 IO 失败；它不声称
已经完成成功归档。`tests/qt/rust_bridge_probe.cpp` 负责同一个 Qt 进程内的真实
gifski 编码/逐帧解码、TLS、SHA、FileCommitter 与错误/退出验证。

这些源码存在不表示用例已运行。实际执行结果由对应 CI 记录；本地编写环境没有
Rust/Cargo。当前聚合新增代码等待真实 CI 编译和运行，不能拿已通过的 core 测试
代替同进程 ABI 结果。真实跨机互传、组播/多网卡、全格式完整验证、正式媒体服务、
任务 UI、长期稳定性与性能基线仍属 P00/P10/P12 门槛，不触发正式发布。
