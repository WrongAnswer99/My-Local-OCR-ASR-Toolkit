# OCR CPU/GPU 性能排查复盘

日期：2026-10-08。项目：OCRtest。测试机器：NVIDIA RTX 5060 Laptop，8 GB 显存，Windows x64。

这次从给 CPU OCR 项目增加 GPU、CLI 和 SDK 支持开始，随后对发布包做全屏识别测试。第一次测到 GPU 比 CPU 慢，经过实际算子剖析、依赖版本对照和重测，定位到 cuDNN 9.10.2 在本机上的性能回归。最终固定使用 cuDNN 9.9.0.52，v4 mobile 的 GPU 完整 OCR 热运行从约 2.6 秒降到约 0.63 秒。

## 1. 起点：增加 GPU 支持，并验证发布包 SDK

项目使用 PaddleOCR 的 ONNX 模型，通过 C++ 调用 ONNX Runtime C API；并非 Python PaddlePaddle 推理环境。GPU 支持通过 `CUDAExecutionProvider` 实现。

本次增加了 CPU/CUDA 设备配置、CLI 的 `--device=cpu|cuda` 和 `--gpu-device` 参数，以及 C ABI 的 `ocr_create_ex`、C++ SDK 的设备初始化选项。原有 CPU API 保持兼容。CPU 和 GPU 使用独立构建及发布目录，GPU 初始化失败会明确返回错误。

用户要求直接测试发布包：只使用 `dist` 内的运行库、模型和 SDK，允许截取全屏，对 CPU/GPU 和两个模型分别测量耗时。于是编写了 `dist/test_sdk.ps1`，通过 Windows 自带 PowerShell/.NET 调用 SDK 的截图与识别接口。

测试约定如下：

- 截取一次完整虚拟桌面，得到 2560×1600 图像；四种组合共用同一份 BGR 数据。
- 模型为 PP-OCRv6 tiny 和 PP-OCRv4 mobile；v6 使用 norm=2，v4 使用 norm=0。
- threads=8、GPU=0、text_score=0.5、det_limit=0，识别批次保持原来的 6。
- 每个组合在独立进程中创建引擎，分别测量初始化、首次 OCR，以及首次之后 5 次 OCR 的平均值。
- OCR 计时包括预处理、模型推理、后处理和 C ABI 结果转换；排除截图、托管层输入拷贝/输出解码和文件写入。
- 清除第三方 PATH，检查 SDK、ORT 和 CUDA/cuDNN 的实际加载路径都位于 `dist`。系统 PowerShell/.NET、Windows DLL 和 NVIDIA 驱动属于操作系统依赖。

测试运行时没有调用项目外的 Python、OCR 代码或 CUDA 环境。编译和准备依赖属于开发阶段，与发布包运行测试分开。

## 2. 异常：第一次测到 CPU 更快

最初发布包的完整 OCR 热运行平均值如下：

| 模型 | CPU | GPU | 识别条数 CPU/GPU |
|---|---:|---:|---:|
| v6 tiny | 522.73 ms | 566.46 ms | 130 / 130 |
| v4 mobile | 1168.44 ms | 2598.13 ms | 119 / 118 |

这组数据确实显示 CPU 更快，尤其 v4 的 GPU 耗时约为 CPU 的 2.22 倍。但它只能说明当时发布包的表现，不能据此判断 GPU 本身更慢：CPU 包使用 ORT 1.29.0，GPU 包使用 ORT 1.23.2，运行库版本尚未统一。

最初把异常解释为调用及 CPU/GPU 数据传输开销，这个解释缺少剖析证据。检查代码后还发现，识别本来已经按 6 条批处理，并非逐条调用。用户对“是否真的在 GPU 上运行”的质疑推动了后续验证。

## 3. 先证明设备，再定位阶段

加入两个诊断入口：`OCR_PROFILE_PREFIX` 输出 ORT JSON 剖析，`OCR_TIMING=1` 输出预处理、检测、裁剪、识别的阶段耗时。SDK、CLI 共用这些入口。

判断实际设备时，检查 ORT Node 事件中的 `args.provider`，不能仅凭加载了 CUDA DLL 或配置了 GPU 参数作结论。优化后的剖析统计如下，每个模型包含两次完整 OCR（首次及一次热运行）：

| 模型/会话 | CUDAExecutionProvider 事件 | CPUExecutionProvider 事件 |
|---|---:|---:|
| v6 检测 | 380 | 0 |
| v6 识别 | 4100 | 0 |
| v4 检测 | 656 | 0 |
| v4 识别 | 15136 | 528 |

v4 留在 CPU 的操作为 Cast、Concat、Slice；主要模型计算确实分配给 CUDA，没有退回全 CPU。这些是动态执行事件次数，不是模型的静态节点数。事件耗时包含主机调度和执行计划构建，不能当作纯 GPU 内核耗时。

阶段计时显示，v4 的主要瓶颈在识别阶段。原剖析中，卷积约占识别 CUDA 节点事件总耗时的 94%，部分深度卷积出现约 100 ms 的异常开销。显存采样约为 3.85/8 GB，没有观察到显存耗尽。

## 4. 尝试过什么，为什么没有保留

以下是同一截图上的探索性测试，各组重复次数不同，仅用于筛选方向，不作为最终公平基准：

| 尝试 | v4 GPU 热运行平均值 | 判断 |
|---|---:|---|
| 禁止 CPU 工作线程忙等待 | 2574.93 ms | 没有明显改善 |
| 显式启用全部图优化 | 2528.06 ms | 默认已经启用，改善有限 |
| 限制卷积工作区并调整分配策略 | 2548.69 ms | 没有明显改善 |
| 优先 NHWC 布局 | 7997.17 ms | 明显变慢 |
| GPU 识别批次从 6 增至 16 | 1859.14 ms | 提速，但 118 条中有 8 条文字变化 |
| 保持原参数，只换成 cuDNN 9.9 | 634.17 ms | 明显改善，v4 文字和置信度一致 |

无效参数调整均已撤回。扩大批次会改变批内最大宽度及补边方式，影响模型输入；不能只看耗时就把它当成无损优化，因此也没有保留。

## 5. 关键线索：动态输入形状与 cuDNN 版本

检查 [ORT 1.23.2 卷积实现](https://github.com/microsoft/onnxruntime/blob/v1.23.2/onnxruntime/core/providers/cuda/nn/conv.cc)发现，输入形状变化会触发 cuDNN 执行计划重建。OCR 识别批次按文字宽高比排序，批次之间的输入宽度会变化，这使计划构建开销可能反复出现。

同时发现 [NVIDIA 的公开问题报告 #1066](https://github.com/NVIDIA/cudnn-frontend/issues/1066)：在 Blackwell 显卡上，cuDNN 9.10.2 的部分深度卷积执行计划构建出现约 108–152 ms 开销；报告中的 9.9 对照没有同样问题。这个现象与本机剖析相符，但公开问题本身不是本项目的证明。

随后只替换项目内的 cuDNN，保持模型、识别批次和其他参数不变。v4 GPU 热运行降到约 634 ms，118 条文字和置信度与原 GPU 结果完全一致。本机剖析与这个对照实验共同支持：cuDNN 9.10.2 的计划构建性能回归是本次异常的主要原因。没有进一步用 cuDNN API 日志逐条证明内部编译路径。

最终将依赖固定为 cuDNN **9.9.0.52**，更新 `tools/setup_gpu_runtime.ps1` 的下载版本及版本清单，并更新 GPU 发布包和许可证。SDK 和 CLI 共用这套依赖。未修改模型，也未保留扩大批次等影响输入的调整。

## 6. 最终重测：统一 ORT，复用原截图

通过 `-CpuRuntime gpu`，让 CPU 和 GPU 都加载 `dist/gpu-runtime` 中的同一份 ORT 1.23.2 和 SDK，仅改变设备配置。正式计时关闭 ORT 剖析，仍使用原全屏截图，首次后重复 5 次。

| 模式 | 模型 | 初始化 | 首次 OCR | 热运行平均值 | 条数 |
|---|---|---:|---:|---:|---:|
| CPU | v6 tiny | 13329.89 ms | 673.52 ms | 584.85 ms | 130 |
| GPU | v6 tiny | 11432.15 ms | 998.09 ms | 490.46 ms | 130 |
| CPU | v4 mobile | 11344.96 ms | 1394.56 ms | 1290.47 ms | 119 |
| GPU | v4 mobile | 11467.35 ms | 1107.77 ms | 629.74 ms | 118 |

| 模型 | GPU 优化前 | GPU 优化后 | GPU 前后加速 |
|---|---:|---:|---:|
| v6 tiny | 566.46 ms | 490.46 ms | 1.15 倍 |
| v4 mobile | 2598.13 ms | 629.74 ms | 4.13 倍 |

最终两个模型的 GPU 热运行都比同 ORT 的 CPU 更快。不过首次调用仍慢于 CPU，初始化也有明显开销；这些结果适用于常驻引擎连续识别，不能把热运行时间当成程序冷启动时间。

四组发布包 SDK 测试均 PASS，CPU/GPU 的样例图片 SDK、CLI 集成测试也通过。识别结果另行比较：v4 优化前后的 GPU 文字和置信度完全一致；v6 保持 130 条，但有 1 条文字变化，最大置信度差约 0.1222。CPU/GPU 的 v4 条数原本就是 119/118。没有对全屏文字逐字人工校对，接口测试通过不等于识别准确率已经完整评估。

## 7. 网上基准如何理解

[PaddleOCR 官方基准](https://www.paddleocr.ai/main/en/version3.x/pipeline_usage/OCR.html)中，PP-OCRv4 mobile 普通模式的检测耗时为 GPU 9.87 ms / CPU 56.60 ms，识别耗时为 GPU 5.26 ms / CPU 17.48 ms。其硬件为 Tesla T4 / Xeon Gold 6271C，使用 Paddle 后端及其他输入集。

官方数据支持 GPU 通常应当更快的预期，但测的是独立模型推理，不能将其毫秒数直接与本项目包含预处理、检测、多个文字批次识别和后处理的全屏 OCR 相比。

## 8. 证据与复现

本地测试产物位于被 Git 忽略的 `dist`，本复盘保存在项目 `docs` 中。关键产物：

- [原始测试报告](../dist/sdk-test-results/20261008_201947/REPORT.md)
- [优化后测试报告](../dist/sdk-test-results/20261008_210139/REPORT.md)
- [优化后完整数据](../dist/sdk-test-results/20261008_210139/summary.json)
- [实际算子设备汇总](../dist/sdk-benchmark/gpu-proof/provider-summary.json)
- [发布包测试脚本](../dist/test_sdk.ps1)
- [GPU 使用与诊断说明](GPU.md)

在项目根目录执行以下命令，可只使用发布包环境，复用原截图重新测试四组：

```powershell
$originalReport = (Resolve-Path .\dist\sdk-test-results\20261008_201947).Path
powershell -NoProfile -ExecutionPolicy Bypass -File .\dist\test_sdk.ps1 `
  -ReplayReportDir $originalReport -CpuRuntime gpu -Repeat 5

$profileDir = (Resolve-Path .\dist\sdk-benchmark\gpu-proof).Path
powershell -NoProfile -ExecutionPolicy Bypass -File .\dist\summarize_profile.ps1 `
  -ProfileDir $profileDir
```

原 SDK 保存在 `dist/ocr-before-optimization.dll` 和 `dist/gpu-runtime/ocr-before-optimization.dll`；旧 cuDNN 保存在 `dist/sdk-benchmark/cudnn910-backup/`。只切换 SDK DLL 不会切换 cuDNN，恢复旧 GPU 环境做对照时必须同时匹配依赖版本。

这次的经验是：遇到 GPU 性能反常，先验证实际算子设备，再拆解阶段耗时，并控制截图、参数和运行库版本。优化既要检查速度，也要比较文字输出。版本更新可能带来性能回归，依赖对照与剖析证据比凭经验猜测原因更可靠。
