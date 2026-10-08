// Standalone SDK consumer: g++ -std=c++17 gpu_example.cpp -I..
#include "ocr.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: gpu_example <image>\n");
        return 1;
    }
    ocr::OCR engine;
    ocr::InitOptions options;
    options.device = ocr::Device::CUDA;
    options.gpuDeviceId = 0;
    std::string err;
    if (!engine.init(options, &err)) {
        std::fprintf(stderr, "CUDA init: %s\n", err.c_str());
        return 2;
    }
    std::vector<ocr::Line> lines;
    if (engine.runFile(argv[1], lines, &err) < 0) {
        std::fprintf(stderr, "OCR: %s\n", err.c_str());
        return 3;
    }
    for (const auto& line : lines)
        std::printf("[%.3f] %s\n", line.score, line.text.c_str());
    return 0;
}
