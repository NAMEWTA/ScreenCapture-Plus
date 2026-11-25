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
cargo test -p localsend --features discovery --no-fail-fast
```

此目录的 `rust-toolchain.toml` 固定 Rust 1.97.1。工作目录在仓库根时，单独传递
`--manifest-path` 不会让 rustup 自动采用这个子目录的工具链。
初始 Cargo.lock 是上游锁文件原件；首次 CI 需要由真实 Cargo 解析新增的
`infer =0.22.0` 并产出新锁文件，取回后提交，随后所有构建必须使用 `--locked`。
不得猜写 registry checksum。`infer` 关闭默认 feature，复用其 PNG/GIF/MP4
magic 检测；其 MIT 许可和 Bojan 的版权声明保留在 `infer.LICENSE`。

## 五类受控修改

| 边界 | 当前实现 | 仍需应用服务完成的工作 |
| --- | --- | --- |
| 控制报文 | Content-Length 预检、逐帧累计限额、读取超时、字段/文件数/总量上限；响应 JSON 同样有上限 | 发现列表的容量、过期、信任和用户提示 |
| 监听与路由 | `start_bound_v2` 只绑定明确选择的 IPv4 地址，TLS 和 checksum 必开；公开 v2 info/register/prepare-upload/upload/cancel | 根据用户许可选择 LAN 网卡；端口冲突备用端口与发现公告；多网卡真实环境验证 |
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

消费者必须在发布前检查取消令牌。请求被丢弃、会话取消或监听停止会使许可失效。
取消到达原子提交之后时，应保留已经发布的完整文件，并报告实际结果；不能删掉
真实归档来伪造“从未提交”。归档代码使用共享 FileCommitter，不能自行实现覆盖式
rename，也不能将最终归档路径交给上游的截断式 Path 接收器。

只有完整校验、flush/close 和无覆盖持久提交完成后，消费者才发送
`CommittedMedia`。`DurabilityUnknown` 表示最终文件已经存在但持久性尚不确定，
此时保留路径并返回失败，禁止再次归档制造重复副本。magic 检测本身不证明媒体
完整，不能用它替代 codec/probe，也不能凭“最后一块已收到”发送成功回执。

所有接收事件进入服务工作队列，UI 不负责消费文件块、完成磁盘操作或返回提交结果。
后续 bridge 必须继续保证关键事件可追踪、退出时清空旧事件并 join 所有任务；
本次 core 接缝不能替代这一 C ABI/runtime 验收。

## 验证范围

新增 `core/tests/native_v2.rs` 使用真实 loopback TLS、官方自签身份和客户端、
真实临时文件、文件同步及无覆盖提交，覆盖 ACK 时序、可选 SHA、错 SHA、长短体、
类型伪装、提交失败、持久化未知、过早完成、重名保护、读取错误、取消、稳定打开
文件、路由白名单和控制报文限额。测试消费者仅接受仓库内完整的单像素 PNG
测试图；此图由项目生成，无第三方图像版权。它不是通用媒体解码器。
“持久化未知”和读取错误用例明确注入失败状态，用来验证失败路径；其余 IO 和
网络均实际执行。上游原测试保留并一同执行。

测试源码存在不等于测试通过。实际 Rust 编译、执行结果和双平台环境应记录在
对应 CI 证据中。本地编写环境未安装 Rust/Cargo，不能据此宣称这些 Rust 用例
通过。真实双机、官方桌面互通、全格式验证、吞吐/资源基线、统一 gifski staticlib、
Qt 同进程、C ABI 压力和退出行为仍是 P00/P10 的必要门槛；这些完成前不能宣称
LAN 功能完成或触发正式发布。
