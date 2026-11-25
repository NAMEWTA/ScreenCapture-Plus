# 可复现的 Wayland 采流测试桌面

`scripts/build-validation-desktop.py` 只为 CI 的受控桌面构建固定 Sway、wlroots
和 Portal-wlr。生产应用不携带这些 compositor 或 Portal daemon，安装时继续
使用用户桌面的真实 Portal。应用的固定 PipeWire client、系统 PipeWire daemon
与此测试桌面的动态库环境分别管理，不因构建顺序或开发者 PATH 混用。

## 为什么同时固定 compositor 与 backend

Portal-wlr 0.8.1 的 CPU/SHM 路径使用 `ext-image-copy-capture-v1` 与
`ext-image-capture-source-v1` 的 output source manager。旧 `wlr-screencopy` 分支还要求 compositor
提供 `linux-dmabuf`；只启动旧 Sway 的 headless/pixman 并不能满足这个条件。
实际日志出现“没有可用截图协议”时，不能更改应用帧内容、跳过正向验证或将
后端初始化失败描述为真实采流成功。

源码依据是固定 Portal-wlr 的
[`wlr_screencast.c`](https://github.com/emersion/xdg-desktop-portal-wlr/blob/e1d5d16f0e064a0b788c5d70c11461b15ee7a4af/src/screencast/wlr_screencast.c)、
固定 Sway 1.11 的
[`server.c`](https://github.com/swaywm/sway/blob/990b23c0e584114c3694447344d6bbf44b2d748f/sway/server.c)
与 wlroots 0.19 源码导入中的
[`wlr_ext_image_copy_capture_v1.c`](https://github.com/deepin-community/wlroots/blob/826be6494c201ca887cddd8deb07d70c13595dd2/types/wlr_ext_image_copy_capture_v1.c)。
后者从实际输出帧复制到客户端 buffer，区分 SHM/DMABuf，并核对尺寸与输出约束；
测试不替换此生产协议实现。

| 测试桌面组件 | 固定版本 | 来源与作用 |
| --- | --- | --- |
| Sway | 1.11 | 官方仓库；提供真实输出、窗口与新采流协议 |
| wlroots | 0.19.0 | Deepin 源码导入；真实 headless backend、pixman renderer 与 SHM capture |
| Wayland | 1.23.1 | Wayland GitHub 镜像；满足 wlroots 的 server/scanner 最低要求 |
| wayland-protocols | 1.41 | Wayland GitHub 镜像；固定生成协议的 XML 和版权声明 |
| libdrm | 2.4.124 | Deepin 源码导入；满足实际源码的格式、修饰符与 API 要求 |
| pixman | 0.44.0 | Deepin 源码导入；实际软件渲染，满足 wlroots 的版本要求 |
| libinput | 1.26.0 | Deepin 源码导入；Sway 公共配置与命令使用的完整输入类型，硬件 backend 仍关闭 |
| Portal-wlr | 0.8.1 | 官方仓库；真实 ScreenCast 与 PipeWire producer |

完整 commit、Meson 选项、canonical upstream 地址和许可证路径均在
`build/dependencies.lock.json` 的 `headlessPortalValidation` 中。
Deepin 导入 commit 是镜像中的实际源码身份，不能称为原 upstream Git commit；
脚本不执行 Debian 打包规则或应用其补丁目录。所有 checkout 必须匹配完整
commit，禁止 Meson fallback 自动下载。现有 Portal-wlr POSIX 声明补丁仍按
SHA256 核对后应用。版权文本复制到私有安装树的
`share/licenses/validation-desktop`，源码 checkout 和构建记录保留精确归属。

## 环境与生命周期

构建仅写 `.deps/validation-desktop`，拒绝复用非空工作区，不覆盖系统库。
关闭 DRM/libinput/X11 硬件 backend、GPU renderer、Xwayland、session 和无关
桌面组件；wlroots 的 headless/Wayland backend、pixman renderer 与 SHM allocator
仍是上游真实实现。此配置不能证明 GPU、输入设备或物理显示器支持。

Sway 即使关闭 libinput backend，仍会在公共配置和输入命令中无条件包含
`libinput.h`，其中 clickfinger 映射枚举从 libinput 1.26 才存在。不能只补一个
旧系统头文件或伪造这些声明。私有前缀先由固定完整 libinput 源码构建并安装，
Sway 的 Meson 功能探测和编译都从该前缀取头文件；宿主只提供其 libudev、
mtdev、libevdev 构建依赖。此库及工具也只属于测试桌面，不随产品分发。

`build-info.json` 记录所有固定来源、实际版本、Sway/swaymsg/Portal-wlr 的
SHA256 与 `ldd`、私有动态库及符号链接。运行 wrapper 先核对当前 lock、
receipt、binary 和完整私有库目录。只有这三个明确选择的程序获得测试桌面
`LD_LIBRARY_PATH`；系统 DBus、PipeWire、WirePlumber 和 Portal frontend
保留系统库，应用 probe 则使用自己的固定 Qt/libportal/PipeWire client。

每次场景都有独立的 0700 runtime、会话 bus、桌面、daemon 和日志；resize
只向该 Sway IPC socket 发命令，daemon-loss 只终止本轮创建的进程。完成或
失败均倒序清理，并核对本轮 document-portal 的 FUSE mount，不终止用户服务。

## 测试能证明的范围

相同 wrapper 驱动生产 `pipewire_capture_probe` 和共享实时源 probe，核对真实
绘制 surface 的收到像素、SHM 帧归还、输出尺寸变化与实际 daemon 丢失。
初始 source preview、后续刷新和录制控件隐藏仍需各自的实际像素断言；
成功启动这个桌面不是这些断言的替代品。

CI 保存构建日志、许可证、环境 receipt、每次服务日志、收到的 PNG 和结果。
这套 headless 桌面不能替代 GNOME/Mutter、Plasma/KWin 的授权、真实多屏混合
DPI、输入法、音视频设备、DMA-BUF/HDR 或安装升级验收。脚本语法检查也不
等于在真实 Wayland/PipeWire 环境中运行通过。
