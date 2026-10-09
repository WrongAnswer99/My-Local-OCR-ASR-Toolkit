# Whisper 重复字幕排查与修复

2026-10-09。涉及 bilibiliToText 独立项目与 My-Local-OCR-ASR-Toolkit 的 CPU/CUDA ASR SDK。

## 故障与定位

下载目录两个视频的音频都可以正常解码，提取为 16 kHz 单声道 PCM 后，短片段能识别不同内容。旧引擎对整段音频仍可复现大面积循环，因此不是 FFmpeg 把原音频转换成重复音频。

- BV1ihHQ6bE7q_20261009095206948.mp4：约 8 分 40 秒，旧完整音频结果包含 420 段“好”。
- BV1gKH96sE3w_20261009094806605.mp4：约 16 分 11 秒，旧完整结果有 215 段“然后你关上这棵”。它实际是英语，自动检测为 en，置信度约 99.8%；强制 zh 导致错误的中文识别。

原字幕和文字已备份到 bilibiliToText/.work/repetition/*.original.srt、*.original.txt。原视频未修改。

## 最终修复

两个项目的 C++ 推理层同步：

1. n_max_text_ctx=0，禁止将错误识别内容作为后续窗口的提示。no_context=true 仅清除上一次 API 调用的历史，本身不足以禁用文件内部的滚动提示。
2. temperature_inc=0.2，恢复按熵和平均对数概率触发的温度回退。旧版强制设为 0，相当于关闭重试。
3. 首次解码采用 5 候选 beam search，改善困难窗口的选择。
4. 增加官方 ggml Silero VAD v6.2.0，在识别前筛选语音；阈值 0.25、最短语音 100 ms、静音分割 300 ms、前后留白 300 ms。较宽松的阈值保留混有音乐的较轻对白。Whisper 将字幕时间映射回原音频，未把字幕按删去静音后的时间直接输出。

仅改提示和回退能阻止贯穿全片的大循环，但中文视频仍有局部重复；加入 VAD 后，复测的两个完整结果均没有连续相同字幕段。没有用删除重复文字的方式掩盖问题。

VAD 模型 ggml-silero-v6.2.0.bin 为 885,098 字节（以本地实际文件为准），固定 revision=9ffd54a，SHA-256：
2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987

下载脚本 tools/setup_whisper_vad.ps1 验证哈希，支持官方站和同提交镜像。Silero MIT 许可证随模型分发。转写完全离线。旧 SDK 消费者不携带 VAD 文件仍可使用提示/回退修复，但新版分发必须保留模型以获得完整修复。

## 语言选择

应用和油猴统一为：英语、简体中文、日语、俄语、繁体中文、自动检测。默认仍为简体中文。

- 英语 → en，简体/繁体中文 → zh，日语 → ja，俄语 → ru，自动检测 → auto。
- 这是音频中实际说话的语言，不是翻译目标语言；没有增加跨语言翻译。
- OpenCC 只处理识别语言为 zh 的结果，避免改写日语汉字。
- 油猴 v0.3.1 将语言传给本地协议，本地按传入参数选择同一个下拉框。
- 已有字幕需要重新识别时，勾选“重新识别”；CLI 使用 --overwrite。

## 完整视频复测

使用项目内 small Q5_1、NVIDIA CUDA、8 线程，运行实际应用流程并覆盖问题 TXT/SRT：

| 视频 | 使用语言 | 新字幕段数 | 最长连续相同字幕段数 | 整体耗时 | 推理耗时 |
| --- | --- | ---: | ---: | ---: | ---: |
| BV1ihHQ6bE7q | zh | 157 | 1 | 16.0 秒 | 15,095 ms |
| BV1gKH96sE3w | en | 146 | 1 | 30.2 秒 | 28,879 ms |

两个 MP4 的 SHA-256 前后一致；同名 TXT/SRT 已在原下载目录重新生成。第二个文件保存原语音的英文字幕。

以上说明循环故障消失，不代表人工逐句校对后的准确率。small 模型仍有错字、人名错误和局部漏词；VAD 也可能漏掉很轻或混在音乐里的声音。保留原视频，必要时可改用更大模型进行识别。

## 验证与分发

- 应用：13 个 Python 测试；油猴 16 个流程场景及 6 个语言选项的传参检查。
- CPU SDK：C ABI 生命周期、重复调用、CLI 错误和 UTF-8 检查；真实问题区域、纯静音、静音后复用、带 10 秒前置静音的字幕原时间轴。
- CUDA SDK：同样的基础检查；对两个完整问题音频验证重复、静音、复用和原始时间戳。
- 中文 SDK 目录中的静态 CPU CLI/VAD 加载检查通过；MinGW 静态加载器使用系统代码页/短路径，官方 CUDA DLL 使用 UTF-8，音频输入保持 UTF-8 接口。
- 实际 Windows 协议传入英语选项，窗口选中“英语”，路径只读，CUDA 生成英文 TXT/SRT，通过。
- SDK 版本 0.3，私有应用引擎 1.1。C ABI 结构体和已有接口不变；CPU/CUDA 分发已重新导出。

应用报告在 .work/repetition/final-video-results.json；SDK 回归报告在 dist-asr/repetition-cpu.json、dist-asr-gpu/repetition-zh.json 和 repetition-en.json。

开发者可用 tests/asr_repetition_integration.py 指定本地回归 WAV；测试不会下载视频或引入 FFmpeg 到 SDK。

## 依据

- [OpenAI Whisper 的失败重试与上下文说明](https://github.com/openai/whisper/blob/main/whisper/transcribe.py)
- [whisper.cpp 1.9.5 的解码、滚动提示及 VAD 时间映射实现](https://github.com/ggml-org/whisper.cpp/blob/v1.9.5/src/whisper.cpp)
- [whisper.cpp 官方 VAD 用法](https://github.com/ggml-org/whisper.cpp#voice-activity-detection-vad)
- [官方 ggml VAD 模型](https://huggingface.co/ggml-org/whisper-vad)
- [Silero MIT 许可证](https://github.com/snakers4/silero-vad/blob/master/LICENSE)
