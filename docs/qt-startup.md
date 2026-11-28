# 启动参数与单实例动作转发

这是 Issue #5 的 F01/P07a 共享启动边界。它提供实际参数解析、Qt 本地 IPC 和启动结果。[统一 Qt 宿主](qt-application.md)现已选择当前用户/桌面的 runtime 与配置 scope，装配服务并执行已接收动作。启动、窗口和退出集成测试与物理桌面用户旅程的验收仍分别记录。

## 保留现有命令行语义

`src/core/Startup.h` 不依赖 Qt 或 Windows。现有 Windows `App` 和 `WinCap::enterByArg` 直接使用同一份结果；Qt 入口使用同一解析函数。参数来自固定 Ling `App::initArgs` 的实际行为，不能换成会拒绝未知选项的默认解析策略。

| 输入 | 保留的结果 |
| --- | --- |
| 无参数或未知 `--enter` | 普通截图入口 |
| `--enter=pin/long/video/qr/qrcode` | 贴图、长图、录制或二维码入口；两个 QR 名称等价 |
| `--enter=tray` 或 `--auto-start=true` | 后台启动，不主动弹出截图 |
| `--auto-quit` 或 `--auto-quit=true` | 一次性独立实例；优先于后台/托盘条件 |
| `--action=finish-capture` | 仅转发到已有同范围主实例；不因 `--auto-quit` 或后台参数变成截图实例 |
| 显式空值或未知 `--action` | 无效控制请求，明确失败，不回退为普通截图 |
| 布尔值 `false`、`TRUE`、`1` 或空值 | 不启用该标志，仅小写字面值 `true` 启用 |
| 重复 `key=value` | 后面的明确赋值覆盖前值 |
| 已有明确值之后的裸标志 | 保留前值，沿用旧 map 的只插入行为 |

参数文本只在 Qt/原生入口边界转换；同一 ASCII 参数规则覆盖窄字符与 Windows 宽字符。一次性实例不创建锁、不连接已有主实例、不抢占其监听端点；其业务结果也不能被误认为来自原主实例。

## 使用 Qt 公共 IPC 和锁

`SingleInstance` 直接依赖固定 Qt 的 [QLocalServer](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/network/socket/qlocalserver.cpp)、[QLocalSocket](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/network/socket/qlocalsocket.h) 和 [QLockFile](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/io/qlockfile.h)，不实现第二套进程锁或 OS pipe。来源是直接库依赖，没有复制 Qt 实现。

Qt 负责识别已经退出的锁进程。stale 时间设为零，不能因为主进程处理较慢就移走活锁。只有拿到主锁、且正常监听明确返回地址被占用时，才允许清理该 scope 的旧端点。Windows 使用当前用户权限的 named pipe；Linux 使用用户 runtime 内的实际 Unix socket 和 0700 私有目录，不使用忽略权限的 abstract namespace。Linux 采用普通 bind，避免 Qt 显式权限模式的临时 socket rename 覆盖已有路径；监听前拒绝普通文件、符号链接或其他非 socket。崩溃清理只允许同 scope 的实际 socket，受同一主锁协调；这不是隔离同用户恶意持续替换私有路径的沙箱。scope 与规范化 runtime 共同生成端点名；过长的 Unix socket 路径明确失败，不偷偷退回全局临时目录。

整个过程在 Qt 事件循环中使用信号和定时器，禁止调用 `waitForConnected`、`waitForReadyRead`、`waitForNewConnection` 或嵌套事件循环。锁竞争只调用无等待 `tryLock(0)`；连通前允许有限重试，三秒截止。服务最多跟踪八个已接受连接，每个只读取最多 25 字节；不完整请求也有三秒截止。

报文只包含协议版本、已定义的启动入口/控制动作、后台标志和随机关联 ID，不接受文件路径、任意命令或进程句柄。普通启动保留原 v1；有界 v2 的 Finish/Verify 复用同一个 24 字节报文和 ACK 关联值。控制请求只连接已有端点，不选主、建锁或清除端点。[桌面动作验证](desktop-capture-action.md)的关联值只能消费一次，Verify 不执行停止。主宿主的快速 admission 回调返回是否接管动作；确认不替代后续采集、归档或复制结果。

## 失败与退出

| 情况 | 对调用方的结果 |
| --- | --- |
| 首个实例获得锁并开始监听 | `Primary`，宿主执行自身启动动作 |
| 一次性调用 | `Independent`，不参与主实例仲裁 |
| 已存在实例明确接收动作 | `Forwarded`，新调用者可以退出 |
| 主宿主忙碌或正在停止，明确拒绝 | `Rejected`，不伪报截图成功 |
| 活锁存在但监听不可用 | 有限等待后 `PrimaryUnavailable`；不强行成为第二个主实例 |
| 请求写出后断开、缺确认或异常确认 | `DeliveryUnknown` 或 `InvalidReply`；可能已执行，不自动重发 |

不在已经尝试写出动作后自动接管或重发，避免丢失确认造成两次截图。主动重试属于明确用户操作，需要宿主告知之前动作结果不确定。关闭接收能力后一直持有主锁，直到协调器析构，避免旧进程还在释放录制/剪贴板/传输资源时启动第二个主应用。

admission 或启动结果处理器可以同步关闭窗口、停止接收甚至销毁协调器；跨回调用 `QPointer` 检查寿命。所有客户端连接均有结束路径，退出先停止监听和定时器，再关闭客户端，最后释放锁。

admission 回调只有明确返回 `false` 才表示拒绝；抛异常可能发生在动作已交接之后，因此关闭连接、保留投递结果不确定，而不回传虚假的拒绝确认。独立进程测试覆盖交接后抛异常，确保只观察到一次激活动作且客户端不重发。

## 实际验证入口

`qt_single_instance` 运行独立的真实 Qt 子进程，通过实际本地 socket/pipe 检查参数动作转发、一次性旁路、主宿主拒绝、处理时析构导致的丢确认、实际进程崩溃后恢复、停止接收但持有主锁、过量/不完整请求、并发争抢及等待期间的事件循环 heartbeat。协调器不会用模拟服务回复代替这些行为。

该验证不执行屏幕截图，也不证明完整产品入口、物理热键、托盘菜单或 UI 焦点已经完成。运行结果以对应构建的实际 CTest 记录为准；尚未运行的配置保持未验证。
