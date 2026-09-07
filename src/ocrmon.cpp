// ocrmon: 测试/演示程序 —— 框选屏幕区域后持续 OCR 并实时打印结果。
// 启动后出现全屏框选遮罩; 拖拽选择一个区域;
// 松开后进入循环: 抓取该区域 -> PP-OCR 识别 -> 终端持续打印识别出的文本行。
//
// 用法: ocrmon [--rect=x,y,w,h] [--interval-ms=N] [--once] [--det=onnx]
//              [--rec=onnx] [--dict=txt] [--score=num] [--threads=N]
//   不带 --rect 时先弹出全屏框选。
//   调试: 设置环境变量 OCR_TRACE=1 后, 每帧截图会保存到 images/ocr_trace/,
//         崩溃时最后一帧即为复现样本。
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
#include "region_select.hpp"
#include "screen.hpp"

static std::string toUtf8(const wchar_t* w) {
    const int len =
        WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len ? len - 1 : 0), '\0');
    if (len > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

int main(int argc, char* argv[]) {
    // 命令行统一转 UTF-8, 兼容中文参数
    int wargc = 0;
    wchar_t** warr = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Ptrs;
    if (warr && wargc > 0) {
        utf8Args.reserve(size_t(wargc));
        for (int i = 0; i < wargc; ++i) utf8Args.emplace_back(toUtf8(warr[i]));
        utf8Ptrs.reserve(size_t(wargc));
        for (std::string& s : utf8Args) utf8Ptrs.push_back(s.data());
        argc = wargc;
        argv = utf8Ptrs.data();
    }
    if (warr) LocalFree(warr);

    const auto valueOf = [&](const char* key, const std::string& def) {
        const std::string prefix = std::string(key) + "=";
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
        }
        return def;
    };

    SetProcessDPIAware();
    SetConsoleOutputCP(CP_UTF8);

    const int intervalMs =
        std::max(0, std::stoi(valueOf("--interval-ms", "300")));
    const bool once = [&] {
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--once") return true;
        return false;
    }();

    OCRConfig cfg;
    cfg.detModelPath = valueOf("--det", "models/onnx/v6_det_tiny.onnx");
    cfg.recModelPath = valueOf("--rec", "models/onnx/v6_rec_tiny.onnx");
    cfg.dictPath = valueOf("--dict", "models/onnx/v6_tiny_dict.txt");
    cfg.textScore = std::stod(valueOf("--score", "0.5"));
    cfg.threads = std::stoi(valueOf("--threads", "0"));
    cfg.detLimitSideLen = std::stoi(valueOf("--det-limit", "0"));
    cfg.pixelRange = std::stoi(valueOf("--norm", "2"));

    std::printf("=== 拖拽框选需要监听的屏幕区域 ==="
                " (Esc/右键取消)\n");
    ScreenRect rect;
    const std::string rectStr = valueOf("--rect", "");
    bool haveRect = false;
    if (!rectStr.empty()) {
        if (std::sscanf(rectStr.c_str(), "%d,%d,%d,%d", &rect.x, &rect.y,
                        &rect.w, &rect.h) == 4 &&
            rect.w > 0 && rect.h > 0)
            haveRect = true;
    }
    if (!haveRect) {
        if (!pickScreenRect(rect)) {
            std::printf("cancelled\n");
            return 1;
        }
    }
    std::printf("selected region: %d,%d %dx%d\n", rect.x, rect.y, rect.w,
                rect.h);

    std::string err;
    OCR ocr;
    if (!ocr.load(cfg, err)) {
        std::fprintf(stderr, "[load] %s\n", err.c_str());
        return 2;
    }
    const bool trace = std::getenv("OCR_TRACE") != nullptr;
    if (trace) {
        CreateDirectoryA("images", nullptr);
        CreateDirectoryA("images\\ocr_trace", nullptr);
        std::printf("[trace] frames will be saved to images/ocr_trace/\n");
    }
    std::printf("model loaded. monitoring (interval %d ms, Ctrl+C 结束)...\n",
                intervalMs);

    const auto start = std::chrono::steady_clock::now();
    int frame = 0;
    for (;;) {
        ++frame;
        const auto t0 = std::chrono::steady_clock::now();

        Image shot = captureScreen(rect, err);
        if (shot.width <= 0) {
            std::fprintf(stderr, "capture failed: %s\n", err.c_str());
            break;
        }
        if (trace) {
            char name[128];
            std::snprintf(name, sizeof(name),
                          "images/ocr_trace/frame_%05d.png", frame);
            saveImagePng(name, shot);
        }
        const std::vector<OCRLine> lines = ocr.runImage(shot, err);
        if (!err.empty()) {
            std::fprintf(stderr, "[run] %s\n", err.c_str());
            break;
        }
        const auto costMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

        std::printf("\n----- frame %d  (%lld ms, %zu lines) -----\n", frame,
                    costMs, lines.size());
        for (const OCRLine& line : lines)
            std::printf("  [%.3f] %s\n", line.score, line.text.c_str());
        std::fflush(stdout);

        if (once) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }
    const auto total = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    std::printf("\nmonitored %d frame(s), %.0f s\n", frame, double(total));
    return 0;
}
