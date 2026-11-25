# 文档地图

产品安装、操作与下载选择从根目录 [README](../README.md) 开始。这里记录代码不容易表达的设计理由、协作约束、失败语义和验收边界，避免为函数清单维护第二份容易过时的说明。

| 想解决的问题 | 阅读入口 |
| --- | --- |
| Qt 重构完成到哪里，哪些能力尚不能当作产品交付？ | [迁移状态与保护边界](qt-migration.md) |
| 启动参数、一次性实例与主实例动作转发怎样保留原有语义？ | [共享启动与本地 IPC](qt-startup.md) |
| Qt 编辑器怎样保留九工具与原版焦点、参数和输出语义？ | [共享编辑器合同](qt-editor.md) |
| 设置、历史、关于页怎样共用控件并等待真实服务结果？ | [共享桌面窗口](qt-desktop.md) |
| 剪贴板如何确认发布，回收和文件定位如何保留真实结果？ | [桌面系统服务](desktop-integration.md) |
| 编辑输出、历史预览和清理怎样由应用连接真实服务？ | [媒体工作区](media-workspace.md) |
| 二维码怎样共享原解码器、字符转换和像素预算？ | [二维码识别](qr-recognition.md) |
| 如何构建固定 LocalSend core，验证真实 TLS 与文件提交？ | [LocalSend core 适配](localsend-core.md) |
| GIF 与 LocalSend 怎样只链接一个 Rust 库，CRT 和线程如何约束？ | [Rust 聚合库与原生 ABI](rust-bridge.md) |
| 文件传输如何预留空间、展示进度并等待真实归档终态？ | [共享传输服务](transfer-service.md) |
| 设备选择、证书确认、接收和任务窗口怎样连接同一传输服务？ | [局域网共享界面](lan-interface.md) |
| 私有身份、网卡变化、任务和确认窗怎样由应用统一拥有？ | [局域网应用控制层](lan-controller.md) |
| 截图九宫格调整怎样复用同一源像素选择模型？ | [截图选择模型](capture-selection.md) |
| Linux Portal 如何与 Qt 共用上下文，怎样验证取消与父窗？ | [Linux Portal 控制面](linux-portal.md) |
| PipeWire 帧如何归还、跨线程持有并验证实际采流？ | [Linux PipeWire 数据面](linux-pipewire.md) |
| 软件 H.264 / AAC 如何固定构建，API 探针能证明什么？ | [软件编码依赖](software-codecs.md) |
| MP4 完整检查为何区别于首帧封面，坏样本怎样跨平台验证？ | [视频检查与封面](video-inspection.md) |
| 怎样用固定源码生成实际 HEVC Main / Main10 回归输入？ | [HEVC 样本与复用记录](hevc-fixtures.md) |
| 项目继承自哪里，如何保留上游归属与许可？ | [上游致谢](upstream.md) |
| 一个需求应该放在哪个模块、由谁拥有资源？ | [架构与扩展](architecture.md) |
| 文件、目录、代码与文档应该怎样命名和组织？ | [工程约定](conventions.md) |
| 截图与视频如何长期保存，历史操作影响哪些文件？ | [媒体历史](media-history.md) |
| 设置与媒体历史如何保持一致的字体、颜色与交互？ | [界面设计](design.md) |
| 为什么批注保持稳定编号，导出为何会增加画布？ | [批注设计](annotations.md) |
| 点击更新后文件保存到哪里，失败或重名怎样处理？ | [更新下载](updates.md) |
| 怎样构建、发布、辨别发行文件和定位问题？ | [构建与发布](building.md) |
| 自动检查证明了什么，还需要怎样的桌面验收？ | [验证边界](validation.md) |
| 怎样维护图标，哪些资产是源文件？ | [品牌资源](brand.md) |
| 怎样添加语言并保持编码、回退与布局正确？ | [语言约定](languages.md) |

本目录 Markdown 文件名全部小写，使用短横线连接单词；语言后缀也使用小写，例如 `readme.en-us.md`。根目录的 `README.md`、`AGENTS.md`、`CONTRIBUTING.md`、`CHANGELOG.md` 是固定的发现入口。第三方许可、源码名称和运行时 `Lang` 资源名称保持各自的契约。

文档修改应随行为变更一起提交。当前行为写入主题文档，版本差异写入 [更新记录](../CHANGELOG.md) 和 `releases/<tag>.md`；具体执行结果留在对应提交的 Actions、测试报告或审阅记录中。
