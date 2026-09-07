// ocr.hpp - SDK 配套的 C++ 便捷封装(自包含, 单头文件)。
//
// 分发方式: ocr.dll + onnxruntime.dll + models/ 目录 与本头文件一起
// 放到对方工程; 对方只需:
//   #include "ocr.hpp"
//   ocr::OCR e;
//   e.init();                       // 默认 v6 tiny(模型在 dll 旁 models/onnx)
//   e.runFile("a.png", lines);      // 识别
// 本封装运行时用 LoadLibrary 加载 ocr.dll(在 exe/dll 同目录或系统路径),
// 因此对方不需要链接导入库, 用任何编译器(MSVC/MinGW/…)都行。
// 更底层的 C 接口见 ocr_api.hpp; 非回调 watch/框选/截屏等均可从 C 接口直调。
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ocr {

// 一条识别结果(与 C 接口 OcrLine 同语义)
struct Line {
    std::string text;
    double score = 0.0;
    double quad_x[4] = {0, 0, 0, 0};  // 左上/右上/右下/左下(原图坐标)
    double quad_y[4] = {0, 0, 0, 0};
    int cx = 0, cy = 0;  // 中心
    int w = 0, h = 0;    // 外接框
};

// 屏幕矩形(虚拟屏幕绝对坐标, 与 ocrwatch/ocrmon 一致)
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

// 截屏图像
struct Image {
    int w = 0, h = 0, stride = 0;
    std::vector<uint8_t> bgr;  // BGR, 行优先
};

// 单引擎句柄(可多个实例并行; 单实例不建议跨线程并发调用)
class OCR {
public:
    OCR() = default;
    ~OCR() { close(); }
    OCR(const OCR&) = delete;
    OCR& operator=(const OCR&) = delete;

    // 加载 dll(缺省名 ocr.dll, 会依次搜索 exe 目录/系统路径)
    bool loadDll(const std::string& dllName = "ocr.dll",
                 std::string* errOut = nullptr);
    bool isLoaded() const { return hDll_ != nullptr && handle_ != nullptr; }

    // [必须] 程序启动后第一个调用本类方法的地方调用:
    // 让进程按物理像素工作(内部 ocr_capture_init -> SetProcessDPIAware)。
    // 必须在创建任何窗口之前调用, 否则缩放屏(如 150%)下框选/截屏坐标会偏。
    // 失败(errOut 非空)说明调用太晚, 请把它移到 main 最开头。
    bool captureInit(std::string* errOut = nullptr);

    // 创建引擎。det/rec/dict 留空 => dll 同目录 models/onnx 默认 v6 tiny。
    // norm: 2=v6(默认) 0=v4; threads<=0 自动; detLimit=0 ROI 原尺寸。
    bool init(const std::string& det = "", const std::string& rec = "",
              const std::string& dict = "", int norm = 2, int threads = 0,
              int detLimit = 0, double textScore = 0.5,
              std::string* errOut = nullptr);

    // 识别图片文件(png/jpg/bmp), 返回识别行数; <0 失败
    int runFile(const std::string& imagePath, std::vector<Line>& out,
                std::string* errOut = nullptr);
    // 识别内存 BGR(宽高>0, bgr 在调用期间有效)
    int runBgr(const uint8_t* bgr, int w, int h, std::vector<Line>& out,
               std::string* errOut = nullptr);

    // 全屏遮罩拖框; 成功返回 true, 取消返回 false (errOut="cancelled")
    bool pickBox(Rect& out, std::string* errOut = nullptr);
    // 截取屏幕区域到内存图像(rect 为空则全屏); 失败返回空图像
    Image capture(const Rect* rect = nullptr, std::string* errOut = nullptr);

    // 阻塞监听屏幕: 出现任一 key 立即返回命中行(>0 行); 超时返回 0
    // (errOut="timeout"); rect 为空监听全屏; timeoutMs<=0 表示无限等
    int watch(const Rect* rect, const std::vector<std::string>& keys,
              int intervalMs, int timeoutMs, std::vector<Line>& hits,
              std::string* errOut = nullptr);

    void close();
    std::string version();

private:
    // ---- 与 C ABI 对齐的镜像结构(不依赖 ocr_api.hpp, 便于单头分发) ----
    struct ApiLine {
        char text[512];
        double score;
        double quad_x[4];
        double quad_y[4];
        int cx, cy, w, h;
    };
    struct ApiRect {
        int x, y, w, h;
    };
    struct ApiImage {
        int w, h;
        uint8_t* bgr;
        int stride;
    };

    HMODULE hDll_ = nullptr;
    void* handle_ = nullptr;  // OcrHandle

    // 函数指针
    void* (*fn_create_)(const char*, const char*, const char*, int, int, int,
                        double, char*, int) = nullptr;
    void (*fn_destroy_)(void*) = nullptr;
    const char* (*fn_error_)(void*) = nullptr;
    int (*fn_run_file_)(void*, const char*, ApiLine*, int, char*, int) = nullptr;
    int (*fn_run_bgr_)(void*, const uint8_t*, int, int, ApiLine*, int, char*,
                       int) = nullptr;
    int (*fn_capture_init_)(void) = nullptr;
    int (*fn_pick_box_)(ApiRect*) = nullptr;
    ApiImage* (*fn_capture_)(const ApiRect*) = nullptr;
    void (*fn_capture_free_)(ApiImage*) = nullptr;
    int (*fn_watch_)(void*, const ApiRect*, const char**, int, int, int,
                     ApiLine*, int, char*, int) = nullptr;
    int (*fn_version_)(char*, int) = nullptr;

    bool loadProcs(std::string& err);
    std::string lastErrFrom(void* h, const char* fallback);
};

// ===================== 实现 =====================

inline bool OCR::loadDll(const std::string& dllName, std::string* errOut) {
    close();
    std::string err;
    hDll_ = LoadLibraryA(dllName.c_str());
    if (!hDll_) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "cannot load %s (需与 exe 同目录或系统路径)", dllName.c_str());
        err = buf;
    } else if (!loadProcs(err)) {
        close();
    }
    if (!err.empty() && errOut) *errOut = err;
    return hDll_ != nullptr;
}

inline bool OCR::captureInit(std::string* errOut) {
    if (!hDll_ && !loadDll("ocr.dll", errOut)) return false;
    if (fn_capture_init_ && fn_capture_init_() == 1) return true;
    if (errOut)
        *errOut =
            "DPI init failed: ocr_capture_init() 需在程序最开头(创建任何窗口"
            "之前)调用, 否则缩放屏下屏幕坐标会偏移";
    return false;
}

inline bool OCR::init(const std::string& det, const std::string& rec,
                         const std::string& dict, int norm, int threads,
                         int detLimit, double textScore,
                         std::string* errOut) {
    if (!hDll_ && !loadDll("ocr.dll", errOut)) return false;
    if (handle_) { fn_destroy_(handle_); handle_ = nullptr; }
    char err[512] = "";
    handle_ = fn_create_(det.empty() ? nullptr : det.c_str(),
                         rec.empty() ? nullptr : rec.c_str(),
                         dict.empty() ? nullptr : dict.c_str(), norm, threads,
                         detLimit, textScore, err, int(sizeof(err)));
    if (!handle_) {
        if (errOut) *errOut = err;
        return false;
    }
    return true;
}

inline int OCR::runFile(const std::string& imagePath, std::vector<Line>& out,
                           std::string* errOut) {
    out.clear();
    if (!isLoaded()) { if (errOut) *errOut = "OCR not init"; return -1; }
    std::vector<ApiLine> buf(256);
    char err[512] = "";
    const int n = fn_run_file_(handle_, imagePath.c_str(), buf.data(),
                               int(buf.size()), err, int(sizeof(err)));
    if (n < 0) {
        if (errOut) *errOut = err[0] ? err : lastErrFrom(handle_, "run_file failed");
        return n;
    }
    out.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        Line l;
        l.text = buf[size_t(i)].text;
        l.score = buf[size_t(i)].score;
        l.cx = buf[size_t(i)].cx; l.cy = buf[size_t(i)].cy;
        l.w = buf[size_t(i)].w; l.h = buf[size_t(i)].h;
        for (int k = 0; k < 4; ++k) {
            l.quad_x[k] = buf[size_t(i)].quad_x[k];
            l.quad_y[k] = buf[size_t(i)].quad_y[k];
        }
        out.push_back(l);
    }
    return n;
}

inline int OCR::runBgr(const uint8_t* bgr, int w, int h,
                          std::vector<Line>& out, std::string* errOut) {
    out.clear();
    if (!isLoaded()) { if (errOut) *errOut = "OCR not init"; return -1; }
    std::vector<ApiLine> buf(256);
    char err[512] = "";
    const int n = fn_run_bgr_(handle_, bgr, w, h, buf.data(), int(buf.size()),
                              err, int(sizeof(err)));
    if (n < 0) {
        if (errOut) *errOut = err[0] ? err : lastErrFrom(handle_, "run_bgr failed");
        return n;
    }
    out.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        Line l;
        l.text = buf[size_t(i)].text;
        l.score = buf[size_t(i)].score;
        l.cx = buf[size_t(i)].cx; l.cy = buf[size_t(i)].cy;
        l.w = buf[size_t(i)].w; l.h = buf[size_t(i)].h;
        for (int k = 0; k < 4; ++k) {
            l.quad_x[k] = buf[size_t(i)].quad_x[k];
            l.quad_y[k] = buf[size_t(i)].quad_y[k];
        }
        out.push_back(l);
    }
    return n;
}

inline bool OCR::pickBox(Rect& out, std::string* errOut) {
    if (!isLoaded()) { if (errOut) *errOut = "OCR not init"; return false; }
    ApiRect r;
    const int rc = fn_pick_box_(&r);
    if (rc != 0) {
        if (errOut) *errOut = "cancelled";
        return false;
    }
    out.x = r.x; out.y = r.y; out.w = r.w; out.h = r.h;
    return true;
}

inline Image OCR::capture(const Rect* rect, std::string* errOut) {
    Image img;
    if (!isLoaded()) { if (errOut) *errOut = "OCR not init"; return img; }
    ApiRect ar;
    ApiRect* p = nullptr;
    if (rect) {
        ar = {rect->x, rect->y, rect->w, rect->h};
        p = &ar;
    }
    ApiImage* c = fn_capture_(p);
    if (!c) {
        if (errOut) *errOut = "capture failed";
        return img;
    }
    img.w = c->w; img.h = c->h; img.stride = c->stride;
    img.bgr.assign(c->bgr, c->bgr + size_t(c->stride) * size_t(c->h));
    fn_capture_free_(c);
    return img;
}

inline int OCR::watch(const Rect* rect,
                         const std::vector<std::string>& keys, int intervalMs,
                         int timeoutMs, std::vector<Line>& hits,
                         std::string* errOut) {
    hits.clear();
    if (!isLoaded()) { if (errOut) *errOut = "OCR not init"; return -1; }
    std::vector<const char*> ks;
    for (const auto& k : keys) ks.push_back(k.c_str());
    if (ks.empty()) { if (errOut) *errOut = "no keys"; return -1; }
    ApiRect ar;
    ApiRect* p = nullptr;
    if (rect) {
        ar = {rect->x, rect->y, rect->w, rect->h};
        p = &ar;
    }
    std::vector<ApiLine> buf(256);
    char err[512] = "";
    const int n = fn_watch_(handle_, p, ks.data(), int(ks.size()), intervalMs,
                            timeoutMs, buf.data(), int(buf.size()), err,
                            int(sizeof(err)));
    if (n < 0) {
        if (errOut) *errOut = err[0] ? err : lastErrFrom(handle_, "watch failed");
        return n;
    }
    if (n == 0) {
        if (errOut) *errOut = "timeout";
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        Line l;
        l.text = buf[size_t(i)].text;
        l.score = buf[size_t(i)].score;
        l.cx = buf[size_t(i)].cx; l.cy = buf[size_t(i)].cy;
        l.w = buf[size_t(i)].w; l.h = buf[size_t(i)].h;
        for (int k = 0; k < 4; ++k) {
            l.quad_x[k] = buf[size_t(i)].quad_x[k];
            l.quad_y[k] = buf[size_t(i)].quad_y[k];
        }
        hits.push_back(l);
    }
    return n;
}

inline void OCR::close() {
    if (handle_) { if (fn_destroy_) fn_destroy_(handle_); handle_ = nullptr; }
    if (hDll_) { FreeLibrary(hDll_); hDll_ = nullptr; }
}

inline std::string OCR::version() {
    if (!hDll_ || !fn_version_) return "";
    char buf[256] = "";
    fn_version_(buf, int(sizeof(buf)));
    return buf;
}

inline std::string OCR::lastErrFrom(void* h, const char* fallback) {
    return (fn_error_ && h && fn_error_(h) && fn_error_(h)[0])
               ? std::string(fn_error_(h))
               : std::string(fallback);
}
inline bool OCR::loadProcs(std::string& err) {
#define LOAD(name, var)                                                       \
    var = reinterpret_cast<decltype(var)>(                                     \
        reinterpret_cast<void*>(GetProcAddress(hDll_, name)));                 \
    if (!var) { err = std::string("missing export: ") + name; return false; }
    LOAD("ocr_create", fn_create_)
    LOAD("ocr_destroy", fn_destroy_)
    LOAD("ocr_error", fn_error_)
    LOAD("ocr_run_file", fn_run_file_)
    LOAD("ocr_run_bgr", fn_run_bgr_)
    LOAD("ocr_capture_init", fn_capture_init_)
    LOAD("ocr_pick_box", fn_pick_box_)
    LOAD("ocr_capture", fn_capture_)
    LOAD("ocr_capture_free", fn_capture_free_)
    LOAD("ocr_watch", fn_watch_)
    LOAD("ocr_version", fn_version_)
#undef LOAD
    return true;
}

}  // namespace ocr
