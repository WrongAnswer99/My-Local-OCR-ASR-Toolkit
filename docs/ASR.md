# Whisper 整段音频转文字

本项目新增本地离线 ASR：使用 OpenAI Whisper 模型，通过 [whisper.cpp](https://github.com/ggml-org/whisper.cpp) 的 C API 接入 C++。第一版功能为打开一个音频文件并转写完整音频，提供 CLI、C ABI 和单头文件 C++ SDK。Whisper 先使用 **CPU**，与 OCR 的 CPU/CUDA 选择独立；无需 API Key，也不会将音频上传到网络。

默认模型为多语言 Whisper **small Q5_1**，文件名 `ggml-small-q5_1.bin`，支持中文及英文，可自动检测语言。模型在开发准备阶段下载，转写时完全离线。Whisper/ggml 静态链接进 `asr.dll`，与现有 OCR 的 `ocr.dll` 分开。

## 使用 CLI

在项目根目录运行：

```powershell
.\dist\asr_cli.exe "D:\音频\会议录音.mp3" --language=zh --threads=8 `
  --output="D:\音频\会议文字.txt" --json="D:\音频\会议结果.json"

# 自动检测语言；直接把完整 UTF-8 文字输出到 stdout。
.\dist\asr_cli.exe .\dist\asr-samples\jfk.wav
```

| 参数 | 说明 |
|---|---|
| 第一个位置参数 | 一个本地音频文件，支持中文路径和空格 |
| `--model=<路径>` | 自定义 Whisper ggml 模型；默认模型从 `asr.dll` 旁的 `models/whisper` 查找 |
| `--language=auto\|zh\|en\|…` | 默认 auto；已知语言时可以指定 |
| `--threads=0..256` | 默认 0，自动选择最多 8 个工作线程 |
| `--output=<路径>` | 可选，保存完整 UTF-8 文字，无 BOM |
| `--json=<路径>` | 可选，保存文字、语言、分段时间戳和耗时 |
| `--help` | 使用说明 |

stdout 只输出完整文字，模型诊断与耗时输出到 stderr。失败返回退出码 1。输出路径不能与输入音频相同。程序不把音频截断为 30 秒：由 Whisper 处理整个输入中的后续窗口。时间戳以毫秒表示。

## 音频格式与运行环境

音频通过 Windows Media Foundation 解码，混合为单声道，并使用低通重采样转换为 Whisper 所需的 16 kHz float PCM。发布包不需要安装 Python、PyTorch、外部 FFmpeg 或 Whisper Python 包。

目标环境为 Windows 10/11 x64，具备 Media Foundation 音频解码组件及支持 AVX2/FMA/F16C 的 CPU。WAV、MP3、M4A/AAC 等格式取决于系统已安装的解码器；不支持的格式、缺少音轨及损坏文件会返回错误。开发阶段生成测试格式使用的 FFmpeg 不属于转写的运行依赖。

当前实现将整段音频解码到内存后转写，长音频的内存随时长增加。首次初始化包含模型读取和引擎创建；重复转写应复用同一个引擎。静音、噪声、音乐或口音可能影响模型输出，本接口不提供说话人分离、实时录音或翻译。

## C++ SDK

使用 `sdk/asr.hpp`（发布包为 `dist/asr.hpp`）。只需要包含头文件，通过绝对路径加载 `asr.dll`，无需链接导入库：

```cpp
#include "asr.hpp"

asr::Engine engine;
std::string error;
// 不传路径时，从宿主 EXE 旁加载 asr.dll。
if (!engine.loadDll("F:/projects/OCRtest/dist/asr.dll", &error)) {
    // 处理 error
}
asr::Options options;
options.language = "zh";
options.threads = 8;
if (!engine.init(options, &error)) {
    // 处理 error
}
asr::Transcript result;
if (engine.transcribeFile("D:/音频/会议.mp3", result, &error)) {
    // result.text：整段文字
    // result.language：检测/指定的语言
    // result.segments：每段 startMs/endMs/text
    // result.audioMs/decodeMs/transcribeMs：音频时长、解码与转写耗时
}
```

路径和文字均使用 UTF-8。同一引擎可反复转写多个文件；C ABI 在单引擎上串行化转写，调用期间不能销毁句柄。C++ wrapper 本身不支持并发 init/close。示例位于 `sdk/examples/asr_example.cpp`，对应发布包工具 `asr_example.exe`。

## C ABI

头文件为 `src/asr_api.hpp`，发布包为 `dist/asr_api.hpp`：

```c
AsrOptions options;
asr_default_options(&options);
options.language = "zh";
options.threads = 8;
char error[2048] = {0};
AsrHandle engine = asr_create(&options, error, sizeof(error));
if (engine) {
    AsrResult result = asr_transcribe_file(engine, "audio.wav", error, sizeof(error));
    if (result) {
        const char* text = asr_result_text(result);
        /* 在释放 result 前使用或复制 text。 */
        asr_free_result(result);
    }
    asr_destroy(engine);
}
```

结果使用独立句柄，没有固定长度的文字缓冲区，避免长音频转写被截断。结果及字符串由 DLL 分配，必须调用 `asr_free_result` 释放；字符串借用指针在结果释放前有效。结果可以在引擎销毁后继续读取，但须保持 DLL 已加载。`asr_create` 和 `asr_transcribe_file` 失败返回 NULL，并通过 error 提供原因。

`AsrOptions.struct_size` 必须正确设置，建议始终调用 `asr_default_options`。第一版 `device=0` 表示 CPU，其他设备值会明确失败。`asr_result_segment_*` 提供分段文字及毫秒时间戳；`asr_result_audio_ms`、`asr_result_decode_ms`、`asr_result_transcribe_ms` 提供时长指标。

## 准备、构建与打包

依赖使用 whisper.cpp **1.9.5**；模型校验 SHA-256 固定为：

```text
ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb
```

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_whisper.ps1
# 官方模型服务器无法访问时，可选择镜像；仍校验相同 SHA-256。
powershell -ExecutionPolicy Bypass -File tools/setup_whisper.ps1 -ModelHost https://hf-mirror.com

cmake -S . -B build-cpu -G "MinGW Makefiles" -DOCR_RUNTIME=cpu -DENABLE_WHISPER=ON
cmake --build build-cpu -j8
powershell -ExecutionPolicy Bypass -File tools/make_dist.ps1 -Runtime cpu -BuildDir build-cpu
```

`ENABLE_WHISPER` 默认 OFF，原有 OCR 构建不强制依赖 Whisper。开启后新增 `asr_sdk`、`asr_cli`、`asr_example` 目标。GPU OCR 构建也可以开启该选项，Whisper 仍使用 CPU。发布脚本复制 ASR DLL/工具、SDK 头文件、模型、许可证、文档及测试脚本；本机测试样本存在时一并复制到 `asr-samples`。

## 发布包测试

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\dist\test_asr.ps1 -Repeat 1
# 中断后可复用成功结果，补测缺失/失败的项目。
# powershell -File .\dist\test_asr.ps1 -ResumeReportDir <dist 内的结果目录绝对路径>
```

测试直接通过 C ABI 调用 `dist/asr.dll`，再验证通过 C++ wrapper 实现的 CLI。测试只使用发布包文件和 Windows 自带组件，清除第三方 PATH。报告位于 `dist/asr-test-results/YYYYMMDD_HHMMSS/`，包括完整文字、分段、耗时、模型 SHA-256 和失败信息。

覆盖真实英文语音 JFK、中文合成语音 WAV/MP3/M4A、超过 30 秒的音频、同一引擎重复调用、中文路径、损坏/缺失文件及不支持设备的明确失败。中文样本来自 Windows TTS，属于功能测试，不能当作真实中文语音准确率基准。测试样本的来源与生成方式见 `tests/audio/README.md`。

## 本次实测（2026-10-08）

以下使用发布包内的 small Q5_1 模型、CPU 8 线程、语言自动检测，每项首次转写后再重复一次。耗时包括音频解码/重采样和完整转写，不包含模型初始化或托管层结果读取、磁盘写入。

| 测试音频 | 时长 | 首次 SDK 调用 | 第二次 SDK 调用 | 结果 |
|---|---:|---:|---:|---|
| 真实英文 JFK WAV | 11.00 秒 | 16667.57 ms | 16855.40 ms | PASS |
| 中文合成语音 WAV | 14.06 秒 | 17741.48 ms | 17690.59 ms | PASS |
| 中文合成语音 MP3 | 14.12 秒 | 17811.82 ms | 17843.64 ms | PASS |
| 中文合成语音 M4A | 14.10 秒 | 18012.62 ms | 17946.16 ms | PASS |
| 三遍 JFK 长音频 | 33.00 秒 | 26986.93 ms | 27108.70 ms | PASS |

长音频最后分段结束时间为 33000 ms，覆盖第一个 30 秒窗口之后的语音。中文三个格式的完整文字与合成文本一致（忽略标点）；SDK 没有固定长度输出截断。当前 CPU 转写约 14 秒中文需约 18 秒，尚未进行 Whisper GPU 加速或性能专项优化。

另外验证了明确指定 `--language=zh` 的 C++ CLI：同一 14.12 秒中文 MP3，解码 44.31 ms、转写 9669.39 ms，完整文字一致。已知语言时建议指定语言，避免自动检测的额外开销。GPU OCR 发布包中的 ASR 也验证成功，设备仍为 CPU。Whisper C ABI 生命周期、重复调用及 CLI 错误检查通过，现有 CPU/GPU OCR 回归测试均通过。

测试中发现 Windows PowerShell 5.1 会把无 BOM UTF-8 脚本中的中文常量按系统 ANSI 编码读取，导致测试文件名乱码。已将测试脚本中的这些常量改为 Unicode 码点构造；SDK 的 UTF-8 路径本身正常。报告通过续测完成，复用了同一 SDK/模型下已经通过的英文及长音频结果。

本机原始数据见 [发布包测试报告](../dist/asr-test-results/20261008_222020/REPORT.md) 和 [完整 JSON](../dist/asr-test-results/20261008_222020/summary.json)。这些发布产物位于被 Git 忽略的 dist；本节保留了可随源码保存的测量摘要。
