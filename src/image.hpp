// 轻量图像库：仅实现 OCR 所需的少量图像操作，避免引入第三方图像库(ABI/依赖问题)。
// 像素布局统一为 BGR 连续存储(h*w*3)，行优先。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Image {
    int width = 0;   // 列数
    int height = 0;  // 行数
    std::vector<uint8_t> data;  // width*height*3, BGR

    Image() = default;
    Image(int w, int h) : width(w), height(h), data(size_t(w) * size_t(h) * 3u, 0) {}

    uint8_t* at(int y, int x) { return data.data() + (size_t(y) * width + x) * 3; }
    const uint8_t* at(int y, int x) const { return data.data() + (size_t(y) * width + x) * 3; }
};

// 解码 jpg/png/bmp 等为 BGR；失败返回 false
bool loadImageBgr(const std::string& path, Image& out);
// 保存为 png(RGB)，成功返回 true
bool saveImagePng(const std::string& path, const Image& img);

// 双线性缩放(与 cv2.resize 默认 INTER_LINEAR 一致)
Image resizeBilinear(const Image& src, int newWidth, int newHeight);

// 逆时针旋转 90 度(np.rot90 语义，用于竖排文本)
Image rotate90CounterClockwise(const Image& src);

// 透视矫正：把 src 中四点四边形(顺序 tl,tr,br,bl)矫正为 outW x outH 的矩形图像，
// 边沿用复制像素填充(BORDER_REPLICATE)，双线性采样
Image warpPerspectiveBgr(const Image& src, const double quad[4][2],
                         int outWidth, int outHeight);

// 单通道 float 图(用于绘制时忽略)不做处理；仅 RGB/BGR 图可以画框
void drawQuadBgr(Image& img, const double quad[4][2], const uint8_t color[3],
                 int thickness = 2);
