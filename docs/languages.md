# huhucapture 语言资源

[项目首页](../README.md) · [English guide](readme.en-us.md)

当前正式发行仍为 v0.0.5 Windows 产品。以下 Windows 目录规则在 Qt 迁移中继续保留；Linux 规则由共享语言服务实现，其资源测试不等于 Linux 产品已经发布。

## 安装与目录优先级

| 平台策略 | 行为 |
| --- | --- |
| Windows | 简体中文、英语内置。额外语言先检查 EXE 旁的 `Lang` 目录；存在时只使用该目录，即使它为空。仅当目录不存在时回退到 `%appdata%\huhucapture\Lang`。两处不能合并。便携 ZIP 继续携带原六份语言文件。 |
| Linux Qt | 六份原语言嵌入程序，安装目录和 AppImage 挂载位置无需写入。用户数据目录的 `Lang` 可提供其他语言，或覆盖所选的非内置中英语言；没有匹配用户文件时使用嵌入的同语言资源。 |

外部文件沿用 `显示名称.语言代码.json` 三段命名，例如 `日本語.ja-JP.json`。Qt 服务按完整语言代码匹配，避免子串误选；目录按文件名排序，同代码只取第一项，语言列表不产生重复选项。添加文件后重新打开程序并在设置中选择。

仓库现有文件保持 **UTF-16 BOM**、原名和原 JSON 对象结构。Qt 读取与配置迁移共用 `JsonText`，另外兼容 UTF-8、UTF-8 BOM 和 UTF-16BE BOM；无效编码、非对象 JSON 和大于 8 MiB 的文件会产生明确错误。运行资源 `Lang` 不适用文档目录的小写命名要求。

## 翻译约定

产品名称统一写为 `huhucapture`，保留命令、路径、快捷键和参数的原始拼写。工具条按 44 逻辑像素点击区、24 逻辑像素图标布局，按钮译名应简短，避免挤压窄面板。

`tool.number` 表示普通序号图形，`tool.annotation` 表示拖出矩形并填写说明的范围批注，二者要能明确区分。新增语言键时同步六份语言；添加提示前核对实际交互，尤其不要把拖动矩形描述为单击放置。

| `annotation` 键 | 含义 |
| --- | --- |
| `title` | 右侧说明栏标题 |
| `placeholder` | 多行输入提示 |
| `empty` | 空备注的导出占位 |
| `addHint` | 拖出矩形范围的引导 |
| `edit` | 当前编号的编辑标题 |
| `remove` | 删除当前批注 |
| `collapse` | 收起说明栏 |
| `saveHint` | 图片和说明一起导出的提示 |
| `exportFailed` | 完整结果无法导出的提示 |

用户写入的说明不属于翻译资源，支持换行并参与完整导出排版。界面可显示摘要，但导出不能借助译文截断用户内容。交互与布局见 [批注说明](annotations.md)。语言文件只存字符串，不加入脚本、链接推广或外部服务依赖。

## 共享服务与回退

`src/services/localization/LanguageCatalog` 是 Qt 窗口共用的语言服务。qrc 直接嵌入原文件字节，不另抄一套翻译到 C++。缺键、空字符串或错误类型逐键回退到内置英语；英语仍无该键时显示键名。

选中的外部文件损坏或不存在时，本次显示回退到英语并返回详细错误。`requestedCode` 保留用户选择，`effectiveCode` 反映实际显示语言；服务不会擅自改写设置，窗口应向用户说明回退原因。如果嵌入英语本身无法读取，则载入失败并保留此前可用目录，不能把空对象当作成功。

一个应用只装配一个 `LanguageCatalog`，窗口通过 `text(QStringView)` 和 `changed()` 使用同一状态。载入新语言后清空解析缓存，窗口重新布局并关闭或重新定位弹层。调用在应用所属线程进行；导出和后台任务接收文本副本，不跨线程读取可变缓存。语言服务不拥有设置窗口，也不承担配置保存。

## Qt 标准对话框

项目的六份 JSON 不包含 Qt 自己的文件对话框和标准按钮。宿主另持有一个很薄的
`QtTranslations`，按同一 `LanguageCatalog::effectiveCode()` 调用官方
`QTranslator::load(QLocale, "qtbase", "_", directory)`。目录来自部署的
`qt.conf` 所解析的 `QLibraryInfo::TranslationsPath`；安装树携带与锁定 Qt
SDK 一致的原始编译 `.qm`，不把标准对话框字符串再抄进 C++。

切换语言会移除原翻译器并安装新目录，现有 Qt 标准按钮收到真实
`LanguageChange` 后重译；选择英文则恢复 Qt 的原文，不要求冗余英文 QM。
复用依据为固定 Qt 的
[QTranslator](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/corelib/kernel/qtranslator.cpp)
和 [QDialogButtonBox](https://github.com/qt/qtbase/blob/5af1a37e31a869cd6eb8a73d82e2c71a450e6634/src/widgets/widgets/qdialogbuttonbox.cpp)
实现，属于使用其公开接口，并未复制 Qt 翻译代码。

固定 Qt 6.12.1 的实际官方目录提供简体、繁体、日语和俄语，**没有印尼语
`qtbase_id.qm`**。项目自己的印尼语界面仍使用原 `Lang`，受影响的标准对话框
使用默认语言。其他实际目录缺失同样返回未载入，并在已有设置语言行显示说明；
不会在每次启动、截图或一次性模式强制弹确认框，也不假装沿用了其他语言。
操作系统完全拥有的原生文件选择器仍由系统语言与桌面后端决定；Qt 字符串
切换不能证明这些外部窗口已经全部改变语言。

## 验证边界

`qt_language_catalog` 实际读取六份原资源，验证中日文字、两种 UTF-16 BOM、UTF-8 BOM、损坏编码、缺键/空值/错误类型、缓存刷新、Windows 空便携目录不合并、Linux 用户覆盖和完整代码匹配。

`qt_standard_translation_tests.cpp` 使用真实部署或 SDK 中的 QM，核对现有
`QDialogButtonBox` 的“打开／取消”在四种非英文目录之间确实改变、移除后恢复
英文，以及空目录确实返回失败。本地 Linux 已对固定 SDK 的实际目录运行通过。
它不合成 QM，不把印尼语目录缺失记为官方覆盖，也不替代原生 OS 对话框验收。

提交翻译前保持原文件编码与结构，并检查 JSON、品牌名称、缺键回退和实际按钮宽度。中文输入法、长译文布局、语言弹层导航及跨显示器 DPI 仍须由正式 Qt 窗口交互和真实桌面验收覆盖，不能从资源读取通过推断其结果。

## Install and choose a language

Simplified Chinese and English are embedded in `huhucapture.exe`. Additional files use `Language.locale.json`, such as `日本語.ja-JP.json`.

Place JSON files in `Lang` beside the EXE, then restart and select the language in settings. If that directory does not exist, the application uses `%appdata%\huhucapture\Lang`. These locations are not merged. The portable ZIP includes the repository's language files.

Keep **UTF-16 with BOM**, the existing JSON structure, and locale filenames. Missing keys fall back to embedded English; an invalid custom language file also falls back to English.

Keep the product name `huhucapture` unchanged. Distinguish the ordinary `tool.number` tool from `tool.annotation`, which creates a rectangular region and an associated note. Translate all `annotation` keys in the table above; `addHint` must describe dragging a rectangle. Keep labels short for narrow panels, preserve literal paths / shortcuts / command-line options, and update embedded and external resources together.

User-written notes are not translation resources. They support multiple lines and must remain complete in exports. Validate encoding, JSON, fallback, and layout before submitting a translation. See the [annotation guide](annotations.md) for behavior details.
