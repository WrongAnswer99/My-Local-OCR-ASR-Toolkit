# Whisper UTF-8 解码失败修复（2026-10-09）

## 故障与定位

消费项目 `bilibiliToText` 转换 BV1gnYs6rESW 时，在全部推理完成后报错：

```text
'utf-8' codec can't decode bytes in position 5313-5314: invalid continuation byte
```

使用缓存的 59 分钟视频复现，直接读取 SDK C ABI 的原始字节。全文 41,408 字节、560 个分段，第 100 个分段（索引 99）最后有两个字节 `E5 91`：它们是不完整的三字节字符。下一个分段以另一个完整字符 `E5 A0 B1` 开头，拼接后的全文仍然不是有效 UTF-8。全文的失败位置与用户报告一致。

Whisper 的模型输出可包含这样的不完整字节；SDK 之前把原始字节直接作为 UTF-8 字符串返回，Python wrapper 的严格解码因此中断。下载和 FFmpeg 音轨提取已成功，这次故障位于 SDK 的识别结果后处理。

## SDK 修复

`src/asr_utf8.hpp` 在原生 SDK 返回结果前统一处理 CPU/GPU 的文字：

- 跨分段解码，保留在相邻分段中拆开的完整字符，把完整码点归入起始字节所在的分段。
- 对真正无法还原的 UTF-8 序列使用 U+FFFD（`�`）标记，保留其余合法文字，不猜测缺失的字符。
- 全文由处理后的分段拼接，保证全文及每段均可严格解码为 UTF-8。
- C ABI 新增 `asr_result_utf8_replacements`，Python 返回 `utf8_replacements`，C++ 返回 `utf8Replacements`；CLI JSON 包含同名计数。

错误修复位于 SDK 项目源码，而不是消费项目的解码容错。更新后的 CPU/GPU 分发均已生成，CUDA SDK 导出到消费项目的 `dist`。应用对非零计数显示提示，不把整次转换判定为失败。

## 验证

原始故障字节回归：只替换 1 个无效序列，结果与 Python 标准 UTF-8 替换解码一致，其余文字完整保留；修复后的文件为 41,409 字节。单元测试还覆盖正常中文、emoji、跨分段字符、空分段、孤立续字节、过长编码、代理区、超过 Unicode 上限及截断字符。

GPU 构建的 CTest（C ABI 生命周期与 UTF-8 回归）2 项通过；CPU 构建的 UTF-8 回归通过。测试源为 `tests/asr_utf8_test.cpp`，实际音频和原始故障字节只保存在本机忽略目录。

随后通过消费项目完整重跑原视频成功：

| 阶段 | 耗时 |
|---|---:|
| 音轨提取 | 1815 ms |
| SDK GPU 推理 | 105202 ms |
| 完整应用流程 | 108.9 s |

输出只有 `output/BV1gnYs6rESW.mp4` 和 `output/BV1gnYs6rESW.txt`。此次全文包含 1 个 `�`，这是缺失字符的标记，不是已还原原词；未进行人工全文准确率检查。

## 消费项目的进度日志

`start.ps1` 对 CPU/GPU 百分比使用 `Write-Log -Progress`。新百分比到达时，仅在上一条日志也是百分比时替换该行；下载、加载、保存、错误和字符修复提示等普通消息保留。

真实 TextBox 验证：连续写入 0..100% 后只有最新的 100% 行；穿插阶段或提示信息后，后续百分比只替换新进度行。重新打开 `start.cmd` 即可加载新界面代码。
