// ocr_c_api.cpp - 对外 C ABI SDK 实现(封装 OCR + 屏幕/框选 + watch 循环)。
// 编译为 ocr.dll, 与 sdk 头 ocr_api.hpp 配套分发。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ocr_api.hpp"
#include "ocr_core.hpp"
#include "image.hpp"
#include "region_select.hpp"
#include "screen.hpp"

// ---------- 内部工具 ----------
namespace {

struct OcrCtx {
    OCR ocr;
    std::string lastErr;
};

// ocr.dll 所在目录(末尾带 '\\')
std::string dllDir() {
    char buf[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&dllDir), &self);
    DWORD n = GetModuleFileNameA(self, buf, MAX_PATH);
    std::string p(buf, n);
    const size_t slash = p.find_last_of('\\');
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

void setErr(char* err, int cap, const std::string& s) {
    if (err && cap > 0) {
        const size_t n = std::min<size_t>(size_t(cap - 1), s.size());
        if (n) std::memcpy(err, s.data(), n);
        err[n] = '\0';
    }
}

bool fileExists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 模型参数为空 -> 默认 v6 tiny; 优先 dll 同目录 models/onnx, 其次当前工作目录
std::string modelPath(const char* given, const char* defFile) {
    if (given && given[0]) return given;
    const std::string beside = dllDir() + "models\\onnx\\" + defFile;
    if (fileExists(beside)) return beside;
    return std::string("models\\onnx\\") + defFile;
}

OcrCtx* asCtx(OcrHandle h) { return static_cast<OcrCtx*>(h); }

int roundi(double v) { return int(std::floor(v + 0.5)); }

// 引擎结果 -> C 结构; 返回填充到 out 的行数(每行均统计)
int toApiLines(const std::vector<OCRLine>& lines, OcrLine* out, int cap) {
    int written = 0;
    for (const OCRLine& l : lines) {
        OcrLine o;
        std::memset(&o, 0, sizeof(o));
        double ax = 0, ay = 0;
        double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
        for (int i = 0; i < 4; ++i) {
            o.quad_x[i] = l.box[i].x;
            o.quad_y[i] = l.box[i].y;
            ax += l.box[i].x;
            ay += l.box[i].y;
            minx = std::min(minx, l.box[i].x);
            maxx = std::max(maxx, l.box[i].x);
            miny = std::min(miny, l.box[i].y);
            maxy = std::max(maxy, l.box[i].y);
        }
        o.cx = roundi(ax / 4.0);
        o.cy = roundi(ay / 4.0);
        o.w = roundi(maxx - minx);
        o.h = roundi(maxy - miny);
        o.score = l.score;
        std::snprintf(o.text, sizeof(o.text), "%s", l.text.c_str());
        if (written < cap) out[written] = o;
        ++written;
    }
    return written;
}

int fillCfg(OCRConfig& cfg, const char* det, const char* rec, const char* dict,
            int norm, int threads, int detLimit, double textScore,
            std::string& err) {
    cfg.detModelPath = modelPath(det, "v6_det_tiny.onnx");
    cfg.recModelPath = modelPath(rec, "v6_rec_tiny.onnx");
    cfg.dictPath = modelPath(dict, "v6_tiny_dict.txt");
    cfg.pixelRange = norm;   // 2=v6 默认; 0=v4
    cfg.threads = threads;
    cfg.detLimitSideLen = detLimit;
    cfg.textScore = textScore > 0 ? textScore : 0.5;
    (void)err;
    return 0;
}

// 抓一帧屏幕区域(rect 为空则全屏)并 OCR
bool grabAndOcr(OcrCtx* ctx, const OcrRect* rect,
                std::vector<OCRLine>& out, std::string& err) {
    ScreenRect r;
    if (rect) {
        r.x = rect->x; r.y = rect->y; r.w = rect->w; r.h = rect->h;
    } else {
        r = virtualScreen();
    }
    if (r.w <= 0 || r.h <= 0) { err = "invalid rect"; return false; }
    Image img = captureScreen(r, err);
    if (img.width <= 0 || img.height <= 0) return false;  // err 已填
    out = ctx->ocr.runImage(img, err);
    return err.empty();
}

bool matchKeys(const std::vector<OCRLine>& lines, const char** keys,
               int keyCount, std::vector<OCRLine>& hits) {
    hits.clear();
    for (const OCRLine& l : lines)
        for (int k = 0; k < keyCount; ++k)
            if (keys[k] && keys[k][0] && l.text.find(keys[k]) != std::string::npos) {
                hits.push_back(l);
                break;
            }
    return !hits.empty();
}

}  // namespace

// ---------- 1) OCR 引擎 ----------
OCR_API OcrHandle ocr_create(
    const char* detModel, const char* recModel, const char* dict,
    int norm, int threads, int detLimit, double textScore,
    char* err, int errCap) {
    OcrCreateOptions options;
    ocr_default_options(&options);
    options.det_model = detModel;
    options.rec_model = recModel;
    options.dict = dict;
    options.norm = norm;
    options.threads = threads;
    options.det_limit = detLimit;
    options.text_score = textScore;
    return ocr_create_ex(&options, err, errCap);
}

OCR_API void ocr_default_options(OcrCreateOptions* options) {
    if (!options) return;
    *options = {};
    options->struct_size = sizeof(OcrCreateOptions);
    options->device = OCR_DEVICE_CPU;
    options->norm = 2;
    options->text_score = 0.5;
}

OCR_API OcrHandle ocr_create_ex(const OcrCreateOptions* options, char* err, int errCap) {
    setErr(err, errCap, "");
    if (!options || options->struct_size < sizeof(OcrCreateOptions)) {
        setErr(err, errCap, "invalid OcrCreateOptions struct_size; call ocr_default_options first");
        return nullptr;
    }
    if (options->device != OCR_DEVICE_CPU && options->device != OCR_DEVICE_CUDA) {
        setErr(err, errCap, "invalid OCR device: expected OCR_DEVICE_CPU or OCR_DEVICE_CUDA");
        return nullptr;
    }
    if (options->gpu_device_id < 0) {
        setErr(err, errCap, "GPU device ID must be >= 0");
        return nullptr;
    }
    try {
        auto ctx = std::make_unique<OcrCtx>();
        std::string e;
        OCRConfig cfg;
        fillCfg(cfg, options->det_model, options->rec_model, options->dict,
                options->norm, options->threads, options->det_limit, options->text_score, e);
        cfg.device = static_cast<OCRDevice>(options->device);
        cfg.gpuDeviceId = options->gpu_device_id;
        if (!ctx->ocr.load(cfg, e)) {
            setErr(err, errCap, e);
            return nullptr;
        }
        return ctx.release();
    } catch (const std::exception& e) {
        setErr(err, errCap, e.what());
    } catch (...) {
        setErr(err, errCap, "unexpected OCR initialization failure");
    }
    return nullptr;
}

OCR_API void ocr_destroy(OcrHandle h) {
    if (h) delete asCtx(h);
}

OCR_API const char* ocr_error(OcrHandle h) {
    return h ? asCtx(h)->lastErr.c_str() : "";
}

OCR_API int ocr_run_file(OcrHandle h, const char* imagePath,
                         OcrLine* out, int cap, char* err, int errCap) {
    if (!h) { setErr(err, errCap, "null handle"); return -1; }
    OcrCtx* ctx = asCtx(h);
    std::string e;
    const std::vector<OCRLine> lines = ctx->ocr.runPath(imagePath, e);
    if (!e.empty()) {
        ctx->lastErr = e;
        setErr(err, errCap, e);
        return -1;
    }
    return toApiLines(lines, out, cap);
}

OCR_API int ocr_run_bgr(OcrHandle h, const uint8_t* bgr, int width, int height,
                        OcrLine* out, int cap, char* err, int errCap) {
    if (!h) { setErr(err, errCap, "null handle"); return -1; }
    if (!bgr || width <= 0 || height <= 0) {
        setErr(err, errCap, "invalid bgr/width/height");
        return -1;
    }
    OcrCtx* ctx = asCtx(h);
    Image img(width, height);
    std::memcpy(img.data.data(), bgr, img.data.size());
    std::string e;
    const std::vector<OCRLine> lines = ctx->ocr.runImage(img, e);
    if (!e.empty()) {
        ctx->lastErr = e;
        setErr(err, errCap, e);
        return -1;
    }
    return toApiLines(lines, out, cap);
}

// ---------- 2) 屏幕/框选 ----------
// 程序启动最早调用: 使进程按物理像素工作(SetProcessDPIAware)。
// 返回 1=已物理像素(成功或进程本就已感知); 0=仍处于虚拟化(调用太晚, 需提前)。
OCR_API int ocr_capture_init(void) {
    typedef BOOL(WINAPI* FnSetLegacy)();
    const HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        FnSetLegacy f2 =
            (FnSetLegacy)(void*)GetProcAddress(u, "SetProcessDPIAware");
        if (f2 && f2()) return 1;
    }
    // 设置被拒: 若进程已是某种感知模式(系统/每显示器)同样可用
    typedef HRESULT(WINAPI* FnGetA)(int*);
    const HMODULE sh = GetModuleHandleW(L"shcore.dll");
    int mode = 0;
    if (sh) {
        FnGetA f = (FnGetA)(void*)GetProcAddress(sh, "GetProcessDpiAwareness");
        if (f && SUCCEEDED(f(&mode)) && mode != 0) return 1;
    }
    return 0;
}

OCR_API int ocr_pick_box(OcrRect* out) {
    if (!out) return -1;
    ScreenRect r;
    if (!pickScreenRect(r, 0)) return 1;  // 取消/Esc
    out->x = r.x; out->y = r.y; out->w = r.w; out->h = r.h;
    return 0;
}

OCR_API OcrImage* ocr_capture(const OcrRect* rect) {
    ScreenRect r;
    if (rect) {
        r.x = rect->x; r.y = rect->y; r.w = rect->w; r.h = rect->h;
    } else {
        r = virtualScreen();
    }
    std::string err;
    Image img = captureScreen(r, err);
    if (img.width <= 0 || img.height <= 0) return nullptr;
    auto* out = new (std::nothrow) OcrImage;
    if (!out) return nullptr;
    out->w = img.width;
    out->h = img.height;
    out->stride = img.width * 3;
    out->bgr = new (std::nothrow) uint8_t[img.data.size()];
    if (!out->bgr) {
        delete out;
        return nullptr;
    }
    std::memcpy(out->bgr, img.data.data(), img.data.size());
    return out;
}

OCR_API void ocr_capture_free(OcrImage* img) {
    if (!img) return;
    delete[] img->bgr;
    delete img;
}

// ---------- 3) 持续监听 ----------
namespace {

// watch 公共循环; 命中且 !callback 时把 hits 转 API 行返回行数;
// 回调模式返回码单独处理。
int watchLoop(OcrCtx* ctx, const OcrRect* rect, const char** keys,
              int keyCount, int intervalMs, int timeoutMs,
              OcrLine* out, int cap, OcrWatchFn onHit, void* user,
              bool isCallback, char* err, int errCap) {
    if (!ctx) { setErr(err, errCap, "null handle"); return -1; }
    if (keyCount <= 0 || !keys) { setErr(err, errCap, "no keys"); return -1; }
    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = timeoutMs > 0
        ? begin + std::chrono::milliseconds(timeoutMs)
        : std::chrono::steady_clock::time_point::max();

    for (;;) {
        std::vector<OCRLine> lines, hits;
        std::string e;
        if (!grabAndOcr(ctx, rect, lines, e)) {
            ctx->lastErr = e;
            setErr(err, errCap, e);
            return -1;
        }
        matchKeys(lines, keys, keyCount, hits);
        if (!hits.empty()) {
            if (isCallback && onHit) {
                // 转为 API 行喂回调
                std::vector<OcrLine> api;
                api.reserve(hits.size());
                for (const OCRLine& l : hits) {
                    OcrLine o;
                    OcrLine one = {};
                    std::vector<OCRLine> tmp(1, l);
                    toApiLines(tmp, &one, 1);
                    o = one;
                    api.push_back(o);
                }
                onHit(api.empty() ? nullptr : api.data(), int(api.size()), user);
                return 0;
            }
            std::vector<OcrLine> api(hits.size());
            toApiLines(hits, api.data(), int(api.size()));
            int n = int(api.size());
            if (out && cap > 0)
                for (int i = 0; i < n && i < cap; ++i) out[i] = api[size_t(i)];
            return n;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            setErr(err, errCap, "timeout");
            return isCallback ? 2 : 0;
        }
        if (intervalMs > 0) std::this_thread::sleep_for(
            std::chrono::milliseconds(intervalMs));
    }
}

}  // namespace

OCR_API int ocr_watch_callback(
    OcrHandle h, const OcrRect* rect, const char** keys, int keyCount,
    int intervalMs, int timeoutMs, OcrWatchFn onHit, void* user,
    char* err, int errCap) {
    OcrCtx* ctx = asCtx(h);
    OcrCtx* tmp = nullptr;
    if (!ctx) {  // h==NULL: 默认模型临时引擎
        tmp = new (std::nothrow) OcrCtx;
        std::string e;
        OCRConfig cfg;
        fillCfg(cfg, nullptr, nullptr, nullptr, 2, 0, 0, 0.5, e);
        if (!tmp || !tmp->ocr.load(cfg, e)) {
            delete tmp;
            setErr(err, errCap, e);
            return -1;
        }
        ctx = tmp;
    }
    const int rc = watchLoop(ctx, rect, keys, keyCount, intervalMs, timeoutMs,
                             nullptr, 0, onHit, user, true, err, errCap);
    delete tmp;
    return rc;
}

OCR_API int ocr_watch(
    OcrHandle h, const OcrRect* rect, const char** keys, int keyCount,
    int intervalMs, int timeoutMs, OcrLine* out, int cap,
    char* err, int errCap) {
    OcrCtx* ctx = asCtx(h);
    OcrCtx* tmp = nullptr;
    if (!ctx) {
        tmp = new (std::nothrow) OcrCtx;
        std::string e;
        OCRConfig cfg;
        fillCfg(cfg, nullptr, nullptr, nullptr, 2, 0, 0, 0.5, e);
        if (!tmp || !tmp->ocr.load(cfg, e)) {
            delete tmp;
            setErr(err, errCap, e);
            return -1;
        }
        ctx = tmp;
    }
    const int rc = watchLoop(ctx, rect, keys, keyCount, intervalMs, timeoutMs,
                             out, cap, nullptr, nullptr, false, err, errCap);
    delete tmp;
    return rc;
}

// ---------- 4) 其他 ----------
OCR_API int ocr_version(char* buf, int cap) {
    const char* v = "ppocr-onnx sdk 0.2 (PP-OCRv6 tiny default, onnxruntime CPU/CUDA)";
    if (buf && cap > 0) std::snprintf(buf, cap, "%s", v);
    return int(std::strlen(v));
}
