# PP-OCR C++ SDK（ONNX Runtime CPU 版）

基于 PP-OCR 推理模型（默认 **PP-OCRv6 tiny**，可选 PP-OCRv4）+ ONNX Runtime 的轻量 C++ OCR 引擎与屏幕监听工具。

> 开发机 Windows + MinGW g++ + CMake 构建；推理只用 CPU，不依赖 GPU，办公本可直接跑。

---

## 目录

- [1. 工具（命令行 exe）](#1-工具命令行-exe)
- [2. SDK 发布物（dll + 头文件）](#2-sdk-发布物dll--头文件)
- [3. C ABI API（ocr_api.hpp）详解](#3-c-abi-apiocr_apihpp详解)
- [4. C++ 便捷封装（ocr.hpp）详解](#4-c-便捷封装ocrhpp详解)
- [5. 结构体字段说明](#5-结构体字段说明)
- [6. 使用前必读](#6-使用前必读)
- [7. 构建方法](#7-构建方法)
- [8. 给使用者（收到 ocr.hpp + ocr.dll 的人）](#8-给使用者收到-ocrhpp--ocrdll-的人)

---

## 1. 工具（命令行 exe）

| 工具 | 作用 |
|---|---|
| `ppocr_onnx.exe` | 识别单张图片，输出文字 + 置信度 + 四角坐标；可 `--viz` 画框 |
| `ocrmon.exe` | 框选屏幕区域 → 持续打印该区域识别出的文字 |
| `ocrwatch.exe` | 监听屏幕/图片，出现指定关键字就执行命令并退出（退出码区分结果） |
| `region_picker.exe` | 全屏框选，把 `--rect=x,y,w,h` 输出到终端并复制到剪贴板 |
| `screen_point_picker.exe` | 半透明全屏选点；记录并标注点击坐标，同时让点击正常落到下层窗口 |
| `api_demo.exe` | SDK 用法示例（file / pick / watch 三个子命令） |

常用参数（三个 OCR 工具通用）：

```
--det=<onnx> --rec=<onnx> --dict=<txt>  模型文件(默认 v6 tiny, 在 models/onnx)
--norm=<0|2>      2=PP-OCRv6(默认, 输入 v/255); 0=PP-OCRv4(v/127.5-1)
--det-limit=<n>   0=ROI 原尺寸直识别(默认); n>0=短边不足放大到 n(小字召回更强, 更慢)
--threads=<n>     onnxruntime 线程数(0=自动; 建议 8)
--score=<num>     置信度阈值(默认 0.5)
```

调试环境变量：
- `OCR_DEBUG=1`：打印推理各阶段输入尺寸（定位崩溃/形状问题用）；
- `OCR_TRACE=1`（仅 ocrmon）：把每帧截图存到 `images/ocr_trace/`。

`sdk/examples/` 下是 SDK 用法示例：
- `example.cpp`：最小纯消费者（单文件，只 include `ocr.hpp`，编译即用，见第 8.1 节）
- `api_demo.cpp`：`file / pick / watch` 三个子命令的完整演示

---

## 2. SDK 发布物（dll + 头文件）

给别人用时，**核心是下面 4 样且必须在同一个目录**（最省事是直接把整个 `dist/` 发过去，见 8.1）：

```
ocr.dll      核心引擎(已静态链接 gcc 运行库, 免装 MinGW 运行库)
onnxruntime.dll     ONNX Runtime CPU 运行库
models\onnx\…       默认 v6 tiny 模型(det/rec/dict); 也支持 v4 三件
ocr.hpp      C++ 便捷封装(单头文件, 内部 LoadLibrary 动态加载, 无需链接)
```

- C 语言使用者还需 `ocr_api.hpp`（C ABI 声明）。
- dll 内用 `LoadLibrary` 加载 onnxruntime，模型缺省在 **dll 同目录 `models\onnx`** 下找，找不到再回退当前工作目录。

---

## 3. C ABI API（ocr_api.hpp）详解

全部函数为 `extern "C"，与编译器无关（MSVC/MinGW/C#/Python ctypes 都能调）。坐标单位：像素。

### 3.1 OCR 引擎

```c
OcrHandle ocr_create(
    const char* detModel, const char* recModel, const char* dict,
    int norm, int threads, int detLimit, double textScore,
    char* err, int errCap);        // 返回引擎句柄; NULL=失败, err 有原因
void      ocr_destroy(OcrHandle h);          // 释放引擎
const char* ocr_error(OcrHandle h);          // 最近一次错误文本
```

| 参数 | 含义 |
|---|---|
| `det/rec/dict` | 模型路径；传 `NULL/""` 用默认 **v6 tiny** |
| `norm` | **2**=PP-OCRv6（输入 v/255，默认）；**0**=PP-OCRv4（v/127.5-1） |
| `threads` | 推理线程数，`0`=自动 |
| `detLimit` | `0`=按原尺寸识别（ROI 快）；`>0`=短边不足放大到该值（全图小字用 736） |
| `textScore` | 置信度阈值，`<=0` 视为 0.5 |

### 3.2 识别

```c
int ocr_run_file(OcrHandle h, const char* imagePath,
                 OcrLine* out, int cap, char* err, int errCap);
int ocr_run_bgr (OcrHandle h, const uint8_t* bgr, int width, int height,
                 OcrLine* out, int cap, char* err, int errCap);
```

- 支持 png/jpg/bmp；`run_bgr` 输入连续 BGR 像素。
- 返回 **>0=识别行数**（填满最多 `cap` 行）；**-1=失败**（err 有原因）。
- 返回的坐标原点 = **你输入图片/像素的左上角**。

### 3.3 屏幕 / 框选

```c
int       ocr_capture_init(void);     // [必须最先调] 见 6.使用前必读
int       ocr_pick_box(OcrRect* out); // 全屏遮罩拖框; 0=已选 1=取消 -1=失败
OcrImage* ocr_capture(const OcrRect* rect);   // 抓屏, NULL=失败
void      ocr_capture_free(OcrImage* img);    // 释放抓屏结果
```

- `ocr_pick_box`：阻塞式 UI（自带消息循环），输出**虚拟屏幕绝对坐标**，可直接喂给 watch。
- `ocr_capture(rect)`：`rect==NULL` 抓全屏；返回图像需 `ocr_capture_free` 释放。

### 3.4 持续监听（watch）

```c
// 回调版: 命中后调用 onHit(当帧所有命中行) 再返回 0
int ocr_watch_callback(OcrHandle h, const OcrRect* rect,
                       const char** keys, int keyCount,
                       int intervalMs, int timeoutMs,
                       OcrWatchFn onHit, void* user,
                       char* err, int errCap);

// 直返版: 命中后把当帧命中行写入 out(最多 cap), 返回命中行数
int ocr_watch(OcrHandle h, const OcrRect* rect,
              const char** keys, int keyCount,
              int intervalMs, int timeoutMs,
              OcrLine* out, int cap,
              char* err, int errCap);
```

| 参数 | 含义 |
|---|---|
| `h` | 引擎句柄；**传 NULL** 时内部自动创建默认引擎 |
| `rect` | 监听区域（屏幕绝对坐标）；**NULL=全屏** |
| `keys[]` | 关键字数组，命中任一即算命中；按 **UTF-8 子串**匹配（支持中文） |
| `intervalMs` | 每帧间隔毫秒 |
| `timeoutMs` | 总超时；`0`=无限等待 |
| `onHit / user` | 命中回调 + 透传指针（相当于回调的 “this”） |
| `err` | 出错/超时原因 |

返回值：

| API | 命中 | 超时 | 出错 |
|---|---|---|---|
| `ocr_watch_callback` | 0 | 2 | -1 |
| `ocr_watch` | >0（命中行数） | 0 | -1 |

> 语义：**阻塞**调用——内部在调用线程里循环“抓屏 → OCR → 比对”，命中/超时才返回。想让程序不被卡住，就在你自己的线程里调用它；或把 `ocr_watch` 放后台线程等结果。

### 3.5 其他

```c
int ocr_version(char* buf, int cap);   // 版本/模型信息
```

---

## 4. C++ 便捷封装（ocr.hpp）详解

命名空间 `ocr`，类 `OCR`。内部用 `LoadLibrary + GetProcAddress` 动态加载 `ocr.dll`，**不需要链接导入库**，任何编译器（MSVC/MinGW/Clang）include 即可用。

```cpp
#include "ocr.hpp"

ocr::OCR e;
e.loadDll("ocr.dll");       // 加载 dll
e.captureInit();                   // [必须] DPI 初始化, 见第 6 节
e.init("", "", "", 2, 8, 0, 0.5);  // 默认 v6 tiny + 8 线程
```

| 方法 | 作用 |
|---|---|
| `bool loadDll(name)` | 加载 dll（自动搜 exe 目录/系统路径） |
| `bool captureInit()` | DPI 初始化；**程序启动后最早调用** |
| `bool init(det, rec, dict, norm, threads, detLimit, score)` | 创建引擎；参数同 `ocr_create` |
| `int runFile(path, vector<Line>&)` | 识别图片文件，返回行数（<0 出错） |
| `int runBgr(bgr, w, h, vector<Line>&)` | 识别内存 BGR |
| `bool pickBox(Rect&)` | 全屏框选，取消返回 false |
| `Image capture(const Rect* = nullptr)` | 截屏（rect 为空=全屏） |
| `int watch(const Rect*, keys, intervalMs, timeoutMs, vector<Line>&)` | 阻塞等关键字，返回命中行数；0=超时；<0 出错 |
| `std::string version()` / `void close()` | 版本 / 释放 |

返回值约定与 C API 相同；`errOut` 参数可拿到错误/超时原因文本。

---

## 5. 结构体字段说明

```c
// 一条识别结果
typedef struct OcrLine {
    char   text[512];   // 识别文本(UTF-8)
    double score;       // 置信度 0~1
    double quad_x[4];   // 四角 X: 左上、右上、右下、左下(原图坐标)
    double quad_y[4];   // 四角 Y
    int    cx, cy;      // 中心点(四角平均, 便于定位/点击)
    int    w, h;        // 外接轴对齐框宽高
} OcrLine;

typedef struct OcrRect { int x, y; int w, h; } OcrRect;   // 屏幕/图片矩形
typedef struct OcrImage { int w, h; uint8_t* bgr; int stride; } OcrImage;
```

坐标系提醒：

- `run_file / run_bgr` 的结果坐标 → **图片自身**的像素坐标。
- `pick_box / capture / watch` 的矩形与命中结果 → **屏幕虚拟坐标系**（左上角为原点、向右/下为正），`capture` 里 OCR 出的坐标是**相对该截图**的，要换算成屏幕绝对坐标需加上 `rect.x/rect.y`。

---

## 6. 使用前必读

1. **DPI（缩放屏必须）**：Windows 在 150% 等缩放下会把不感知的程序坐标“虚拟化”（真实位置 ×⅔ 的偏移）。所以：
   - 你的程序**创建任何窗口之前**，第一个调用 `ocr_capture_init()`（C++ 用 `OCR::captureInit()`）。
   - 若忘记调/调太晚，`ocr_pick_box` 会拒绝返回错误坐标，`ocr_capture/watch` 会返回 DPI 相关错误提示。
2. **编码**：API 输入/输出文本均为 **UTF-8**；控制台请先 `SetConsoleOutputCP(CP_UTF8)` 以免中文乱码。
3. **线程**：单个引擎句柄不要并发调用；需要并行可建多个句柄（每个 `init` 一次）。`pick_box/watch` 是阻塞式，可放到你的工作线程。
4. **模型目录**：默认 v6 tiny 三件套（`v6_det_tiny.onnx` / `v6_rec_tiny.onnx` / `v6_tiny_dict.txt`）要放在 dll 同目录 `models\onnx\` 下；换 v4 时传对应路径并把 `norm` 设为 0。

最小可运行示例：

```cpp
#include "ocr.hpp"
int main() {
    SetConsoleOutputCP(CP_UTF8);
    ocr::OCR e;
    e.captureInit();                       // 必须在任何窗口前
    e.init();                              // 默认 v6 tiny
    std::vector<ocr::Line> lines;
    if (e.runFile("demo.png", lines) > 0)
        for (auto& l : lines)
            printf("[%.2f] %s @(%d,%d)\n", l.score, l.text.c_str(), l.cx, l.cy);
    return 0;
}
```

---

## 7. 构建方法

环境要求：

- Windows 10/11 **x64**（屏幕抓取、dll 均为 64 位）
- **MinGW-w64 g++**（64 位，支持 C++17；本仓库用 g++ 11+ 验证）
- **CMake ≥ 3.16**
- 以上命令需在 `PATH` 中。用 MSYS2 装齐的示例：`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-cmake`，再把 `msys64\mingw64\bin` 加入 PATH。

首次准备（仓库把 `models/`、`third_party/`、`build/`、`dist/` 都放进了 .gitignore，**大体积的模型与第三方运行库不随源码提交**，fresh clone 后需要手动放一次）：

1. 按 [7.1](#71-第三方依赖布局与获取) 放好 `third_party/onnxruntime/`（include + lib）与 `third_party/stb/`（两个单文件头）；
2. 按 [7.2](#72-模型来源与许可) 放好 `models/onnx/` 下 6 个模型文件；
3. 再执行下面的 configure/build。

```powershell
# 主工程(工具 + ocr.dll + api_demo)
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

产物位置：`build/` 下有各 exe、`ocr.dll`（onnxruntime.dll 已自动拷到 exe 旁）。

运行示例（**在仓库根目录运行**，因为 CLI 工具默认从当前目录的 `models/onnx` 找模型；各工具不带参数会打印用法）：

```powershell
build\ppocr_onnx.exe   示例图片.png          # 识别单张图
build\region_picker.exe                     # 全屏框选, 输出 --rect=x,y,w,h
build\screen_point_picker.exe               # 记录真实点击坐标；在控制台按 Ctrl+C 退出
build\ocrmon.exe                            # 框选后持续打印识别文字
build\ocrwatch.exe                          # 监听屏幕, 出现关键字触发
```

`screen_point_picker` 只观察鼠标按下事件，不拦截点击，也不处理鼠标移动、拖拽或键盘输入。左、右、中键和 X1/X2 按键都会按顺序显示在覆盖层并输出到控制台；点击仍由光标下方的真实窗口处理。按住左键再按右键（或反过来），当左右键同时处于按下状态时，工具会在传递真实点击后退出覆盖层，并执行 `pause` 保留控制台记录。工具启动时会设置 **Per-Monitor V2 DPI awareness**，因此不同显示器分别使用 100%/125%/150% 缩放时，覆盖层与点击点仍使用一致的物理屏幕坐标；运行中改变显示缩放或显示器布局也会刷新覆盖范围。可用 `--alpha=0..255` 调整遮罩透明度（默认 `96`）。

> 若想从任意目录运行，把 `models/onnx` 也放到 exe 同目录，或用 `--det/--rec/--dict` 传绝对路径。

最小单文件示例（不需要 CMake，`sdk/examples/example.cpp` 只用 dll + 头文件）：
```powershell
# 把 example.cpp 和 ocr.hpp 放同目录后：
g++ -O2 example.cpp -o example.exe
```

> 模型默认 v6 tiny（最快）；如要更精准的小字识别，用 v4 mobile 并把 `--norm=0`（见第 1 节示例工具，SDK 中同样适用）。

### 7.1 第三方依赖（布局与获取）

所有第三方都放在 `third_party/`，由 [CMakeLists.txt](CMakeLists.txt) 顶部按 My-Game-Demo 的方式**启动时检查**：缺了就 `FATAL_ERROR` 并给出获取指引，不联网自动下载。

| 目录 | 内容 | 用途 | 获取方式 |
|---|---|---|---|
| `third_party/onnxruntime/` | `include/`(C API 头) + `lib/onnxruntime.dll` | ONNX 推理运行库：编译期只用头文件，运行期 `LoadLibrary` 延迟加载 | 从 GitHub 下载 Windows x64 CPU 发布包并解压，把 `include/`、`lib/` 放到该目录：<br>`https://github.com/microsoft/onnxruntime/releases` |
| `third_party/stb/` | `stb_image.h` `stb_image_write.h` | header-only 图像读写 | 从 GitHub 拿两个单文件头（或整仓 clone 到该目录）：<br>`https://github.com/nothings/stb` |

说明：
- onnxruntime 与 stb 没有本地 `CMakeLists.txt`，无法 `add_subdirectory`，因此在 CMake 里登记为 **INTERFACE 目标**（只提供 include 路径），需要的地方 `target_link_libraries(... onnxruntime stb)` 即可；
- 旧的 `third_party/clipper` 依赖已弃用并移除；
- 对外分发（`dist/`）只需要 `ocr.dll` + `onnxruntime.dll` + `models/`，见第 2、8 节。

### 7.2 模型来源与许可

`models/onnx/` 下的推理模型**不是本项目训练/生成的**，全部来自 PaddleOCR（百度飞桨官方，[PaddlePaddle/PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)），许可为 **Apache-2.0**。本项目只做 ONNX Runtime 推理，不改权重。

| 文件 | 对应官方模型 | 说明 |
|---|---|---|
| `v6_det_tiny.onnx` | PP-OCRv6 tiny det | 默认模型（最快） |
| `v6_rec_tiny.onnx` | PP-OCRv6 tiny rec | 同上 |
| `v6_tiny_dict.txt` | PP-OCRv6 识别字典 | 6906 类（含 blank/空格） |
| `ch_PP-OCRv4_det_mobile.onnx` | PP-OCRv4 mobile det（超轻量） | `--norm=0` 时使用 |
| `ch_PP-OCRv4_rec_mobile.onnx` | PP-OCRv4 mobile rec | 同上 |
| `ppocr_keys_v1.txt` | 官方通用字典（`ppocr/utils/ppocr_keys_v1.txt`） | 106 语言常用字符 |

获取方式（官方渠道）：
- 中文模型列表（PP-OCRv4 等历史版本）：<https://github.com/PaddlePaddle/PaddleOCR/blob/release/2.7/doc/doc_ch/models_list.md>
- 3.x 模型列表（含 PP-OCRv6 的 det/rec 下载链接）：<https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/model_list.md>
- PP-OCRv6 官方 ONNX：ModelScope 官方仓库 `PaddlePaddle/PP-OCRv6_tiny_det_onnx` / `PP-OCRv6_tiny_rec_onnx`；PaddleOCR ≥ 3.7 也可直接 `engine="onnxruntime"` 自动下载 ONNX。
- 本项目的 `.onnx` 即上述官方推理模型导出的 ONNX 版；算法前后处理与官方对齐（v4 输入 `v/127.5-1`，v6 输入 `v/255`，对应 `--norm` 参数）。

### 7.3 一键打包对外分发目录（dist）

先构建主工程（上面命令），然后：

```powershell
# PowerShell 当前会话直接执行
& tools\make_dist.ps1
```

生成 `dist/`（内容见第 2、8.1 节），自动包含：`ocr.dll`、`onnxruntime.dll`、`ocr.hpp`、`ocr_api.hpp`、`README.md`、`example.cpp`，以及 `models\onnx\` 下的 6 个模型，并清理目录里多余/旧的模型文件。把整个 `dist/` 发给别人即可（见第 8 节）。

---

## 8. 给使用者（收到 `ocr.hpp` + `ocr.dll` 的人）

这一节站在**拿到 SDK 的人**的角度写：不需要了解引擎内部，只要会用 C/C++ 编译并运行自己的程序即可。

### 8.1 你会收到什么、放哪里

把整个 `dist` 目录原样放到你的工程里（或 exe 所在目录）：

```
dist/
├─ ocr.hpp             ← C++ 封装: 你只 include 这一个(C++ 用)
├─ ocr_api.hpp         ← C ABI 声明(用 C/C#/Python 直调时才需要)
├─ ocr.dll             ← 引擎本体(静态链接了 gcc 运行库, 无需另装)
├─ onnxruntime.dll     ← 推理运行库(必须和 dll 同目录)
├─ README.md           ← 这份说明
├─ example.cpp         ← 最小示例源码(见 8.2, 可直接改)
└─ models/
   └─ onnx/
      ├─ v6_det_tiny.onnx / v6_rec_tiny.onnx / v6_tiny_dict.txt
      │                          ← 默认 PP-OCRv6 tiny(缺一不可, 保持相对位置)
      └─ ch_PP-OCRv4_det_mobile.onnx / ch_PP-OCRv4_rec_mobile.onnx
                                 / ppocr_keys_v1.txt   ← 可选 v4(用 --norm=0/init norm=0)
```

要点：

- **只需 include** `ocr.hpp`，**不需要配置任何链接库、不需要 vcpkg/CMake**。
- dll 内部运行时用 `LoadLibrary` 自己加载 onnxruntime 和模型，所以你**不用管链接**，只要保证上面 4 个文件在你程序的**运行目录**里（通常是 `Debug/`、`Release/` 或 exe 旁）。
- C 语言（或 C#/Python 想直接调底层）用 `ocr_api.hpp` 里的 `extern "C"` 函数，但日常 C++ 开发建议直接用本节的封装。

### 8.2 三分钟跑通（识别一张图片）

新建 `main.cpp`，复制以下全部代码：

```cpp
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <vector>
#include "ocr.hpp"      // 改成你实际的 include 路径

int main() {
    SetConsoleOutputCP(CP_UTF8);          // 避免中文乱码(建议)

    ocr::OCR e;
    std::string err;
    if (!e.loadDll("ocr.dll", &err) ||
        !e.captureInit(&err) ||           // 必须先调! 见 8.4
        !e.init("", "", "", 2, 8, 0, 0.5, &err)) {
        std::fprintf(stderr, "init failed: %s\n", err.c_str());
        return 2;
    }

    std::vector<ocr::Line> lines;
    int n = e.runFile("要识别的图片.png", lines, &err);   // png/jpg/bmp 均可
    if (n < 0) { std::fprintf(stderr, "run: %s\n", err.c_str()); return 3; }
    std::printf("识别到 %d 行:\n", n);
    for (auto& l : lines)
        std::printf("[%.2f] %s  中心(%d,%d) 尺寸%dx%d\n",
                    l.score, l.text.c_str(), l.cx, l.cy, l.w, l.h);
    return 0;
}
```

编译（任选一种编译器，**不需要链接任何库**）：

```powershell
# MSVC(Visual Studio 开发者命令行)
cl /EHsc main.cpp /utf-8

# MinGW g++
g++ main.cpp -o main.exe -O2 -static
```

把 `dist/` 的文件放到 `main.exe` 同目录后运行 `main.exe`，即可看到图片里的文字与坐标。

### 8.3 三个常用场景的代码骨架

**A. 持续检测屏幕某区域，逐帧输出文字**（类似 ocrmon）：

```cpp
ocr::Rect r{100, 200, 600, 120};      // 屏幕区域(物理像素坐标)
for (;;) {
    ocr::Image shot = e.capture(&r, &err);
    std::vector<ocr::Line> lines;
    if (e.runBgr(shot.bgr.data(), shot.w, shot.h, lines, &err) > 0)
        for (auto& l : lines)
            std::printf("%s\n", l.text.c_str());
    Sleep(500);                       // 帧间隔
}
```

**B. 先让用户框选区域，再等“你好”出现**（即 `sdk/examples/example.cpp` 的完整逻辑）：

```cpp
ocr::Rect region;
if (!e.pickBox(region, &err)) { printf("取消框选\n"); return 1; }
std::vector<std::string> keys = {"你好"};
std::vector<ocr::Line> hits;
int n = e.watch(&region, keys, 200, 0, hits, &err);   // 0 超时=无限等
if (n > 0) printf("你好，世界\n");                      // n>0=命中
```

**C. 不弹 UI、直接给坐标监听**：把上例 `pickBox` 换成
```cpp
ocr::Rect region{ x, y, w, h };   // 坐标可用我们提供的 region_picker 工具得到
```

### 8.4 注意事项与常见问题（FAQ）

1. **为什么必须先调 `captureInit()`？**
   屏幕类功能需要“物理像素”坐标。Windows 在 150% 缩放下，不感知的程序坐标会被系统虚拟化（偏移成真实位置的约 ⅔）。`captureInit()`（内部 `SetProcessDPIAware`）必须在**创建任何窗口之前**调用。只要使用 `pickBox/capture/watch`，就请把它放在 `main` 最开头。
2. **运行报“cannot load ocr.dll”** → dll 没放在 exe 同目录（或系统路径）。把 4 个文件放到一起。
3. **运行报“CreateSession … File doesn't exist”** → `models/onnx/` 三个文件没放对位置（应相对 dll 同目录），或传了错误的自定义模型路径。
4. **中文输出乱码** → 程序先执行 `SetConsoleOutputCP(CP_UTF8)`。
5. **识别很慢** → 传 `init(..., threads=8, ...)`；监听小区域用 `detLimit=0`（原尺寸直识别）。低配 CPU 单帧约 40~200ms，足够做界面文字监听。
6. **识别不到小字** → `init` 里 `detLimit` 传 `480`~`736` 会放大检测输入，小字召回提升但更慢。
7. **能不能多线程？** 一个 `OCR` 实例不要并发调用；要并行就建多个 `OCR`。`pickBox/watch` 是阻塞函数，放到你自己的后台线程即可。
8. **对方机器报缺 `vcruntime140.dll`**（少见）→ 安装一次“微软 VC++ 运行库 2015-2022”。这是 onnxruntime.dll 的要求，不是本 SDK 的问题。
9. **什么时候用 v4 模型？** 把 `init` 前三个参数换成 `models/onnx/ch_PP-OCRv4_det_mobile.onnx` 等，并把 `norm` 设为 `0`（v6 是 `2`）。

### 8.5 给“不会写代码”的最终用户

如果你只是要把功能（如“出现某文字就做某事”）交给一个不懂编程的人，推荐直接发**打包好的 exe 形式**（用 `sdk/examples/example.cpp` 编译成一个自包含目录：exe + `ocr.dll` + `onnxruntime.dll` + `models\onnx`），并附一行说明：

```
把本文件夹整个解压，运行 example.exe
先拖框圈住要看的地方 → 出现“你好”就会自动提示
```

这时对方**不需要**安装任何东西、不需要编译器。
