# GPU 使用指南

本项目使用 PaddleOCR 的 ONNX 模型和 ONNX Runtime C API。GPU 通过 NVIDIA CUDA Execution Provider 加速，检测、识别和可选方向分类都会使用同一设备配置；图像预处理和 OCR 后处理仍在 CPU 上执行。GPU 和 CPU 共享模型、识别结果格式、截图和监听接口。

## 1. 运行库和构建

要求 Windows x64、支持 CUDA 的 NVIDIA 显卡、合适的驱动，以及与 ONNX Runtime GPU 包匹配的 CUDA/cuDNN。GPU 构建也能通过 `--device=cpu` 运行。

下面的脚本默认下载官方 ONNX Runtime **1.23.2** GPU 包到项目 `third_party/onnxruntime-gpu`；CPU 目录不会被覆盖。CUDA 12 和 cuDNN 9 是单独的运行依赖，按所选版本的[官方兼容表](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html#requirements)配置 DLL 搜索路径。`nvidia-smi` 中的 CUDA Version 是驱动支持上限，不能证明 CUDA/cuDNN 运行库已经安装。

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_gpu_runtime.ps1
cmake -S . -B build-gpu -G "MinGW Makefiles" -DOCR_RUNTIME=cuda
cmake --build build-gpu -j 8
```

如果希望在项目内准备完整 CUDA 依赖，避免手动安装或更改系统 PATH，可使用 Python/pip 下载固定版本的 NVIDIA Windows DLL（约 1.5 GB）：

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup_gpu_runtime.ps1 -WithCudaDependencies
cmake -S . -B build-gpu -G "MinGW Makefiles" -DOCR_RUNTIME=cuda -DOCR_CUDA_DLL_DIR="$PWD/third_party/cuda-dlls"
cmake --build build-gpu -j 8
```

此方式包含 CUDA runtime 12.9.79、cuBLAS 12.9.1.4、cuFFT 11.4.1.4、cuDNN 9.9.0.52 和 NVRTC 12.9.86。CMake 把 DLL 复制到程序目录，发布脚本也会自动带上构建时指定的依赖和许可证。没有显卡驱动的机器仍需安装合适的 NVIDIA 驱动。

固定使用 cuDNN 9.9 是性能选择：在 RTX 5060 Laptop 上，同一 2560×1600 全屏截图、相同 v4 mobile 模型和识别参数，9.10.2 的热运行约 2.6 秒，9.9 的对照约 0.63 秒。ORT 1.23.2 的卷积实现在输入形状变化时重建 cuDNN 执行计划，而识别批次宽度会变化；剖析显示主要耗时集中在深度卷积。NVIDIA 的[公开问题报告](https://github.com/NVIDIA/cudnn-frontend/issues/1066)也记录了 Blackwell + cuDNN 9.10.2 深度卷积执行计划编译变慢的现象。这是本机验证的依赖回归修复，不表示所有 GPU/模型都能得到同样加速。原有 DLL 目录再次执行依赖脚本会按版本清单更新。

SDK 初始化 CUDA 时，会按绝对路径预加载 ORT 同目录的 CUDA/cuDNN/NVRTC 库，避免宿主 EXE 位于其他目录时优先找到系统 PATH 中另一套依赖。没有随包提供 DLL 时，仍可使用正确配置的系统运行库。

也可以手动解压[官方 GPU 发布包](https://github.com/microsoft/onnxruntime/releases/tag/v1.23.2)，保持如下结构：

```text
third_party/onnxruntime-gpu/include/onnxruntime_c_api.h
third_party/onnxruntime-gpu/lib/onnxruntime.dll
third_party/onnxruntime-gpu/lib/onnxruntime_providers_shared.dll
third_party/onnxruntime-gpu/lib/onnxruntime_providers_cuda.dll
```

自定义运行库根目录：`-DONNXRUNTIME_DIR=F:/path/to/onnxruntime-gpu`。头文件和 DLL 应来自同一发布包。项目使用 C API 12 接口，运行库需 >= 1.12；设备依赖兼容性以实际选择的发布包为准。CPU/GPU 分别使用独立构建目录，避免 CMake 缓存或旧 DLL 混用。

CPU 构建保持原来方式，也可使用独立目录：

```powershell
cmake -S . -B build-cpu -G "MinGW Makefiles" -DOCR_RUNTIME=cpu
cmake --build build-cpu -j 8
```

## 2. CLI

四个入口 `ppocr_onnx`、`ocrwatch`、`ocrmon` 和 `api_demo` 支持 `--device=cpu|cuda`（`gpu` 为 `cuda` 别名）及 `--gpu-device=<编号>`。默认 CPU、编号 0；`--threads` 控制 CPU 工作线程，不控制 CUDA 线程。

在项目根目录执行：

```powershell
.\build-gpu\ppocr_onnx.exe images/ch_en_num.jpg --device=cuda --gpu-device=0
.\build-gpu\ocrwatch.exe --image=images/ch_en_num.jpg --key=正品促销 --max-frames=1 --device=cuda
.\build-gpu\ocrmon.exe --rect=100,200,600,120 --once --device=cuda
.\build-gpu\api_demo.exe file images/ch_en_num.jpg --device=cuda
```

模型参数 `--det/--rec/--dict`、归一化 `--norm` 和原有 OCR 参数继续可用。加载成功后输出 `backend: cuda`，表示 CUDA provider 已注册且会话已建立；ONNX Runtime 可以把 CUDA 不支持的个别算子分配给 CPU，这不等于整个引擎回退为 CPU。显式选择 CUDA 后，provider 缺失、DLL 加载失败、设备编号错误或会话初始化失败均返回错误，程序不会另建 CPU 会话。

## 3. C++ SDK

`sdk/ocr.hpp` 保持单头文件、运行时加载 DLL，无需链接导入库。原有 `e.init()` 和带位置参数的 `init(...)` 继续使用 CPU。

```cpp
#include "ocr.hpp"

ocr::OCR e;
ocr::InitOptions options;
options.device = ocr::Device::CUDA;
options.gpuDeviceId = 0;
options.threads = 4;
std::string err;
if (!e.init(options, &err)) {
    // 处理错误，err 包含运行库/设备初始化原因
}
std::vector<ocr::Line> lines;
e.runFile("demo.png", lines, &err);
```

内存识别 `runBgr`、截图后识别和 `watch` 自动复用已初始化的 GPU 引擎。要使用屏幕接口，请在任何窗口创建之前调用 `captureInit`。`sdk/examples/gpu_example.cpp` 是可独立编译的 GPU 消费者示例。

新头文件可以加载旧 CPU SDK：CPU 初始化使用原 `ocr_create`；CUDA 初始化会明确提示升级 DLL，不会调用不存在的导出。

## 4. C ABI

原 `ocr_create` 签名、结果结构和所有识别/监听接口保持兼容。新增可扩展结构及初始化入口：

```c
#include "ocr_api.hpp"

OcrCreateOptions options;
ocr_default_options(&options);  /* 设置 struct_size、默认模型配置和 CPU */
options.device = OCR_DEVICE_CUDA;
options.gpu_device_id = 0;
char err[1024] = {0};
OcrHandle h = ocr_create_ex(&options, err, sizeof(err));
if (h) {
    OcrLine lines[256];
    int count = ocr_run_file(h, "demo.png", lines, 256, err, sizeof(err));
    ocr_destroy(h);
}
```

`struct_size` 必须至少为当前结构大小；不支持的设备和负 GPU 编号在加载模型前返回错误。`ocr_create_ex` 创建失败返回 NULL。模型路径可填 NULL/空字符串以使用默认模型。创建配置中的字符串只需在创建调用期间有效。

`ocr_watch`/`ocr_watch_callback` 传入 GPU 句柄即可使用 GPU；传 NULL 保持原有临时 CPU 引擎行为。同一进程中 CPU/GPU 引擎可以共存，所有引擎共享同一份 ONNX Runtime DLL；不能在一个进程中同时加载不同版本的 CPU/GPU ORT 包。

## 5. 发布和测试

```powershell
powershell -ExecutionPolicy Bypass -File tools/make_dist.ps1 -Runtime cuda -BuildDir build-gpu
# 生成 dist-gpu：CLI、ocr.dll、两个 SDK 头文件、GPU provider DLL、模型、示例和文档

powershell -ExecutionPolicy Bypass -File tools/make_dist.ps1 -Runtime cpu -BuildDir build-cpu
# 生成 dist

ctest --test-dir build-cpu --output-on-failure
ctest --test-dir build-gpu --output-on-failure
```

发布脚本从构建缓存读取实际 ORT 目录，并核对 `-Runtime`，避免 SDK 与运行库打包不一致。GPU provider DLL 自动随包复制。CUDA/cuDNN 可安装在目标机器并放入 PATH，也可把所需 DLL 汇总到一个目录后使用 `-CudaDllDir=<目录>` 复制进发布目录；第三方 DLL 再分发时保留其许可证。Windows ORT 包还需安装 [Visual C++ 运行库](https://onnxruntime.ai/docs/install/)。

多配置生成器（如 Visual Studio）打包时，`-BuildDir` 指向含 EXE/DLL 的具体配置子目录，例如 `build-gpu/Release`；脚本会从父构建目录读取 CMake 缓存。

测试需要项目 `models/onnx` 和 `images/ch_en_num.jpg`。CPU 测试验证旧/新 API、C++ SDK、CLI、BGR 输入及 GPU 请求明确失败；CUDA 测试要求真实设备和完整依赖，比较 CPU/GPU 的样例文字和置信度，验证错误 GPU 编号，以及失败后 CPU 句柄仍能推理。测试不执行监听命中后的外部命令。

常见问题：

- `CUDAExecutionProvider unavailable`：当前加载的是 CPU ORT DLL，换用 GPU 发布包。
- Windows error 126：检查 provider DLL、匹配 CUDA/cuDNN DLL 和 VC++ 运行库是否齐全。
- `invalid device ordinal`：检查 `--gpu-device` 是否对应实际可见 GPU。
- cuDNN 8/9 或 CUDA 12/13 混用：核对兼容表和 PATH；不同大版本的 DLL 不能相互替代。
- 小图/小模型没有变快：模型初始化、CPU 预处理和设备数据拷贝可能占主要时间；比较同一常驻引擎上的多次推理，而非每次重启程序。

## 6. 验证实际执行设备和阶段耗时

设置 `OCR_PROFILE_PREFIX` 为可写文件名前缀，销毁引擎时输出检测/识别会话的 ORT JSON 剖析文件。检查 Node 事件的 `args.provider`，可以区分实际分配给 `CUDAExecutionProvider` 和 `CPUExecutionProvider` 的算子。少量 Shape/Gather 等形状算子留在 CPU 是正常的；仅看到 CUDA DLL 被加载不能证明模型计算在 GPU 上执行。剖析中的事件耗时包含主机调度等开销，不能直接当作纯 GPU 内核耗时。

设置 `OCR_TIMING=1` 会在 stderr 输出每次 OCR 的预处理、检测、裁剪、识别耗时（毫秒）。关闭剖析再做正式性能比较，保持同一截图、模型、参数和 ORT 版本。SDK、CLI 和监听接口共享这两个诊断入口。
