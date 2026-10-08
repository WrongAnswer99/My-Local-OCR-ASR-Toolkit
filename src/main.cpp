// ppocr_onnx: 基于 PP-OCRv4(ONNX) + ONNX Runtime 的命令行 OCR 工具。
// 用法: ppocr_onnx <image> [--det=模型] [--rec=模型] [--dict=字典] [--viz=输出png]
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "image.hpp"
#include "ocr_core.hpp"

static void usage(const char* exe) {
    std::printf(
        "Usage: %s <image> [options]\n"
        "options:\n"
        "  --det=<onnx>     文本检测模型  (default models/onnx/v6_det_tiny.onnx)\n"
        "  --rec=<onnx>     文本识别模型  (default models/onnx/v6_rec_tiny.onnx)\n"
        "  --dict=<txt>     识别字典      (default models/onnx/v6_tiny_dict.txt)\n"
        "  --score=<num>    置信度阈值    (default 0.5)\n"
        "  --threads=<num>  onnxruntime 线程数 (default 0=自动)\n"
        "  --device=<cpu|cuda>  推理设备 (default cpu; gpu=cuda)\n"
        "  --gpu-device=<n>     CUDA 显卡编号 (default 0)\n"
        "  --det-limit=<n>  检测短边策略: 0=原尺寸直识别(默认); n>0=短边不足放大到 n\n"
        "  --norm=<0|2>     像素取值: 2=PP-OCRv6 系(默认, v/255); 0=PP-OCRv4 系\n"
        "  --viz=<png>      在原图上画框并保存\n",
        exe);
}

static std::string valueOf(int argc, char* argv[], const std::string& key,
                           const std::string& def) {
    const std::string prefix = key + "=";
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return def;
}

static bool hasFlag(int argc, char* argv[], const std::string& key) {
    for (int i = 2; i < argc; ++i)
        if (key == std::string(argv[i])) return true;
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    const std::string imagePath = argv[1];
    if (imagePath == "--help" || imagePath == "-h") {
        usage(argv[0]);
        return 0;
    }

    OCRConfig cfg;
    std::string deviceErr;
    if (!parseOcrDeviceArgs(argc, argv, cfg.device, cfg.gpuDeviceId, deviceErr)) {
        std::fprintf(stderr, "[args] %s\n", deviceErr.c_str());
        return 1;
    }
    cfg.detModelPath = valueOf(argc, argv, "--det",
                               "models/onnx/v6_det_tiny.onnx");
    cfg.recModelPath = valueOf(argc, argv, "--rec",
                               "models/onnx/v6_rec_tiny.onnx");
    cfg.dictPath = valueOf(argc, argv, "--dict",
                           "models/onnx/v6_tiny_dict.txt");
    cfg.textScore = std::stod(valueOf(argc, argv, "--score", "0.5"));
    cfg.threads = std::stoi(valueOf(argc, argv, "--threads", "0"));
    cfg.detLimitSideLen = std::stoi(valueOf(argc, argv, "--det-limit", "0"));
    cfg.pixelRange = std::stoi(valueOf(argc, argv, "--norm", "2"));

    std::string err;
    OCR ocr;
    const auto t0 = std::chrono::steady_clock::now();
    if (!ocr.load(cfg, err)) {
        std::fprintf(stderr, "[load] %s\n", err.c_str());
        return 2;
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("backend: %s, gpu-device: %d\n", ocrDeviceName(cfg.device), cfg.gpuDeviceId);

    const std::vector<OCRLine> lines = ocr.runPath(imagePath, err);
    const auto t2 = std::chrono::steady_clock::now();
    if (!err.empty()) {
        std::fprintf(stderr, "[run] %s\n", err.c_str());
        return 3;
    }
    const auto ms = [](const auto& a, const auto& b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    std::printf("timing: model-init %lld ms, inference %lld ms\n", ms(t0, t1),
                ms(t1, t2));

    std::printf("detected %zu text line(s)\n", lines.size());
    for (const OCRLine& line : lines) {
        std::printf("[%.4f] %s\n", line.score, line.text.c_str());
        std::printf("    box: (%d,%d) (%d,%d) (%d,%d) (%d,%d)\n",
                    int(line.box[0].x), int(line.box[0].y), int(line.box[1].x),
                    int(line.box[1].y), int(line.box[2].x), int(line.box[2].y),
                    int(line.box[3].x), int(line.box[3].y));
    }

    const std::string vizPath = valueOf(argc, argv, "--viz", "");
    if (!vizPath.empty()) {
        Image img;
        if (loadImageBgr(imagePath, img)) {
            const uint8_t color[3] = {0, 0, 255};  // BGR 红
            for (const OCRLine& line : lines) {
                double q[4][2];
                for (int i = 0; i < 4; ++i) {
                    q[i][0] = line.box[i].x;
                    q[i][1] = line.box[i].y;
                }
                drawQuadBgr(img, q, color, 2);
            }
            if (!saveImagePng(vizPath, img))
                std::fprintf(stderr, "[viz] save failed: %s\n", vizPath.c_str());
            else
                std::printf("visualization saved to %s\n", vizPath.c_str());
        }
    }
    return 0;
}
