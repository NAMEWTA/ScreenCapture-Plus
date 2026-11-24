# huhucapture 语言文件 / Language files

[项目首页](../README.md) · [English guide](readme.en-us.md)

## 安装与目录优先级

简体中文与英语内置于 `huhucapture.exe`。额外语言使用 `语言名称.locale.json`，例如 `日本語.ja-JP.json`。

1. 把需要的 JSON 文件放入 EXE 同目录的 `Lang` 文件夹。便携 ZIP 已附带仓库语言文件。
2. 仅当 EXE 旁没有 `Lang` 文件夹时，程序使用 `%appdata%\huhucapture\Lang`。
3. 重启程序，在设置中选择语言。

两处目录是优先与回退关系，不会合并读取。语言文件使用 **UTF-16 并带 BOM**，应保留现有 JSON 对象结构和 locale 命名。缺少键时回退到内置英语；无法解析的自定义语言也回退到英语。

## 翻译约定

产品名称统一写为 `huhucapture`，保留命令、路径、快捷键和参数的原始拼写。工具条按 44 逻辑像素点击区、24 逻辑像素图标布局，按钮译名应简短，避免挤压窄面板。

`tool.number` 表示普通序号图形，`tool.annotation` 表示拖出矩形并填写说明的范围批注，二者要能明确区分。新增语言键时同步内置及外部语言；添加功能提示前核对实际交互，尤其不要把拖动矩形描述为单击放置。

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

用户写入的说明不属于翻译资源，支持换行并参与完整导出排版。界面可显示摘要，但导出不能借助译文截断用户内容。交互与布局见 [批注说明](annotations.md)。

提交前检查文件仍为 UTF-16 BOM、JSON 可解析、缺键回退有效、产品名称正确，并在 Windows 检查按钮宽度与面板提示。语言文件只存字符串资源，不加入脚本、链接推广或外部服务依赖。

## Install and choose a language

Simplified Chinese and English are embedded in `huhucapture.exe`. Additional files use `Language.locale.json`, such as `日本語.ja-JP.json`.

Place JSON files in `Lang` beside the EXE, then restart and select the language in settings. If that directory does not exist, the application uses `%appdata%\huhucapture\Lang`. These locations are not merged. The portable ZIP includes the repository's language files.

Keep **UTF-16 with BOM**, the existing JSON structure, and locale filenames. Missing keys fall back to embedded English; an invalid custom language file also falls back to English.

Keep the product name `huhucapture` unchanged. Distinguish the ordinary `tool.number` tool from `tool.annotation`, which creates a rectangular region and an associated note. Translate all `annotation` keys in the table above; `addHint` must describe dragging a rectangle. Keep labels short for narrow panels, preserve literal paths / shortcuts / command-line options, and update embedded and external resources together.

User-written notes are not translation resources. They support multiple lines and must remain complete in exports. Validate encoding, JSON, fallback, and layout before submitting a translation. See the [annotation guide](annotations.md) for behavior details.
