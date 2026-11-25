# 文档地图

产品安装、操作与下载选择从根目录 [README](../README.md) 开始。这里记录代码不容易表达的设计理由、协作约束、失败语义和验收边界，避免为函数清单维护第二份容易过时的说明。

| 想解决的问题 | 阅读入口 |
| --- | --- |
| Qt 重构完成到哪里，哪些能力尚不能当作产品交付？ | [迁移状态与保护边界](qt-migration.md) |
| 如何构建固定 LocalSend core，验证真实 TLS 与文件提交？ | [LocalSend core 适配](localsend-core.md) |
| Linux Portal 如何与 Qt 共用上下文，怎样验证取消与父窗？ | [Linux Portal 控制面](linux-portal.md) |
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
