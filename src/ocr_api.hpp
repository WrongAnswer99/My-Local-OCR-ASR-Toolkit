// ocr_api.hpp - 对外 SDK 头文件骨架(先给功能清单，供评审后逐个落地)
//
// 目标: 以 ocr.dll + 本头文件分发, 对方 include 后即可:
//   1) 打开/识别 图片或内存 BGR 像素 (现引擎已具备, 直接封装)
//   2) 框选屏幕区域 / 截屏 (现有 region_select/screen 模块可封装)
//   3) 持续盯屏, 命中关键字回调 (ocrmon/ocrwatch 逻辑可封装)
//
// 约定: 全部函数 C ABI(extern "C"), 坐标单位像素;
//       阻塞式 UI 函数(框选)只能在主线程调用。
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(OCR_BUILD_DLL)
#define OCR_API __declspec(dllexport)
#else
#define OCR_API
#endif

// ---------- 基础类型 ----------
// 一条识别结果: 文字 + 置信度 + 四角(左上/右上/右下/左下, 原图坐标)
// center=(四角平均), wh 为外接轴对齐框宽高, 便于定位/点击
typedef struct OcrLine {
    char text[512];
    double score;
    double quad_x[4];
    double quad_y[4];
    int cx, cy;   // 中心点(整数化)
    int w, h;     // 外接轴对齐框
} OcrLine;

typedef struct OcrRect {
    int x, y;     // 左上角(虚拟屏幕绝对坐标)
    int w, h;
} OcrRect;

typedef struct OcrImage {
    int w, h;
    uint8_t* bgr;         // 由 dll 分配, 用 ocr_capture_free 释放
    int stride;           // 每行字节数 (通常 w*3)
} OcrImage;

typedef void* OcrHandle;  // OCR 引擎句柄(内部含 det/rec 两个会话)

typedef enum OcrDevice {
    OCR_DEVICE_CPU = 0,
    OCR_DEVICE_CUDA = 1
} OcrDevice;

// Extensible creation options. Initialize with ocr_default_options first.
// Strings are UTF-8; NULL/empty paths select the default models.
typedef struct OcrCreateOptions {
    uint32_t struct_size;
    int32_t device;
    int32_t gpu_device_id;
    const char* det_model;
    const char* rec_model;
    const char* dict;
    int32_t norm;
    int32_t threads;
    int32_t det_limit;
    double text_score;
} OcrCreateOptions;

OCR_API void ocr_default_options(OcrCreateOptions* options);
// CUDA failures return NULL with a diagnostic; no whole-session CPU fallback.
OCR_API OcrHandle ocr_create_ex(const OcrCreateOptions* options, char* err, int errCap);

// ---------- 1) OCR 引擎(现有能力, 已可封装) ----------
// 创建引擎: 模型参数传 NULL/"" 时使用 dll 同目录 models/onnx 下的默认 v6 tiny;
// norm: 2=PP-OCRv6(默认) 0=PP-OCRv4; detLimit: 0=ROI 原尺寸.
// Legacy API always uses CPU and remains binary compatible.
OCR_API OcrHandle ocr_create(
    const char* detModel, const char* recModel, const char* dict,
    int norm, int threads, int detLimit, double textScore,
    char* err, int errCap);
OCR_API void ocr_destroy(OcrHandle h);
OCR_API const char* ocr_error(OcrHandle h);  // 最近一次错误文本

// 识别一张图片文件(png/jpg/bmp); 返回>0=识别行数(<=cap), -1=失败
OCR_API int ocr_run_file(OcrHandle h, const char* imagePath,
                         OcrLine* out, int cap, char* err, int errCap);
// 识别内存 BGR 像素(宽高必须>0, bgr 须在调用期间有效)
OCR_API int ocr_run_bgr(OcrHandle h, const uint8_t* bgr, int width, int height,
                        OcrLine* out, int cap, char* err, int errCap);

// ---------- 2) 屏幕/框选(现有 region_select/screen 模块可封装) ----------
// [推荐] 程序启动时最先调用: 使进程按物理像素工作(SetProcessDPIAware),
// 否则在 150% 等缩放下, 框选/截屏坐标会被系统虚拟化而偏移(如 2/3)。
// 必须在创建任何窗口/加载可能固定 DPI 的库之前调用; 返回 1=成功 0=失败
// (失败通常意味着进程已被早期代码固定为不感知, 请把本调用放到 main 开头)。
OCR_API int ocr_capture_init(void);
// 全屏遮罩拖框, 返回所选矩形(虚拟屏幕绝对坐标, 与 ocr_watch 等一致)
// 返回 0=已选择; 1=用户取消(Esc/右键); -1=失败
OCR_API int ocr_pick_box(OcrRect* out);
// 抓取屏幕区域为图像; 失败返回 NULL, 用完 ocr_capture_free
OCR_API OcrImage* ocr_capture(const OcrRect* rect);
OCR_API void ocr_capture_free(OcrImage* img);

// ---------- 3) 持续监听: 出现关键字就回调(计划封装 ocrwatch 逻辑) ----------
// 两者都是阻塞调用(在调用线程内循环抓屏->OCR->比对), h=NULL 时自动用默认模型建临时引擎.
// keys/text 匹配按 UTF-8 子串(含中文); 命中任一关键字即算命中.
// 命中回调: 回调里给出当帧命中的整行结果; user 为透传指针
typedef void (*OcrWatchFn)(const OcrLine* hit, int hitCount, void* user);
// 回调版: 命中后调用 onHit(当帧所有命中行) 并返回 0.
// 返回: 0=已命中; 2=超时未命中; -1=出错(err 有内容)
OCR_API int ocr_watch_callback(
    OcrHandle h, const OcrRect* rect,
    const char** keys, int keyCount,
    int intervalMs, int timeoutMs,
    OcrWatchFn onHit, void* user,
    char* err, int errCap);

// 直返版: 命中后把当帧命中的整行写入 out(最多 cap 行), 返回>0=命中行数.
// 返回: >0=命中行数; 0=超时未命中; -1=出错(err 有内容)
OCR_API int ocr_watch(
    OcrHandle h, const OcrRect* rect,
    const char** keys, int keyCount,
    int intervalMs, int timeoutMs,
    OcrLine* out, int cap,
    char* err, int errCap);

// ---------- 4) 其他 ----------
OCR_API int ocr_version(char* buf, int cap);  // 版本/模型信息

#ifdef __cplusplus
}  // extern "C"
#endif
