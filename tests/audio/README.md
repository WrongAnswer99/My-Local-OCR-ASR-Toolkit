# ASR 测试音频

仓库保留 `jfk.wav`、`expected-chinese.txt` 和本说明。其余音频是生成产物，由 `.gitignore` 排除；运行完整发布包测试前，先在项目根目录按下方命令生成，再执行 `tools/make_dist.ps1`，将样本复制到发布包的 `asr-samples`。

- `jfk.wav`：来自 whisper.cpp v1.9.5 的公开真实语音样本 [`samples/jfk.wav`](https://github.com/ggml-org/whisper.cpp/blob/v1.9.5/samples/jfk.wav)，内容为 JFK 演讲中的 “ask not what your country can do for you…”。
- `long-jfk.wav`：将上述样本连续播放 3 遍，验证转写覆盖 30 秒之后的语音。
- `中文语音.wav`：Windows `Microsoft Huihui Desktop` 合成声音生成，文本如下。
- `中文语音.mp3` / `中文语音.m4a`：由上述 WAV 转换为 44.1 kHz 双声道 MP3、48 kHz 双声道 AAC/M4A，验证解码、混音及重采样。

中文合成文本：

> 你好，这是一次语音识别测试。我们正在使用本地模型，将整个音频转换成文字。测试完成以后，请保存识别结果。

开发阶段生成方式（系统需要该中文 TTS 声音及 FFmpeg；发布包转写和测试运行不需要 FFmpeg）：

```powershell
Add-Type -AssemblyName System.Speech
$voice = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voice.SelectVoice('Microsoft Huihui Desktop')
$voice.SetOutputToWaveFile((Join-Path $PWD 'tests/audio/中文语音.wav'))
$voice.Speak('你好，这是一次语音识别测试。我们正在使用本地模型，将整个音频转换成文字。测试完成以后，请保存识别结果。')
$voice.Dispose()
ffmpeg -y -i tests/audio/中文语音.wav -ar 44100 -ac 2 tests/audio/中文语音.mp3
ffmpeg -y -i tests/audio/中文语音.wav -ar 48000 -ac 2 -c:a aac tests/audio/中文语音.m4a
ffmpeg -y -stream_loop 2 -i tests/audio/jfk.wav -ar 16000 -ac 1 tests/audio/long-jfk.wav
```

这些样本用于端到端功能验证，没有组成准确率或通用性能评估数据集。
