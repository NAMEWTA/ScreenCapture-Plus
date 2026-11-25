# 共享设置、全局快捷键与自启动

本模块落实 Issue #5 的 F01 / F12 应用接缝，复用同一个
`SettingsStore`、`LanguageCatalog` 和 `SettingsWindow`。窗口不注册系统快捷键，
不修改注册表或桌面文件，也不拥有另一个配置仓库。应用持有
[SettingsController](../src/app/SettingsController.h)，截图、归档目录、局域网设置、
链接和更新动作继续交给既有应用服务。关闭设置窗口只释放视图，不关闭活动快捷键。

## 状态和持久化边界

`SettingsStore::load()` 必须先成功；未来 schema、损坏文件和失败读取不能由控制器
重建默认配置掩盖。已有 `common.autoStart`、`common.language`、`shortcutKey.cap`
的名称保持不变。每次修改仍调用共享 store 的单键事务，保留未知字段和原有备份规则。

配置与操作系统没有共同事务。修改快捷键时先获得真实候选注册，再写配置，最后释放旧
注册；保存失败清理候选并保留旧注册。修改自启动时先修改本应用的系统条目，再写配置；
保存失败尝试恢复原始条目。若外部程序已经改写系统条目，回滚不覆盖它，界面显示实际
结果及恢复失败。这里不承诺跨文件、跨注册表或断电原子性。

初始化只查询实际自启动状态。保存的 `autoStart=true` 不会隐式注册新的启动项，
`false` 也不会移除用户在系统中设置的条目。用户明确操作开关后才会修改系统状态。
应用重新激活或设置页重开时刷新实际状态。语言修改先保存选择，再加载同一个 catalog；
损坏的外部翻译使用内嵌英语回退，保留所选语言。无法加载任何可用资源时不能显示保存
成功，也不能清空已有翻译。

应用把实际归档目录、局域网入口能力、链接能力、版本与更新快照传给控制器。无法执行
的能力保持禁用；控制器不根据 UI 存在就声称后台服务可用。初始化和 Portal 请求各有
真实终态，`shutdownFinished` 在待决初始化和绑定回调排空后发出。

## 同一快捷键服务，两种原生接缝

[GlobalShortcut](../src/services/desktop/GlobalShortcut.h) 只负责一个稳定动作
`capture`，共享配置、规范名称解析和 typed 错误。平台代码仅提供实际注册、激活、
暂停和资源释放。`Ctrl++` 的最后一个 `+` 是旧配置中的数字键盘加号，不能按分隔符
拆成空键。沿用已有保留快捷键规则、F1 默认值和历史迁移规则，不重新迁移用户明确
设置的 `Ctrl+Alt+A`。

| 会话 | 实际能力 | 设置交互 |
| --- | --- | --- |
| Windows 原生 Qt 平台 | `RegisterHotKey` / `WM_HOTKEY` | 共享按键录入控件 |
| 真正 X11 / xcb | 独立 XCB 连接的被动按键抓取 | 相同录入控件 |
| Wayland 或可识别的 XWayland | 既有 `PortalShortcuts` / libportal | 系统授权与快捷键选择器 |
| 没有可用全局能力 | typed Unavailable / Unsupported | 可复制的真实截图命令和共享截图入口 |

不使用本地 `QShortcut` 伪装全局快捷键。检测到 Wayland 环境或 XWAYLAND 扩展时不
对 XWayland 根窗口抓键。能力不足时给出的命令使用持久程序路径和既有默认启动截图
语义；它会通过正式单实例入口送达截图请求，命令文本本身不会在设置服务中执行。

### Windows

复用旧 `Setting` / `WinSettingShortcut` 和固定 Ling `Util::strToKey` 的规范名称及
Win32 键码映射；数字键盘运算符含义保持不变。一个隐藏的 Qt 原生窗口持有注册，
候选和旧绑定使用不同 ID；只接收属于该窗口和活动绑定的 `WM_HOTKEY`。按键录入
期间实际撤销本应用的注册，结束时恢复；若此时被其他程序抢占，显示恢复失败而非
继续声称已注册。F12 按 [微软 API 约束](https://learn.microsoft.com/en-us/windows/desktop/api/winuser/nf-winuser-registerhotkey)
作为调试器保留键拒绝。

与旧直接注册相比，`MOD_NOREPEAT` 明确把长按限制为每次按下一次截图请求，避免自动
重复造成多个并发请求。丢弃旧注册排队的消息，并检查真实原生错误码；不解析英语错误
文本。若撤销原生注册失败，销毁服务自己的隐藏窗口以释放全部相关注册，报告不可用
或恢复失败，不遗留一个界面看不到但仍占用其他程序按键的注册。

### X11

被动抓取、锁定修饰键组合和失败撤销流程基于已阅读的 QHotkey X11 实现；来源与许可
见 [固定记录](../third_party/qhotkey/upstream.md)。本项目使用 XCB checked requests
代替进程全局 Xlib 错误处理器，使用 xcb-keysyms / xkbcommon 读取实际键盘映射。
按所有实际根窗口和当前 Alt / Super / Caps / Num / Scroll Lock 映射注册，检查
每个 grab 的返回结果。最多 16 个根窗口、32 个同符号键码、256 个 grab；超过边界
明确不支持，不进行无界注册。

候选与旧绑定共用的 grab 不会在失败回滚时被误撤销。checked ungrab 确认服务端完成
释放后才返回，其他客户端可实际获得该键。MappingNotify 后重新计算注册；重绑失败
停止分发已经不再代表所选符号的旧键码。Qt socket notifier 分发此连接的事件，拒绝
SendEvent 伪造标志、抑制自动重复并按用户绑定代次丢弃旧的排队激活。映射重查后还
检查排队按键对应的根窗口、键码和修饰键 grab 是否仍有效；不能仅因增加了其他锁键
组合就丢弃有效的第一次按键。X server 在切换
到输入设备时也可能发出这类通知，见已阅读的
[CopyKeyClass / XkbDeviceApplyKeymap](https://github.com/mirror/xserver/blob/fc625fe172d9f6a149a594b5214364bedf680239/Xi/exevents.c)
和 [XkbSendLegacyMapNotify](https://github.com/mirror/xserver/blob/fc625fe172d9f6a149a594b5214364bedf680239/xkb/xkbEvents.c)。
映射重查中的同步 XCB reply 可能已经缓存按键事件，因此无变化重查后仍正常排空事件，
不能只等待文件描述符下一次可读。应用退出断开自己的 XCB
连接，服务端释放该客户端全部抓取。它不改变桌面其他客户端的键盘状态。

### Wayland / Portal

应用设置稳定 desktop file ID `io.github.NAMEWTA.huhucapture`，动作 ID 为 `capture`。
偏好触发字符串按 [XDG shortcuts 规范](https://specifications.freedesktop.org/shortcuts/latest/)
转换；界面始终展示 Portal 实际返回的 `trigger_description`。授权获得且设置保存成功
后，在 `shortcutKey.portal.configured` 保存一次配置标记，保留这个对象的未知字段；
不会把系统的展示文案写成能由本地按键录入解释的配置。

首次使用由用户从设置入口打开系统 chooser。已有配置标记时，启动会尝试恢复绑定。
协议允许显式空 parent，后台恢复走原 libportal API 的空 parent 请求；它仍可能
显示真实授权对话框，等待期间明确显示处理中，不能据此声称已经记住授权。普通设置
操作继续走真实可见 QWindow 的异步 PortalParent 导出，不伪造 `wayland:` 句柄。

候选会话完全授权后才调用配置提交函数。提交失败保留旧 session；提交回调前后持有
共享 operation/state 并重新检查取消、owner、parent、候选会话寿命。关闭尚在授权的
设置窗口取消候选；关闭已经完成授权的窗口不撤销活动会话。服务或总线消失报告真实
不可用，重开设置允许重新查询能力。Portal 仍由 [既有模块](linux-portal.md) 实现，
没有第二套 D-Bus 协议或后台主循环。

激活 token 仅随 `captureRequested` 临时转交应用的窗口激活接缝，不记录到配置或日志。
GNOME / Plasma 对同一应用多个 session 的冲突策略、授权恢复和真实按键触发仍需实际
桌面验收；保留旧 session 的代码结构不等于已证明所有 compositor 会继续触发旧绑定。

## 自启动与可执行路径

[AutoStart](../src/services/desktop/AutoStart.h) 只修改本应用自己的用户级条目。
构造和查询不会创建目录、锁文件、注册表值或计划任务。应用提供有效的持久程序路径；
AppImage 必须传 `APPIMAGE` 指向的外层文件，不保存 `APPDIR` 挂载目录内的二进制。
不存在、不可执行、非绝对或含换行 / NUL 的路径禁止注册。

Windows 继续使用旧项目的
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` / `huhucapture` 值和
`--auto-start` 参数。只读写这一个值，保留其精确原始类型和字节用于回滚；正常写入为
正确引用的 `REG_SZ`。旧 `huhucapture.exe --auto-start` 条目可以迁移到当前程序路径。
同名但不属于本程序的条目不删除。查询能证明当前 Run 注册及目标文件存在，不能证明
系统策略或任务管理器允许下次登录启动；这部分需要真实登录验收。

Linux 使用 XDG 用户配置目录中的 `autostart/huhucapture.desktop`，并按系统配置目录
优先级只读查询继承条目。复用 GLib `GKeyFile` / `GDesktopAppInfo`，保留未知字段、
注释及翻译；仅接受带 `X-huhucapture-AutostartVersion=1` 的自有条目，不覆盖同名
外部文件、符号链接、非普通文件或超过 64 KiB 的文件。明确启用时更新本应用的 Exec、
TryExec、Hidden 等字段并移除旧 OnlyShowIn / NotShowIn 限制。明确禁用使用用户
`Hidden=true` 覆盖已有自有条目，防止删掉用户文件后重新启用系统目录中的条目。
系统目录文件不写入。目录数量上限 32；写入采用同一文件的 QLockFile 和禁止直接
覆盖回退的 QSaveFile，并检查写前与回滚前实际字节。

Exec 转义依据 [Desktop Entry 规范](https://specifications.freedesktop.org/desktop-entry/1.4/exec-variables.html)，
双引号、反斜杠、美元符号、反引号与 `%` 分别处理。实际阅读
[GLib 2.80.0 的 launcher](https://github.com/GNOME/glib/blob/2.80.0/gio/gdesktopappinfo.c)
发现它在展开 `%%` 前先校验 argv[0]：程序路径含字面百分号时，合法的转义仍会令
该预检查失败。因此仅此情况使用系统 `/usr/bin/env` 或 `/bin/env` 的 `--` 启动参数
形式，让实际程序路径在字段展开后传给 env；TryExec 仍指向真实程序。没有 shell
执行或驻留辅助程序。本地真实 GLib 启动验证已覆盖中文、空格、美元符号和百分号。

## 验证范围

[设置控制器测试](../tests/qt/settings_controller_tests.cpp) 使用生产服务、真实原生
注册、真实文件保存和真实启动命令。Windows 使用进程范围的 `RegOverridePredefKey`
隔离测试 HKCU；Linux 在私有 XDG 目录中写文件。它们不修改用户真正的登录启动项。
Windows SendInput / Linux XTEST 生成实际 OS 输入，验证全局激活、冲突、录入期间
释放、保存失败后旧注册仍占用、候选释放和退出清理。实际 Qt 控件驱动设置保存与
多语言切换，并输出真实窗口 PNG；这类输入不等于物理键盘、输入法或混合 DPI 验收。
X11 测试逐项检查真实 XTEST 请求的原生错误和结束后的按键释放状态，并在私有测试
服务器原样重提交现有键盘映射，验证真实 MappingNotify 后第一次输入仍可触发。
测试启用 `huhucapture.shortcuts.x11` 的 debug 分类，保留实际 focus、映射通知、
grab 集变化和激活判定用于诊断原生失败。正式程序默认不启用这一分类；不记录其他
按键的字符或按键内容。

Linux 生产依赖是已有 Qt、Portal、GIO 加 xcb / xcb-keysyms / xkbcommon；
`xcb-xtest` 只用于测试。测试必须在真正 X11 / Xvfb 下运行原生分支，不能用 offscreen
替代；独立 `--unavailable` 分支在 offscreen 下验证能力禁用、可复制命令与共享截图
入口。Windows 链接 user32、advapi32、shell32，不增加常驻外部运行时。

CMake 的 `HUHUCAPTURE_NATIVE_INPUT_VALIDATION` 默认开启，双平台 Qt CI 保持开启。
开发机缺少 XTEST 开发文件时，可显式关闭它以单独编译生产控制器；这会排除该原生
输入测试目标，不能据此记录完整 Qt 测试通过。Xvfb/真实桌面仍是对应测试的运行条件。

本地已实际完成 Qt 6.12.1 头文件、moc 和生产设置控制器的编译链接检查，以及生产
Linux AutoStart 的真实文件事务和 GLib 子进程启动检查。独立 offscreen smoke 已
实际运行 SettingsController / SettingsWindow：验证能力禁用、真实可复制命令、
共享截图动作、自启动 UI 的实际 XDG 文件与共享配置保存、关闭清理，并检查实际
生成的窗口 PNG；该 smoke 没有执行原生热键。Portal 故障程序已构建，缺失 session bus 检查已
运行通过；本地环境无法创建 D-Bus 监听 socket，缺失服务及断线检查不能记为通过。
完整控制器测试、Windows 原生行为与 X11 注入以集成后实际 CI 结果为准。Wayland
chooser、真实登录自启动、物理热键、外部系统策略及长时间运行尚需实机验收。
以上组件结果不能作为 P12 完整产品安装、升级、卸载或正式发布的通过证据。
