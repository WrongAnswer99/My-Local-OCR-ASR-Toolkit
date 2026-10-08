// api_demo.cpp - ocr.hpp(SDK 封装) 用法演示。
// 不链接导入库: 运行时自动 LoadLibrary ocr.dll。
//
//   api_demo file <图片>                    识别一张图片
//   api_demo pick                         全屏拖框选矩形, 打印 rect
//   api_demo watch <关键字> [--rect=x,y,w,h]
//          [--timeout=毫秒] [--interval=毫秒]   监听屏幕, 命中打印整行
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ocr.hpp"
#include "ocr_device.hpp"

static int argPos(int argc, char* argv[], const char* key) {
    const std::string k = key;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind(k + "=", 0) == 0) return i;
    }
    return -1;
}

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::printf("用法:\n"
                    "  %s file <图片>\n"
                    "  %s pick\n"
                    "  %s watch <关键字> [--rect=x,y,w,h] [--timeout=毫秒] [--interval=毫秒]\n"
                    "  OCR options: --device=<cpu|cuda> --gpu-device=<n> (default cpu, GPU 0)\n",
                    argv[0], argv[0], argv[0]);
        return argc < 2 ? 1 : 0;
    }
    const std::string mode = argv[1];

    ocr::OCR e;
    std::string err;
    OCRDevice device = OCRDevice::CPU;
    int deviceId = 0;
    if (!parseOcrDeviceArgs(argc, argv, device, deviceId, err)) {
        std::fprintf(stderr, "[args] %s\n", err.c_str());
        return 1;
    }
    if (!e.loadDll("ocr.dll", &err)) {
        std::fprintf(stderr, "load dll: %s\n", err.c_str());
        return 2;
    }
    if (!e.captureInit(&err)) {
        std::fprintf(stderr, "capture init: %s\n", err.c_str());
        return 2;
    }
    ocr::InitOptions options;
    options.threads = 8;
    options.device = static_cast<ocr::Device>(device);
    options.gpuDeviceId = deviceId;
    if (!e.init(options, &err)) {
        std::fprintf(stderr, "init: %s\n", err.c_str());
        return 2;
    }
    std::printf("sdk version: %s\n", e.version().c_str());
    std::printf("backend: %s, gpu-device: %d\n", ocrDeviceName(device), deviceId);

    if (mode == "file" && argc >= 3) {
        std::vector<ocr::Line> lines;
        const int n = e.runFile(argv[2], lines, &err);
        if (n < 0) {
            std::fprintf(stderr, "run: %s\n", err.c_str());
            return 3;
        }
        std::printf("detected %d line(s)\n", n);
        for (const auto& l : lines)
            std::printf("[%.3f] %s   center=(%d,%d) wh=%dx%d\n", l.score,
                        l.text.c_str(), l.cx, l.cy, l.w, l.h);
    } else if (mode == "pick") {
        ocr::Rect r;
        if (!e.pickBox(r, &err)) {
            std::printf("picked: cancelled (%s)\n", err.c_str());
            return 1;
        }
        std::printf("picked: --rect=%d,%d,%d,%d\n", r.x, r.y, r.w, r.h);
    } else if (mode == "watch" && argc >= 3) {
        ocr::Rect rect;
        bool hasRect = false;
        const int ri = argPos(argc, argv, "--rect");
        if (ri > 0 && std::sscanf(argv[ri] + 7, "%d,%d,%d,%d", &rect.x, &rect.y,
                                  &rect.w, &rect.h) == 4)
            hasRect = true;
        int timeout = 0, interval = 800;
        const int ti = argPos(argc, argv, "--timeout");
        if (ti > 0) timeout = std::atoi(argv[ti] + 10);  // "--timeout=" 共10字符
        const int ii = argPos(argc, argv, "--interval");
        if (ii > 0) interval = std::atoi(argv[ii] + 11);  // "--interval=" 共11字符

        std::vector<std::string> keys = {argv[2]};
        std::vector<ocr::Line> hits;
        const int n = e.watch(hasRect ? &rect : nullptr, keys, interval,
                              timeout, hits, &err);
        if (n < 0) {
            std::fprintf(stderr, "watch: %s\n", err.c_str());
            return 3;
        }
        if (n == 0) {
            std::printf("watch: timeout, key \"%s\" not seen\n", argv[2]);
            return 0;
        }
        std::printf("watch: hit %d line(s), key \"%s\"\n", n, argv[2]);
        for (const auto& l : hits)
            std::printf("  [%.3f] %s   center=(%d,%d)\n", l.score,
                        l.text.c_str(), l.cx, l.cy);
    } else {
        std::printf("unknown mode\n");
        return 1;
    }
    return 0;
}
