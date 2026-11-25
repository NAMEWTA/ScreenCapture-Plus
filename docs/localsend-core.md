# LocalSend 核心接入边界

## 固定来源与构建

`third_party/localsend/core` 来自 LocalSend 官方仓库的
[`44f48f18a5503c43873570ff489d27ff1983e198`](https://github.com/localsend/localsend/tree/44f48f18a5503c43873570ff489d27ff1983e198/packages/core)，
对应 Issue #5 固定的 LocalSend v1.19.0，crate 名称和版本保持 `localsend 0.1.0`。
保留全部上游 core 源码、测试、资源及 Apache-2.0 许可证；只使用 `discovery`
feature，不启用 `full` / WebRTC，不引入 Flutter、Dart、CLI 子进程。
协议依据是固定的
[LocalSend v2.2](https://github.com/localsend/protocol/blob/62bd3406ec80d62f2ed46269cdc06c4dcc391083/README.md)。

原始文件的 Git blob SHA 和大小见
[upstream-files.json](../third_party/localsend/upstream-files.json)，目录映射和额外依赖见
[upstream.json](../third_party/localsend/upstream.json)。这些 SHA 描述上游原件；
本项目的受控差异另存于 `patches/native-core.patch`，修改过的源码带有标记。
不得用重新下载依赖覆盖这些差异，也不得在 `.deps` 中手工补丁。

独立验证命令：

```sh
cd third_party/localsend
cargo test -p localsend --features discovery --locked --no-fail-fast
```

此目录的 `rust-toolchain.toml` 固定 Rust 1.97.1。工作目录在仓库根时，单独传递
`--manifest-path` 不会让 rustup 自动采用这个子目录的工具链。
core 阶段的 Cargo.lock 由真实 Windows/Linux Rust 1.97.1 CI 从上游锁文件生成，两平台
输出逐字节一致：新增 `infer =0.22.0`，移除独立 workspace 不再需要的上游 App
依赖，保留其余包的源与校验值。生成来源记录在 `upstream.json`；后续构建使用
`--locked`。新增 [Rust 聚合成员](rust-bridge.md) 后，须再次由真实 Cargo 解析
gifski 依赖并回收双平台锁，再恢复 locked 构建；不能拿旧锁冒充聚合验证结果。
不得猜写 registry checksum。`infer` 关闭默认 feature，复用其 PNG/GIF/MP4
magic 检测；其 MIT 许可和 Bojan 的版权声明保留在 `infer.LICENSE`。

## 五类受控修改

| 边界 | 当前实现 | 仍需应用服务完成的工作 |
| --- | --- | --- |
| 控制报文 | Content-Length 预检、逐帧累计限额、读取超时、字段/文件数/总量上限；响应 JSON、UDP 公告及发现缓存同样有上限 | 设备过期、持久信任和用户提示 |
| 监听与路由 | `start_bound_v2` 只绑定明确选择的 IPv4 地址，TLS 和 checksum 必开；公开 v2 info/register/prepare-upload/upload/cancel；`discovery::start_secure` 限定明确的接口白名单并拒绝 HTTP 对端 | 根据用户许可选择 LAN 网卡；端口冲突备用端口与发现公告；多网卡真实环境验证 |
| 发送生命周期 | `FileContent::File` 转移已经打开的普通文件；`ResultStream` 保留读取错误，默认四块、每块至多 512 KiB | MediaLease 防止应用自身清理/替换；哈希和发送使用同一稳定内容；取消会话与 C ABI 所有权 |
| 接收校验 | 累计精确长度、可用 SHA256、扩展名与 MIME、magic 一致；上传并发受限 | PNG/GIF/MP4 的完整 codec/probe 验证及资源预算 |
| 成功 ACK | `VerifiedStream` 先发送校验结果，再等待明确的归档完成回执；错误 SHA 为 422；过早回执、写入失败、持久化不明均不返回 200 | 在后台执行完整媒体验证及 FileCommitter 无覆盖持久提交，接入历史索引与 UI |

上游的 HTTP/web/v3 入口和旧 `Path` / `Stream` API 保留供 upstream 测试回归；
生产适配只允许上述严格入口和 `VerifiedStream`。严格接收端拒绝原始 Path 或
未经校验的 Stream 目标。没有新增第二套 TLS、发现或协议实现。

## 校验、提交和取消

接收消费者将有界块流写入本次排他拥有的暂存文件。字节队列关闭只代表流结束；
消费者必须继续等待 `verification`。只有成功校验许可才允许执行完整媒体验证和
最终归档。许可包含实际长度、本地 SHA256、是否与发送端 digest 比对，以及取消令牌。
发送端省略可选 SHA 时仍计算本地哈希，不能向用户声称已经完成发送端 SHA 比对。

这里的许可不是普通可替换的 oneshot receiver：受控 receiver 记录应用已经实际
消费成功许可，受控 completion sender 在此之前拒绝任何成功回执，并令 HTTP
返回失败。即使小文件已经完成接收、许可已经排队但消费者尚未读取，也不能越过
此门。该区别修复了首轮真实 Linux TLS 测试揭示的提前完成竞态；原来只检查一次
completion 队列是否为空不能建立这个约束。通道的 server half 是不可外部构造的
类型，不能通过另建原始 sender 绕过检查。

消费者必须在发布前检查取消令牌。请求被丢弃、会话取消或监听停止会使许可失效。
取消到达原子提交之后时，应保留已经发布的完整文件，并报告实际结果；不能删掉
真实归档来伪造“从未提交”。归档代码使用共享 FileCommitter，不能自行实现覆盖式
rename，也不能将最终归档路径交给上游的截断式 Path 接收器。

只有完整校验、flush/close 和无覆盖持久提交完成后，消费者才发送
`CommittedMedia`。`DurabilityUnknown` 表示最终文件已经存在但持久性尚不确定，
此时保留路径并返回失败，禁止再次归档制造重复副本。magic 检测本身不证明媒体
完整，不能用它替代 codec/probe，也不能凭“最后一块已收到”发送成功回执。

所有接收事件进入服务工作队列，UI 不负责消费文件块、完成磁盘操作或返回提交结果。
bridge 保留可 drain 的终态事件并 join 所有任务；其接口和独立实际验证见
[聚合库说明](rust-bridge.md)。core 接缝测试不能替代 C ABI/runtime 验收。

严格服务器在接收会话前预留一个终态事件位置，另为监听故障预留一个位置。
队列拥塞时拒绝新会话，不把完成任务无限排队，也不让注册通知挤掉终态结果。
严格请求的取消清理同步完成，不在 `wait_stopped` 后再派生清理任务。
发现缓存最多保留 256 个设备、每设备 16 个通道及 100 条有界日志；它是可丢弃的
发现缓存，不能作为持久信任库。公告回拨最多 16 个，所有发现 HTTP 请求合计
最多 50 个；停止时取消并等待这些后台任务。通知可以合并，设备状态及组播运行
失败仍可查询，不能只依赖可能被合并的 UI 刷新通知。

原有 `wait_stopped` 保留上游调用兼容；原生 bridge 使用
`wait_stopped_checked` 获取真实 join 结果。三类监督任务共享按引用等待的
JoinHandle 所有权，等待 future 被取消时不丢失任务；panic/abort 的错误对重复
和并发调用保持可见。发现后台任务也捕获 panic、停止接纳新任务、取消并 join
已有工作。组播接收 worker 的 panic 与普通网络不可用区分记录，不能将失败
当作正常 shutdown。原生 bridge 在销毁 runtime 后才发出最后的 STOPPED。

HTTP 连接 worker 的 TaskTracker 本身只跟踪退出、不返回 panic 结果，因此在
连接 future 外显式捕获 unwind，记录不可清除的失败并取消同组连接和监听器。
严格监听器保留的故障事件负责通知应用，checked join 再次核对该错误。已中毒的
会话锁只在停止清理时恢复访问，以释放资源；不能恢复处理业务请求。

## 验证范围

时间戳格式测试保留原有精确断言与往返断言。Windows 的 Rust `SystemTime`
使用 FILETIME 的 100 ns 刻度，不能要求它输出入参转换前已被舍去的纳秒；依据
[Rust 1.97.1 实现](https://github.com/rust-lang/rust/blob/1.97.1/library/std/src/sys/pal/windows/time.rs)
调整 Windows 的期望精度，Linux 仍检查完整纳秒。生产格式化代码没有降低精度。

新增 `core/tests/native_v2.rs` 使用真实 loopback TLS、官方自签身份和客户端、
真实临时文件、文件同步及无覆盖提交，覆盖 ACK 时序、可选 SHA、错 SHA、长短体、
类型伪装、提交失败、持久化未知、过早完成、重名保护、读取错误、取消、稳定打开
文件、路由白名单和控制报文限额。测试消费者仅接受仓库内完整的单像素 PNG
测试图；此图由项目生成，无第三方图像版权。它不是通用媒体解码器。
“持久化未知”和读取错误用例明确注入失败状态，用来验证失败路径；其余 IO 和
网络均实际执行。上游原测试保留并一同执行。

测试源码存在不等于测试通过。实际 Rust 编译、执行结果和双平台环境应记录在
对应 CI 证据中。本地编写环境未安装 Rust/Cargo，不能据此宣称新改动已经通过。
新增的 Pending 取消处理和哈希取消后 join 文件 IO 仍需跟随聚合库再次执行回归。
checked join 的新增测试使用实际 Tokio worker 的 panic/abort，检查清理已执行，
取消等待后仍能 join，满事件队列不丢失错误结果；源码写入不等于真实 CI 已通过。
HTTP worker 回归先使实际会话锁中毒，再由真实 TLS prepare-upload 请求触发
连接 task 的 unwind，检查监听故障、重复 checked join 的错误及原端口重新绑定。
测试明确注入内部故障，没有给生产路径安装 panic 开关或替换 HTTP/TLS 服务。
WebRTC signaling 的连接、回调和辅助发送函数只在原有 feature 启用时编译，
保留其 DTO 测试；不压制整个 crate 的警告，也不为消除警告启用无关运行时。
真实双机、官方桌面互通、全格式验证、吞吐/资源基线、统一 gifski staticlib、
Qt 同进程、C ABI 压力和退出行为仍是 P00/P10 的必要门槛；这些完成前不能宣称
LAN 功能完成或触发正式发布。
