# huhucapture 品牌资产

这里是 huhucapture 图形、字标和应用图标的唯一维护入口。品牌名称统一使用小写 `huhucapture`；维护者与仓库为 [NAMEWTA/huhucapture](https://github.com/NAMEWTA/huhucapture)。

## 设计与来源

视觉参考来自 NAMEWTA 的 [huhu 品牌目录](https://github.com/NAMEWTA/huhu/tree/e8008bf4c805f772b5b541831bd594a5e2548302/assets/brand)，固定参考提交为 `e8008bf4c805f772b5b541831bd594a5e2548302`。参考了橘色虎斑猫、暖奶油底色、深棕轮廓、举爪与卷尾这些家族特征。

huhucapture 的全身角色、截图四角框和字标均在本仓库以独立几何路径绘制。角色采用正面坐姿、微笑眼与简化轮廓，截图四角框表达产品用途；小尺寸版本保留全身形状，减少花纹并加粗轮廓。字标是自行构造的小写圆角笔画，不使用字体文件。这里没有复制 huhu 的 SVG 路径、Geist 字形或位图；参考仓库的资源不作为本项目的分发资源。新增绘图与生成脚本遵循本仓库的 [LICENSE](../../LICENSE)。

## 文件与维护规则

| 文件 | 用途 | 维护方式 |
| --- | --- | --- |
| `huhucapture-mark.svg` | 完整全身角色与截图框，透明背景 | 手工维护的主图形源 |
| `huhucapture-mark-small.svg` | 16–48 px 的简化全身图形 | 手工维护的小尺寸源 |
| `huhucapture-wordmark.svg` | 小写产品名称的几何字标 | 手工维护的字标源 |
| `huhucapture-icon.svg` | 奶油圆角底的完整应用图标 | 脚本生成 |
| `huhucapture-icon-small.svg` | 奶油圆角底的小尺寸应用图标 | 脚本生成 |
| `huhucapture-logo-light.svg` | 浅色背景上的横向组合标志 | 脚本生成，深棕字标 |
| `huhucapture-logo-dark.svg` | 深色背景上的横向组合标志 | 脚本生成，奶油字标 |
| `huhucapture-icon-{size}.png` | 16、24、32、48、64、128、256、512、1024 px 图标 | 脚本从对应 SVG 独立渲染 |
| [`../../resources/logo.ico`](../../resources/logo.ico) | Windows EXE、窗口与托盘资源 | 脚本合并七个原生尺寸帧 |
| [`../../docs/logo.png`](../../docs/logo.png) | 文档中的 512 px 位图兼容入口 | 脚本生成 |

修改前三个源 SVG 后执行：

```sh
python -m pip install Pillow
python scripts/generate-brand.py
```

需要 Python 3.10+、Pillow 和可执行的 Inkscape。若 Inkscape 未加入 `PATH`，使用 `--inkscape /path/to/inkscape`；Windows 可传入 `inkscape.exe` 的完整路径。品牌生成工具只在修改资源时使用，正常构建和发布直接使用已提交的 ICO，不依赖 Python、Pillow、Inkscape 或字体安装。

生成器逐一从 SVG 渲染每个尺寸，16–48 px 使用简化源，64 px 起使用完整源。ICO 含 16、24、32、48、64、128、256 px 的 RGBA PNG 帧；生成后会重新打开文件，验证帧尺寸和像素与对应 PNG 一致。PNG 不携带渲染器时间戳等元数据；同一工具版本的重复生成应无内容差异。跨版本渲染器的抗锯齿可能不同，更新工具后应检查差异并重新目视核对。

## 调色板

| 角色 | 色值 | 用途 |
| --- | --- | --- |
| 深棕 | `#67381F` | 图形轮廓、截图框、浅色主题字标 |
| 橘色 | `#FFA94D` | 猫身主色 |
| 虎斑色 | `#BF5E20` | 额头与身体花纹 |
| 奶油底 | `#FFF2DE` | 图标底色、深色主题字标 |
| 暖白 | `#FFF5EA` | 腹部和爪子 |
| 肉垫粉 | `#FF9C91` | 举爪的心形肉垫 |
| 耳内粉 | `#FFB4A8` | 耳朵内侧 |
| 浅棕 | `#C79D80` | 大尺寸爪部细节 |

这些色彩定义品牌资源，不改变截图编辑器中的批注序号、边框或主题颜色。

详细的品牌用法、README 深浅主题示例和验收要求见 [docs/BRAND.md](../../docs/BRAND.md)。
