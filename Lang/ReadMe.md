# ScreenCapture-Plus v0.0.1 语言文件 / Language files

[项目首页](../README.md) · [English project guide](../docs/ReadMe.en-US.md)

## 安装与选择语言

简体中文与英语内置于 `ScreenCapture.exe`。额外语言文件使用 `语言名称.locale.json` 的命名方式，例如 `日本語.ja-JP.json`。

1. 将需要的 JSON 文件放在 `ScreenCapture.exe` 同目录的 `Lang` 文件夹中。便携 ZIP 已附带仓库内的语言文件。
2. 如果 EXE 旁没有 `Lang` 文件夹，也可以使用 `%appdata%\ScreenCapture\Lang`。两者是优先与回退关系，不会合并读取。
3. 重启程序，在语言设置中选择对应语言。

语言更新时应保留现有文件的编码、JSON 对象结构与 locale 命名。缺少的新键会回退到内置英语；自定义语言出现解析错误时，也会使用英语。

## v0.0.1 翻译内容

本版本的工具条统一为 44 逻辑像素点击区域与 24 逻辑像素图标。原有 `tool.number` 是独立序号图形，新增的 `tool.annotation` 是带右侧说明栏的批注工具，两者应使用容易区分的译名。

`annotation` 对象包含以下文案：

| 键 | 用途 |
| --- | --- |
| `title` | 右侧说明栏标题 |
| `placeholder` | 多行输入框提示 |
| `empty` | 尚未填写的批注说明 |
| `addHint` | 点击图片添加批注的引导 |
| `edit` | 当前编号的编辑标题 |
| `remove` | 删除当前批注 |
| `collapse` | 收起说明栏 |
| `saveHint` | 图片与说明一起导出的提示 |
| `exportFailed` | 完整结果无法导出时保留编辑并说明原因 |

保持按钮文案简短，避免在窄面板内挤压。说明正文支持换行，导出时会自动排版并按空间调整字号；这部分内容由用户输入，不属于语言文件。交互细节见 [批注说明](../docs/ANNOTATIONS.md)。

## Install and select a language

Simplified Chinese and English are embedded in `ScreenCapture.exe`. Additional files use the naming convention `Language.locale.json`, for example `日本語.ja-JP.json`.

1. Put the JSON files in a `Lang` directory beside the EXE. The portable release ZIP includes the repository's language files.
2. If that directory does not exist, `%appdata%\ScreenCapture\Lang` is used instead. The two locations are not merged.
3. Restart the application and choose the language in settings.

Preserve the existing encoding, JSON structure, and locale filename when updating translations. Missing keys fall back to built-in English; an invalid language file also falls back to English.

In v0.0.1, distinguish the existing `tool.number` drawing tool from the new `tool.annotation` tool with a right-side notes panel. Translate every key in the `annotation` object listed above. Keep button labels short for narrow panels. User-written notes support multiple lines; exported text is wrapped and sized to fit the complete result. See the [annotation guide](../docs/ANNOTATIONS.md).
