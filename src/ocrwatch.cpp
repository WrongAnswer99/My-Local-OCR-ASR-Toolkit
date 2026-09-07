// ocrwatch: 截取屏幕指定区域 -> 循环 OCR -> 命中关键字后执行动作。
//
// 用法示例:
//   ocrwatch --rect=100,200,600,120 --key="发货成功,已签收" --interval-ms=800
//           --run="powershell -c Start-Process notepad" --dump=dump.png
//   --key 可重复传多个；--rect 为虚拟屏幕坐标(px)；不带 --rect 则抓整个屏幕。
//   未命中时: 超时(--timeout-sec)或最大帧数(--max-frames)到期则退出。
//   退出码: 0=命中并已执行动作; 2=超时未命中; 3=达到最大帧数未命中; 其他=错误。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "image.hpp"
#include "ocr_core.hpp"
#include "screen.hpp"

static void usage(const char* exe) {
    std::printf(
        "Usage: %s [--rect=x,y,w,h] --key=keyword [--key=...] [options]\n"
        "capture:\n"
        "  --rect=x,y,w,h        抓取虚拟屏幕坐标区域 (默认整个屏幕)\n"
        "  --window=<标题子串>   抓取指定标题的窗口区域(与 --rect 互斥)\n"
        "trigger:\n"
        "  --key=<text>          目标关键字(可多次指定，命中任一即触发)\n"
        "  --ignore-case         ASCII 部分忽略大小写\n"
        "  --run=<command>       命中后执行(经由 cmd.exe)\n"
        "loop:\n"
        "  --interval-ms=<n>     每次识别间隔毫秒 (default 800)\n"
        "  --timeout-sec=<n>     总超时秒数,0=不限 (default 0)\n"
        "  --max-frames=<n>      最多识别帧数,0=不限 (default 0)\n"
        "ocr:\n"
        "  --det=<onnx> --rec=<onnx> --dict=<txt> --score=<num> --threads=<num>\n"
        "  --det-limit=<n>   0=ROI 原尺寸直识别(默认); n>0=短边不足放大到 n\n"
        "  --norm=<0|2>      2=PP-OCRv6 系(默认, v/255); 0=PP-OCRv4 系\n"
        "debug:\n"
        "  --dump=<png>          抓一帧存成图片后退出(用于核对区域)\n"
        "  --image=<file>        改从本地图片循环读取(便于无屏自测/模拟)\n"
        "  --verbose             每帧打印识别出的文本\n"
        "\n"
        "退出码: 0=命中并执行; 2=超时; 3=帧数耗尽; 其他=错误\n",
        exe);
}

static std::string valueOf(int argc, char* argv[], const std::string& key,
                           const std::string& def) {
    const std::string prefix = key + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return def;
}

static std::vector<std::string> allOf(int argc, char* argv[],
                                      const std::string& key) {
    const std::string prefix = key + "=";
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) out.push_back(arg.substr(prefix.size()));
    }
    return out;
}

static bool has(int argc, char* argv[], const std::string& key) {
    for (int i = 1; i < argc; ++i)
        if (key == std::string(argv[i])) return true;
    return false;
}

// ASCII 部分小写(中文等不变)，用于忽略大小写比较
static std::string foldAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

static ScreenRect parseRect(const std::string& s, bool& ok) {
    ScreenRect r;
    ok = false;
    int v[4] = {0};
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4) {
        r.x = v[0];
        r.y = v[1];
        r.w = v[2];
        r.h = v[3];
        ok = r.w > 0 && r.h > 0;
    }
    return r;
}

// EnumWindows 回调需要外部状态，用静态辅助类完成
namespace {
struct WinSearch {
    std::string lowerSub;
    HWND result = nullptr;
    static BOOL CALLBACK proc(HWND hwnd, LPARAM lp) {
        WinSearch* self = reinterpret_cast<WinSearch*>(lp);
        char buf[512] = {0};
        GetWindowTextA(hwnd, buf, 511);
        std::string t(buf);
        if (foldAscii(t).find(self->lowerSub) != std::string::npos) {
            self->result = hwnd;
            return FALSE;  // 找到即停
        }
        return TRUE;
    }
};
}  // namespace

static HWND findWindowByTitleSub(const std::string& sub) {
    WinSearch search;
    search.lowerSub = foldAscii(sub);
    EnumWindows(WinSearch::proc, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

static bool getWindowRectAny(HWND hwnd, ScreenRect& out) {
    RECT rc;
    if (!hwnd || !GetWindowRect(hwnd, &rc)) return false;
    out.x = rc.left;
    out.y = rc.top;
    out.w = rc.right - rc.left;
    out.h = rc.bottom - rc.top;
    return true;
}

// Windows 控制台/快捷方式传入的中文参数常是 ANSI(GBK) 编码，与程序内 UTF-8 文本
// 不兼容。用 CommandLineToArgvW 取宽字符命令行，再统一转为 UTF-8。
static std::string toUtf8(const wchar_t* w) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr,
                                        nullptr);
    std::string s(size_t(len ? len - 1 : 0), '\0');
    if (len > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

int main(int argc, char* argv[]) {
    // 先把命令行参数转为 UTF-8
    int wargc = 0;
    wchar_t** warr = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Ptrs;
    if (warr && wargc > 0) {
        // 分两阶段：先全部生成字符串，再取 data() 指针。
        // 若边 push 边取 data()，vector 扩容会使短字符串(SSO)的指针失效。
        utf8Args.reserve(size_t(wargc));
        for (int i = 0; i < wargc; ++i) utf8Args.emplace_back(toUtf8(warr[i]));
        utf8Ptrs.reserve(size_t(wargc));
        for (std::string& s : utf8Args) utf8Ptrs.push_back(s.data());
        argc = wargc;
        argv = utf8Ptrs.data();
    }
    if (warr) LocalFree(warr);

    if (argc < 2 || has(argc, argv, "--help")) {
        usage(argv[0]);
        return 0;
    }
    SetProcessDPIAware();  // 以物理像素捕获
    SetConsoleOutputCP(CP_UTF8);  // 中文输出在控制台正确显示

    const std::string dumpPath = valueOf(argc, argv, "--dump", "");
    ScreenRect rect{};
    bool haveRect = false;
    const std::string rectStr = valueOf(argc, argv, "--rect", "");
    if (!rectStr.empty()) {
        bool ok = false;
        rect = parseRect(rectStr, ok);
        if (!ok) {
            std::fprintf(stderr, "bad --rect format: %s\n", rectStr.c_str());
            return 1;
        }
        haveRect = true;
    }
    const std::string winTitle = valueOf(argc, argv, "--window", "");
    if (!winTitle.empty()) {
        if (haveRect) {
            std::fprintf(stderr, "--window and --rect are mutually exclusive\n");
            return 1;
        }
        HWND hwnd = findWindowByTitleSub(winTitle);
        if (!getWindowRectAny(hwnd, rect)) {
            std::fprintf(stderr, "window not found: %s\n", winTitle.c_str());
            return 1;
        }
        haveRect = true;
    }
    if (!haveRect) rect = virtualScreen();  // 默认整屏

    // debug: 抓一帧看看区域对不对
    if (!dumpPath.empty()) {
        std::string err;
        Image shot = captureScreen(rect, err);
        if (shot.width <= 0) {
            std::fprintf(stderr, "capture failed: %s\n", err.c_str());
            return 1;
        }
        if (!saveImagePng(dumpPath, shot)) {
            std::fprintf(stderr, "save failed: %s\n", dumpPath.c_str());
            return 1;
        }
        std::printf("saved %dx%d dump to %s\n", shot.width, shot.height,
                    dumpPath.c_str());
        return 0;
    }

    // 关键字与循环参数
    std::vector<std::string> keys = allOf(argc, argv, "--key");
    if (keys.empty()) {
        std::fprintf(stderr, "require at least one --key=<text>\n");
        usage(argv[0]);
        return 1;
    }
    const bool ignoreCase = has(argc, argv, "--ignore-case");
    if (ignoreCase)
        for (std::string& k : keys) k = foldAscii(k);

    const int intervalMs = std::max(0, std::stoi(valueOf(argc, argv, "--interval-ms", "800")));
    const int timeoutSec = std::max(0, std::stoi(valueOf(argc, argv, "--timeout-sec", "0")));
    const int maxFrames = std::max(0, std::stoi(valueOf(argc, argv, "--max-frames", "0")));
    const bool verbose = has(argc, argv, "--verbose");
    const std::string runCmd = valueOf(argc, argv, "--run", "");

    OCRConfig cfg;
    cfg.detModelPath = valueOf(argc, argv, "--det", "models/onnx/v6_det_tiny.onnx");
    cfg.recModelPath = valueOf(argc, argv, "--rec", "models/onnx/v6_rec_tiny.onnx");
    cfg.dictPath = valueOf(argc, argv, "--dict", "models/onnx/v6_tiny_dict.txt");
    cfg.textScore = std::stod(valueOf(argc, argv, "--score", "0.5"));
    cfg.threads = std::stoi(valueOf(argc, argv, "--threads", "0"));
    cfg.detLimitSideLen = std::stoi(valueOf(argc, argv, "--det-limit", "0"));
    cfg.pixelRange = std::stoi(valueOf(argc, argv, "--norm", "2"));

    const std::string imageFile = valueOf(argc, argv, "--image", "");
    Image staticImg;  // --image 模式复用同一帧
    if (!imageFile.empty()) {
        if (!loadImageBgr(imageFile, staticImg)) {
            std::fprintf(stderr, "load image failed: %s\n", imageFile.c_str());
            return 1;
        }
        std::printf("image mode: %s (%dx%d)\n", imageFile.c_str(),
                    staticImg.width, staticImg.height);
    }

    std::string err;
    OCR ocr;
    if (!ocr.load(cfg, err)) {
        std::fprintf(stderr, "[load] %s\n", err.c_str());
        return 1;
    }

    if (imageFile.empty()) {
        std::printf("watching rect=(%d,%d %dx%d), keys=%zu, interval=%dms",
                    rect.x, rect.y, rect.w, rect.h, keys.size(), intervalMs);
        if (timeoutSec > 0) std::printf(", timeout=%ds", timeoutSec);
        std::printf("\n");
    }

    const auto start = std::chrono::steady_clock::now();
    int frame = 0;
    while (maxFrames <= 0 || frame < maxFrames) {
        if (timeoutSec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= timeoutSec) {
                std::fprintf(stderr, "timeout after %d s, not found\n",
                             timeoutSec);
                return 2;
            }
        }
        ++frame;
        const auto t0 = std::chrono::steady_clock::now();

        Image shot;
        if (!imageFile.empty()) {
            shot = staticImg;  // 复用缓存帧
        } else {
            shot = captureScreen(rect, err);
        }
        if (shot.width <= 0) {
            std::fprintf(stderr, "capture failed: %s\n", err.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            continue;
        }
        const std::vector<OCRLine> lines = ocr.runImage(shot, err);
        if (!err.empty()) {
            std::fprintf(stderr, "[run] %s\n", err.c_str());
            return 1;
        }

        // 命中检测：任一文本行包含任一关键字
        std::vector<std::pair<const OCRLine*, std::string>> hits;
        for (const OCRLine& line : lines) {
            std::string text =
                ignoreCase ? foldAscii(line.text) : line.text;
            for (const std::string& key : keys) {
                if (text.find(key) != std::string::npos) {
                    hits.emplace_back(&line, key);
                    break;
                }
            }
        }
        const auto costMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
        if (verbose) {
            std::printf("[frame %d, %lld ms] %zu line(s)\n", frame,
                        costMs, lines.size());
            for (const OCRLine& line : lines)
                std::printf("   [%.3f] %s\n", line.score, line.text.c_str());
        }

        if (!hits.empty()) {
            std::printf("TRIGGER on frame %d (%.3f s):\n", frame,
                        double(costMs) / 1000.0);
            for (const auto& [line, key] : hits)
                std::printf("  key='%s' text='%s' score=%.3f box=(%d,%d)-(%d,%d)\n",
                            key.c_str(), line->text.c_str(), line->score,
                            int(line->box[0].x), int(line->box[0].y),
                            int(line->box[2].x), int(line->box[2].y));
            if (!runCmd.empty()) {
                std::printf("exec: %s\n", runCmd.c_str());
                const int rc = std::system(runCmd.c_str());
                std::printf("command exit code: %d\n", rc);
            }
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }
    std::fprintf(stderr, "max-frames(%d) reached, not found\n", maxFrames);
    return 3;
}
