# 共享二维码识别

二维码迁移直接复用现有 `quirc` 检测、纠错及解码实现。旧 Windows 的
`ImageRecognition` 已调用同一个 [识别核心](../src/core/QrRecognition.h) 和
[文字转换策略](../src/platform/QrText.h)；[Qt 服务](../src/services/recognition/QrService.h)
只适配 `QImage` 与 `QString`。这里没有新增 OCR、二维码编码器或另一套识别算法。

## 结果与生命周期

识别入口是同步工作线程接口。输入是调用期间有效的只读、从上到下的 BGRA
像素，允许行尾 padding。Qt 宿主在派发前拥有图像，并在回到界面时检查自己的请求令牌
和窗口生命周期。服务不访问剪贴板、不建立对话框，也不关闭截图窗口。

核心保留原来的 `(R*77 + G*150 + B*29) >> 8` 灰度公式，忽略 alpha，保留
`quirc` 的发现顺序。只在 `QUIRC_ERROR_DATA_ECC` 时翻转码格并重试一次，其他
错误仍跳过相应候选。解码成功的原始字节、版本、纠错等级、掩码、四角、类型、ECI
及镜像重试结果共同保留。多个可读且非空的结果依原顺序以换行连接。

识别失败、预算不足和取消都不覆盖调用方已有的结果；合法空白图像则成功产生空结果。
文字转换状态与识别状态分开：找到一个编码未知的真实二维码，不能被界面显示成
“没有二维码”，也不能把空字符串显示成已经成功复制的内容。

上游的 `quirc_end()` 是一次完整识别调用，取消在灰度逐行处理、该调用前后和逐码
解码之间检查，不声称能打断正在执行的上游识别过程。应用退出应停止提交并等待
既有工作结束，不能释放仍被任务借用的图像。

## 字符编码合同

共同策略先使用系统库严格按 UTF-8 转换，失败后才进入原有兼容路径。Windows 使用
`MultiByteToWideChar`；Linux 使用 libc `iconv`，未实现自有字符解析器。

| 输入情况 | Windows | Linux |
| --- | --- | --- |
| 合法 UTF-8，包括中文、非 BMP 字符和嵌入 NUL | 严格 UTF-8，保留完整长度 | 同一 UTF-8 优先顺序，保留完整长度 |
| 非 UTF-8 的 Kanji 模式 | 保持原 CP932、flags=0 | CP932 转换，保持 Windows 波浪线等实际字符映射 |
| 非 UTF-8 的其他模式 | 保持原 CP_ACP、flags=0；记录实际 `GetACP()` | 不猜测另一台 Windows 的 ACP |
| 上一行具有受支持的明确 ECI | Windows 仍保留旧 ACP 行为，没有悄悄改变兼容优先级 | 使用明确的 ECI 转换；当前包括 quirc 声明的 ISO-8859 系列、CP437、CP874、CP932、UTF-8 |
| Linux 上无法确定或不支持的编码 | — | 原始 payload 保留，返回 `UnknownEncoding` / `UnsupportedEncoding`，没有乱码成功结果 |
| 实际字节不符合选中的严格转换器 | 保留原 Windows API 的既有行为和实际错误 | `InvalidEncoding`，不使用替换字符伪造完整结果 |

`quirc_data` 只提供最高数据模式及最后一个 ECI 值，没有各分段的编码边界；本次没有
替换解码器或补造分段解析器。混合编码二维码的这项上游限制仍存在，原始字节和 ECI
用于明确呈现不能确认的情况。Windows 的旧 ACP 结果具有机器区域设置依赖，Linux
无法从没有声明的字节恢复这一信息。

## 内存与构建约束

`QUIRC_MAX_REGIONS=65534` 是旧生产工程的实际配置，必须同时用于全部四个 quirc
C 文件、`QrRecognition.cpp` 和访问内部结构的核心测试。它使区域图使用 16 位
像素；误用上游默认 254 区域不是等价迁移。

`qrWorkingBytes()` 依据当前编译的实际结构计算新识别器的堆请求：
`sizeof(quirc) + width*height*(1+sizeof(quirc_pixel_t)) +
max(1,height*2/3)*sizeof(quirc_flood_fill_vars)`。
默认工作预算为 256 MiB，不把调用方已经拥有的输入图像算作新分配。识别输出受到
quirc 的最多 64 个候选、每码 8896 字节上限约束。Qt 对原生 ARGB32/RGB32 直接借用，
其他格式只转换一次，并把转换所得 BGRA 图像计入同一工作预算后再分配。

宽高、像素乘积、stride、最后一行有效字节和预算都在读取输入前检查。像素数不得
超过当前解码器的有符号 int 索引范围。这不是对上游全部几何运算的安全证明，见下文
数值范围修复与验证范围。

构建需要 C11（quirc 的四个 `.c`）、C++20，以及平台对应的一个 `QrText.cpp`。
Linux 的 `iconv` 来自目标 libc；Windows 转换调用系统 API。Qt 服务只链接 Qt Core / Gui，
不依赖 Qt Widgets、Qt Core5Compat 或 Ling。保留的旧 Windows 产品只登记共享核心、
共同文字策略和 Windows 文字转换器，不因二维码迁移而链接 Qt。

## 来源、补丁与 fixture

[quirc 来源清单](../third_party/quirc/upstream.json) 保留继承快照逐文件 git blob、
当前 SHA-256、ISC 许可，以及实际阅读的上游固定提交。继承代码没有可信的上游版本
标签记录，不能把 `quirc_version()` 的固定字符串 `1.0` 当成精确版本。

当前仅从已检查的上游代码回移两个窄补丁：

- 第一次 resize 的旧图像为空且复制长度为零时不调用 `memcpy`。原始调用已在本地
  UBSan 失败，回移后同一调用通过。
- Otsu 的两处加权乘法在相乘前转换为 `quirc_float_t`，避免合法 8K 亮背景中
  `255 * 白色像素数` 先发生 unsigned int 回绕。原函数实测阈值错误；回移后阈值
  和真实完整二维码识别均恢复。

此外，本项目为原有直线交点运算保留同一公式，在相减和相乘前提升到 double，并在转换回 int 前检查有限值及范围。8K 屏幕坐标的整数中间乘积可被 double 精确表示；最终结果保留原有向零截断语义。它解决已观察到的大尺寸、远离原点二维码的 int 中间乘积溢出，没有降低分辨率或取消测试。该本地补丁单独标注来源，不冒称上游已合入。

三份 patch 按顺序从清单中的原始文件直接重放，重放后的文件逐字节匹配工作源码。
这不是把整个 quirc 更新到所参考的上游提交，也没有修改二维码协议或纠错算法。

[fixture 生成器](../scripts/generate-qr-fixtures.py) 调用未修改的 Segno 1.6.6
公开 API，并在导入前核对所有依赖模块的固定 git blob。源码与 BSD-3-Clause
归属见 [Segno 来源](../third_party/segno/upstream.json)。Segno 仅用于维护时生成数据，
不成为产品运行时依赖；CI 使用已经入库的真实码格。生成方法：

```sh
python3 scripts/generate-qr-fixtures.py --segno-source /path/to/pinned-segno-checkout
```

生成的 14 组固定数据包括 ASCII、UTF-8 中文/非 BMP 字符、Kanji/CP932、明确 ECI、
未知原始字节、嵌入 NUL、空内容、数字、字母数字和版本 40。测试的旋转、镜像、拼接和
逐格栅格化只改变真实码格的显示方式，不伪造检测结果。

## 实际验证与尚未完成的门槛

本地 gcc 14 的第三方 C11 `-O2`、g++ 14 的共享 C++20
`-O2 -Wall -Wextra -Wpedantic -Werror` 编译与纯 C++ 测试已通过。
第三方 C 保留既有符号比较和未使用参数警告，不把它描述成通过同一组 `-Werror`。实际覆盖全部固定码、四种旋转及镜像、单数据格纠错、多码顺序、
灰度通道和透明度、stride、工作预算、取消、失败保留已有结果、8K 亮背景识别，
以及 8192×4096 图像中位于 (5000, 1000)、每个码格 70 像素的完整二维码。
同组测试也通过 ASan/UBSan（包括浮点转整数溢出检查）；当前执行环境的 ptrace 限制阻止 LeakSanitizer 工作，
因此没有把泄漏检查写成通过。

Qt 工作线程/图像格式/PNG 往返测试已经写入，Windows 实际 ACP 和旧产品接线测试
仍待对应真实 SDK 构建。本地没有 Qt / Windows SDK，不能据纯 C++ 结果宣称这些项通过。
截图入口的 Qt 对话框、结果复制以及最终使用旅程由应用装配接入后另行验收。

已观察到的直线交点整数溢出已修复并通过上述实际样本。它不等于对上游所有几何运算的形式化证明，也不替代 Qt 截图入口、双平台和真实 DPI 验收。
