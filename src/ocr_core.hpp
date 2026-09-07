// 轻量 OCR 引擎：PP-OCRv4(det+rec) + ONNX Runtime。
// 图像处理与后处理算法移植自 RapidOCR(python)，与 PaddleOCR 推理语义保持一致。
#pragma once

#include <string>
#include <vector>

#include "image.hpp"

struct OCRBoxPt {
    double x = 0.0;
    double y = 0.0;
};

// 一条识别结果；box 四个点顺序：左上、右上、右下、左下(映射回原图坐标)
struct OCRLine {
    OCRBoxPt box[4];
    std::string text;
    double score = 0.0;
};

struct OCRConfig {
    std::string detModelPath;  // PP-OCRv4 det onnx
    std::string recModelPath;  // PP-OCRv4 rec onnx
    std::string dictPath;      // ppocr_keys_v1.txt
    std::string clsModelPath;  // 可选：文本行方向分类(0/180)，传空表示不使用
    bool useCls = false;
    int threads = 0;  // 0 = onnxruntime 默认
    double textScore = 0.5;  // 低于该置信度的结果丢弃
    int detLimitSideLen = 0;
    // 检测输入短边策略: 0=按原尺寸直接识别(推荐 ROI 场景);
    // >0 时若短边小于该值则放大到该值(如 736=RapidOCR 全图默认)
    int pixelRange = 2;
    // 像素喂给模型的取值模式: 0=(v/127.5-1) PP-OCRv4 系; 2=(v/255) PP-OCRv6 系(默认); 1=原值 0..255
};

class OCR {
public:
    OCR();
    ~OCR();
    OCR(const OCR&) = delete;
    OCR& operator=(const OCR&) = delete;

    bool load(const OCRConfig& cfg, std::string& err);
    // 识别本地图片路径
    std::vector<OCRLine> runPath(const std::string& imagePath, std::string& err);
    // 识别内存图像(BGR)
    std::vector<OCRLine> runImage(const Image& img, std::string& err);

    int characterCount() const { return int(characters_.size()); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    void destroyImpl();
    std::vector<std::string> characters_;  // index0=blank,末尾=空格
};
