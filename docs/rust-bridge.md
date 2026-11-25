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
严格 GIF 接收验证直接复用这棵编码依赖树已有的 `gif =0.13.3`，不另外引入
运行时；其精确源码、MIT/Apache-2.0 许可与 image-rs/nwin 归属记录在
`third_party/gif/upstream.json`。TLS 保持 core 实际已验证的 `rustls =0.23.46`，
不能把 core 清单的最低兼容版本误锁为低于既有 lock 的版本。

从 `third_party/localsend` 目录执行，以便 rustup 读取固定的 Rust 1.97.1：

```sh
cargo test -p localsend --features discovery --locked --no-fail-fast
cargo test -p huhucapture-native --locked --no-fail-fast
cargo rustc -p huhucapture-native --release --locked -- --print native-static-libs
```

聚合锁已从提交 `c16aba4c1662645a93817706b8b35e85a6511fab` 的 Windows/Linux
真实 Cargo 构建收回；两端字节完全一致，SHA-256 为
`02579ba6cce8b2a4e55a464834b895f40dfb8b677e5211b5883fdd0b8369172c`。
CI 恢复 `--locked`，不会在每次构建重新解析。锁中的 registry checksum 来自
实际 Cargo 输出；既有依赖的 checksum 均未改变。
编码依赖以固定上游锁中的实际版本为基准；版本漂移需要独立说明及编码对照。
最终 C++ 链接使用该次构建打印的 `native-static-libs`，不猜测系统库列表。
Windows 的 `/defaultlib:msvcrt` 原样保留为链接选项。官方 windows-sys 使用的
`windows.0.52.0.lib` 从同一次 Cargo metadata 指向的 `windows_x86_64_msvc`
0.52.6 包取得，记录 registry checksum 与导入库 SHA-256，复制到构建证据目录后
按验证过的绝对路径链接。缺失或变化必须失败，不依赖开发机的额外 LIB 路径。

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

bridge 调用 core 的 `wait_stopped_checked`。HTTP、发现、组播的监督任务被真正
join 后才释放其句柄；重复或并发等待保留原来的 panic/abort 结果。取消一次等待
不会消费任务所有权，也不会让下次等待伪报成功。后台发现任务的 panic 会停止
发现，并由 checked join 保留错误，即使刷新事件队列已满。停止结果与 `STOPPED`
的 status 都会报告 `PANIC`，不能只在日志里留下错误后返回 OK。
HTTP 连接 worker 的 panic 同样进入持久失败状态，并取消监听和同组连接；不能
因 TaskTracker 已空就把该次退出当作成功。

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
`huhu_media_lease_info` 从该打开对象返回完整整数的 volume / file / bytes；Qt
PreparedMedia 在 worker 上将它与历史租约的 FileIdentity 和大小比较，拒绝
选择之后已经被替换的源，不通过 JSON double 传递文件身份。Linux 直接使用
File::metadata 的 dev / ino；Windows 的 Rust 1.97.1 MetadataExt 对应方法仍
[未稳定](https://github.com/rust-lang/rust/blob/1.97.1/library/std/src/os/windows/fs.rs)，
所以复用已在依赖锁中的 Microsoft windows-sys 0.52.0 的
[GetFileInformationByHandle 绑定](https://github.com/microsoft/windows-rs/blob/3a605cba064b26f2a198ac58085f8c8836f47c38/crates/libs/sys/src/Windows/Win32/Storage/FileSystem/mod.rs)，
只开启 Foundation / Storage_FileSystem features，直接查询租约的同一 HANDLE。
长度变化、短读、超长、读取错误和取消有不同结果；队列最多四块、每块 512 KiB。

peer 参数必须带真实 TLS 发现得到的 fingerprint，alias 不参与认证。传输请求
使用 pinning；未认证的发现仅用于取得真实证书身份，不能直接当作用户长期信任。
新接收必须经过显式确认。选择 LAN 网卡、网络类型许可、持久身份、历史删除租约
以及任务 UI 都由应用服务负责，bridge 不另建平台业务规则。

接受前的空间/策略预留失败使用窄 `huhu_transfer_reject`：只允许限额 413、存储
不可用 507 和用户拒绝 403。发送方保留实际 HTTP 状态，并分别映射 LIMIT、IO、
CANCELLED；无效错误码不消耗待决确认。这里复用标准 v2 错误响应，不添加私有
协议字段，也不让磁盘不足退化为“对方主动拒绝”。接收终态的决策所有权在唤醒
HTTP worker 前移出共享 slot，避免并发 abort 覆盖应用已经确定的拒绝原因。

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

官方协议允许相同 fileId/token 在 SHA 错误后重新上传。每次文件交接都有自己的
内部代次；旧 receive 延迟释放或完成时不能覆盖新尝试的状态。`RECEIVE_END.files`
反映每个文件最后一次被接纳的尝试；还未完成的尝试不会因前一次失败而伪装完成。

## 严格 GIF 媒体验证

`huhu_gif_validate_file` 在调用方的接收 worker 上同步工作，输入为已关闭写入者
的私有暂存文件、显式资源预算和可选 receive / 独立取消许可；不返回图片到 C++，只
返回完整成功后的尺寸、帧数、累计像素和持续时间。失败时输出清零，不提交媒体
也不发送 ACK。PNG/MP4 由共享媒体服务调用对应的严格解码路径。

不能以 Qt GIF handler 能展示图像作为完整性证据。这里实际使用固定的
[gif reader](https://github.com/image-rs/image-gif/blob/acf2f6f7e6195962a9f455dab50a278b7dabc884/src/reader/mod.rs)
和 [decoder](https://github.com/image-rs/image-gif/blob/acf2f6f7e6195962a9f455dab50a278b7dabc884/src/reader/decoder.rs)：
启用帧范围检查和 LZW END 检查；使用 Indexed 输出，逐块检查恰好等于帧矩形的
像素数和有效调色板索引。每帧读完规定像素后继续以一个像素的非空缓冲读到
DataEnd，拒绝额外有效像素。不能此时直接调用 next_frame_info，因为该调用的
OutputBuffer::None 路径可能跳过尚未处理的压缩字节。

继续读取所有帧直到上游识别 Trailer，再检查底层 BufReader 的物理 EOF；缺
Trailer 的文件由官方 reader 报 UnexpectedEof，Trailer 后的额外数据也被拒绝。
LZW END 由已锁的 [weezl decoder](https://github.com/image-rs/weezl/blob/fd444602c54a17ed3a557b455906efd8c90a5c98/src/decode.rs)
实际消费，不自行实现 GIF 或 LZW parser。分块验证不需要反交错或保留所有帧，
因为每个索引与每帧完整像素数均被检查；严格通过后才允许 Qt 生成显示封面。

预算包含输入字节、逻辑画布像素、累计解码像素、帧数和 metadata 内存。像素
缓冲固定为 64 KiB；所有累计值检查溢出，取消在读取和分块解码时检查。
`HuhuCancellation` 是只向取消状态转换的 Rust 原子句柄，用于历史导入等没有
receive 的调用。C++ worker 将 `std::stop_token` 连接到调用 request 的
`std::stop_callback`；验证结束并销毁 callback、加入所有 request 调用者后，
才通过同侧 free 释放句柄。两种取消按 OR 生效，不能以独立句柄绕过接收许可。
`LIMIT_EXCEEDED` 区别于 `INVALID_MEDIA`，并可传给 receive_complete；core 返回
标准 HTTP 413，发送/接收终态保留限额原因，不能把它显示为对方主动拒绝或磁盘错。
具体预算由共享媒体政策提供，不在桥里按某个样例尺寸降低产品的录制能力。

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

重试回归通过实际 TLS 发送同长度错误内容，收到 422 后使用同一 token 重试，
保留旧 receive 到新尝试归档报错之后再释放；第二个文件保持会话打开，最后逐项
检查新状态没有被旧句柄覆盖。core 另运行真实 Tokio task panic/abort、并发和
重复 join、取消等待后重新 join，以及通知满队列时的发现 worker 回收测试。

`tests/gif_validation.rs` 使用官方 Encoder 产生普通文件，检查每个严格前缀截断、
无 Trailer、尾随数据、缺 LZW END、额外/不足的有效像素、零尺寸、调色板错误、
interlaced 局部帧范围和资源预算。ABI 的实际 TLS 测试另验证取消许可及两帧 GIF
触发单帧接收预算后返回 HTTP 413；没有伪造成功归档。新增这些源码后仍须执行
真实双平台 CI，不能把既有 Qt 逐帧显示测试当作该严格验收已完成。

这些源码存在不表示用例已运行。实际执行结果由对应 CI 记录；本地编写环境没有
Rust/Cargo。当前聚合新增代码等待真实 CI 编译和运行，不能拿已通过的 core 测试
代替同进程 ABI 结果。真实跨机互传、组播/多网卡、全格式完整验证、正式媒体服务、
任务 UI、长期稳定性与性能基线仍属 P00/P10/P12 门槛，不触发正式发布。
