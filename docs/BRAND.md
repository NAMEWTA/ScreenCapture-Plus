# huhucapture 品牌规范

产品名统一写作 **huhucapture**，全小写。官方项目为 [NAMEWTA/huhucapture](https://github.com/NAMEWTA/huhucapture)，发布包、程序信息、文档标题与仓库入口均使用这个名称。

## 视觉方向

标志延续 NAMEWTA 的 huhu 家族气质：橘色虎斑、暖奶油底、深棕描边、举起的白爪和卷尾。huhucapture 加入四个圆角截图取景框，并用正面坐姿、闭眼微笑和简洁的全身轮廓表达轻量、友好的截图工具。主标志和小图标都保留猫的身体、腹部与尾巴。

参考来源是 [NAMEWTA/huhu 的品牌目录](https://github.com/NAMEWTA/huhu/tree/e8008bf4c805f772b5b541831bd594a5e2548302/assets/brand)，参考提交固定为 `e8008bf4c805f772b5b541831bd594a5e2548302`。本项目重新构造图形几何与小写字标，没有引入参考项目的原始 SVG 路径、字体轮廓或栅格图。来源说明、颜色和资源清单保存在 [assets/brand/README.md](../assets/brand/README.md)；新增资源遵循根目录 [LICENSE](../LICENSE)。

## 选择合适的资源

| 使用场景 | 文件 | 注意事项 |
| --- | --- | --- |
| README、浅色页面、浅色关于页 | [`huhucapture-logo-light.svg`](../assets/brand/huhucapture-logo-light.svg) | 深棕字标，透明画布 |
| 深色页面、深色关于页 | [`huhucapture-logo-dark.svg`](../assets/brand/huhucapture-logo-dark.svg) | 奶油字标；猫图标保留奶油底，保证轮廓可见 |
| 大尺寸应用图标 | [`huhucapture-icon.svg`](../assets/brand/huhucapture-icon.svg) | 完整细节、圆角底色 |
| 16–48 px 托盘、窗口图标 | [`huhucapture-icon-small.svg`](../assets/brand/huhucapture-icon-small.svg) | 加粗轮廓、精简花纹的全身版本 |
| 单独的角色与截图框 | [`huhucapture-mark.svg`](../assets/brand/huhucapture-mark.svg) | 透明背景，优先在浅色底使用 |
| 只需要产品名称 | [`huhucapture-wordmark.svg`](../assets/brand/huhucapture-wordmark.svg) | 原生 SVG 笔画，无字体依赖 |
| Windows EXE 内嵌资源 | [`resources/logo.ico`](../resources/logo.ico) | 含七个尺寸；随资源编译进入 EXE |
| 不支持 SVG 的文档位置 | [`docs/logo.png`](logo.png) | 512 × 512 RGBA 图标 |

横向组合标志的画布是 1040 × 256，建议宽度至少 416 px。保持原始宽高比；四角框、角色和字标的位置属于组合的一部分。图标可按需使用已提供的各尺寸 PNG，优先选择显示目标的原生尺寸。16–48 px 使用独立精简图形，避免把完整大图的细节直接缩到托盘大小。

标志外侧建议保留至少图标边长的 1/16 作为空白。完整图形的线条与圆角应一起缩放，避免单独改变描边、裁掉尾巴或填满取景四角之间的空白。大尺寸透明图形适合浅背景；深背景优先使用已有的深色主题组合标志或带奶油底的图标。

## README 深浅主题

GitHub README 可以使用下面的结构。在仓库根目录使用这些相对路径；位于其他目录时调整相对位置。

```html
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/brand/huhucapture-logo-dark.svg">
  <img src="assets/brand/huhucapture-logo-light.svg" width="560" alt="huhucapture">
</picture>
```

SVG 包含标题、描述和可编辑的分组，不依赖外部图片、CSS、字体、脚本或网络。横向字标采用本项目绘制的圆角小写几何笔画，名称在各种设备上保持一致。

## 修改与再生成

唯一人工维护源为：

- `assets/brand/huhucapture-mark.svg`：完整图形。
- `assets/brand/huhucapture-mark-small.svg`：16–48 px 图形。
- `assets/brand/huhucapture-wordmark.svg`：字标。

编辑这些源后，在仓库根目录执行：

```sh
python -m pip install Pillow
python scripts/generate-brand.py
```

生成工具要求 Python 3.10+ 和 Inkscape CLI。可通过 `--inkscape` 指定 Inkscape 路径。生成器会更新深浅主题组合标志、大小应用图标 SVG、九个 PNG 尺寸、文档图标以及 Windows ICO。该工具不属于应用启动或常规构建流程，发布构建使用仓库中已提交的资源。

提交品牌修改前检查：

1. 分别在浅色与深色背景查看横向标志，确认产品名完整、字标对比清楚、两者间距一致。
2. 按实际尺寸查看 16、24、32、48、64 px 图标，确认取景框、全身形状与尾巴仍可辨认；同时查看 256/512 px 版本，确认轮廓、肉垫和边缘没有裁切。
3. 检查所有 ICO 帧均存在，16–48 px 来自精简图形；生成器会验证 ICO 帧像素与 PNG 的一致性。
4. 在同一渲染工具版本下重复生成，确认仓库资源不发生额外变化。不同 Inkscape/Pillow 版本产生的栅格差异需要重新核对。
5. 运行 `git diff --check`，并在 Windows 的构建、托盘与窗口检查中确认图标已随资源更新。桌面图标缓存有时保留旧图，应先确认构建后的 EXE 使用了新资源再处理缓存。

品牌变更不更改截图、矩形批注、导出、录屏、快捷键或语言功能。功能与文档界面示例应继续使用实际程序输出。
