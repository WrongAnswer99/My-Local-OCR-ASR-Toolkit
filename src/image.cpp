#include "image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>

bool loadImageBgr(const std::string& path, Image& out) {
    int w = 0, h = 0, comp = 0;
    unsigned char* raw = stbi_load(path.c_str(), &w, &h, &comp, 3);  // 强制 3 通道 RGB
    if (!raw) return false;
    out = Image(w, h);
    const unsigned char* p = raw;
    uint8_t* dst = out.data.data();
    const size_t n = size_t(w) * h;
    for (size_t i = 0; i < n; ++i, p += 3, dst += 3) {
        dst[0] = p[2];  // R<->B 交换为 BGR
        dst[1] = p[1];
        dst[2] = p[0];
    }
    stbi_image_free(raw);
    return true;
}

bool saveImagePng(const std::string& path, const Image& img) {
    if (img.width <= 0 || img.height <= 0) return false;
    std::vector<uint8_t> rgb(img.data.size());
    const uint8_t* p = img.data.data();
    uint8_t* dst = rgb.data();
    const size_t n = size_t(img.width) * img.height;
    for (size_t i = 0; i < n; ++i, p += 3, dst += 3) {
        dst[0] = p[2];
        dst[1] = p[1];
        dst[2] = p[0];
    }
    return stbi_write_png(path.c_str(), img.width, img.height, 3, rgb.data(),
                          img.width * 3) != 0;
}

Image resizeBilinear(const Image& src, int newWidth, int newHeight) {
    Image dst(newWidth, newHeight);
    if (src.width <= 0 || src.height <= 0 || newWidth <= 0 || newHeight <= 0)
        return dst;
    const float scaleX = float(src.width) / float(newWidth);
    const float scaleY = float(src.height) / float(newHeight);
    // 与 OpenCV 约定一致：采样中心平移 0.5 个像素后归一化
    for (int y = 0; y < newHeight; ++y) {
        const float fy = (y + 0.5f) * scaleY - 0.5f;
        int y0 = (int)std::floor(fy);
        const float wy = fy - y0;
        if (y0 < 0) { y0 = 0; }
        const int y1 = std::min(y0 + 1, src.height - 1);
        if (y0 > src.height - 1) y0 = src.height - 1;
        for (int x = 0; x < newWidth; ++x) {
            const float fx = (x + 0.5f) * scaleX - 0.5f;
            int x0 = (int)std::floor(fx);
            const float wx = fx - x0;
            if (x0 < 0) { x0 = 0; }
            const int x1 = std::min(x0 + 1, src.width - 1);
            if (x0 > src.width - 1) x0 = src.width - 1;
            const uint8_t* p00 = src.at(y0, x0);
            const uint8_t* p10 = src.at(y0, x1);
            const uint8_t* p01 = src.at(y1, x0);
            const uint8_t* p11 = src.at(y1, x1);
            uint8_t* d = dst.at(y, x);
            for (int c = 0; c < 3; ++c) {
                const float top = p00[c] * (1.f - wx) + p10[c] * wx;
                const float bottom = p01[c] * (1.f - wx) + p11[c] * wx;
                d[c] = (uint8_t)std::lround(top * (1.f - wy) + bottom * wy);
            }
        }
    }
    return dst;
}

Image rotate90CounterClockwise(const Image& src) {
    // np.rot90: out[x][H-1-y] = in[y][x] (src 尺寸 HxW -> dst WxH)
    Image dst(src.height, src.width);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            uint8_t* d = dst.at(x, src.height - 1 - y);
            const uint8_t* s = src.at(y, x);
            std::memcpy(d, s, 3);
        }
    }
    return dst;
}

// 解 8 元线性方程组(高斯消元)，求透视矩阵 H(h33=1)，使 dst 角点映射到 src 对应点
static bool solveHomography(const double src[4][2], const double dst[4][2],
                            double h[3][3]) {
    double a[8][9] = {{0}};
    for (int i = 0; i < 4; ++i) {
        const double x = dst[i][0], y = dst[i][1];
        const double u = src[i][0], v = src[i][1];
        a[i * 2][0] = x; a[i * 2][1] = y; a[i * 2][2] = 1.0;
        a[i * 2][3] = 0.0; a[i * 2][4] = 0.0; a[i * 2][5] = 0.0;
        a[i * 2][6] = -x * u; a[i * 2][7] = -y * u; a[i * 2][8] = u;
        a[i * 2 + 1][0] = 0.0; a[i * 2 + 1][1] = 0.0; a[i * 2 + 1][2] = 0.0;
        a[i * 2 + 1][3] = x; a[i * 2 + 1][4] = y; a[i * 2 + 1][5] = 1.0;
        a[i * 2 + 1][6] = -x * v; a[i * 2 + 1][7] = -y * v; a[i * 2 + 1][8] = v;
    }
    for (int col = 0; col < 8; ++col) {
        int piv = col;
        for (int r = col + 1; r < 8; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (std::fabs(a[piv][col]) < 1e-12) return false;
        for (int c = col; c <= 8; ++c) std::swap(a[piv][c], a[col][c]);
        const double inv = 1.0 / a[col][col];
        for (int c = col; c <= 8; ++c) a[col][c] *= inv;
        for (int r = 0; r < 8; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int c = col; c <= 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 8; ++r) h[r / 3][r % 3] = a[r][8];
    h[2][0] = h[2][1] = 0.0;
    h[2][2] = 1.0;
    return true;
}

Image warpPerspectiveBgr(const Image& src, const double quad[4][2],
                         int outWidth, int outHeight) {
    Image dst(outWidth, outHeight);
    if (src.width <= 0 || src.height <= 0) return dst;
    double rect[4][2] = {{0, 0},
                         {double(outWidth - 1), 0},
                         {double(outWidth - 1), double(outHeight - 1)},
                         {0, double(outHeight - 1)}};
    // 直接求 dst->src 的逆映射(h·rect = quad)，采样时无需再变换
    double hinv[3][3];
    if (!solveHomography(quad, rect, hinv)) return dst;
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            const double w0 = hinv[2][0] * x + hinv[2][1] * y + hinv[2][2];
            const double sx = (hinv[0][0] * x + hinv[0][1] * y + hinv[0][2]) / w0;
            const double sy = (hinv[1][0] * x + hinv[1][1] * y + hinv[1][2]) / w0;
            double fx = sx, fy = sy;
            // BORDER_REPLICATE
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            if (fx > src.width - 1) fx = src.width - 1;
            if (fy > src.height - 1) fy = src.height - 1;
            const int x0 = std::min(int(std::floor(fx)), src.width - 1);
            const int y0 = std::min(int(std::floor(fy)), src.height - 1);
            const int x1 = std::min(x0 + 1, src.width - 1);
            const int y1 = std::min(y0 + 1, src.height - 1);
            const float wx = float(fx - x0), wy = float(fy - y0);
            const uint8_t* p00 = src.at(y0, x0);
            const uint8_t* p10 = src.at(y0, x1);
            const uint8_t* p01 = src.at(y1, x0);
            const uint8_t* p11 = src.at(y1, x1);
            uint8_t* d = dst.at(y, x);
            for (int c = 0; c < 3; ++c) {
                const float top = p00[c] * (1.f - wx) + p10[c] * wx;
                const float bottom = p01[c] * (1.f - wx) + p11[c] * wx;
                d[c] = (uint8_t)std::lround(top * (1.f - wy) + bottom * wy);
            }
        }
    }
    return dst;
}

void drawQuadBgr(Image& img, const double quad[4][2],
                 const uint8_t color[3], int thickness) {
    auto drawLine = [&](double x0, double y0, double x1, double y1) {
        const double len = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
        const int steps = int(len) + 1;
        for (int s = 0; s <= steps; ++s) {
            const double t = double(s) / double(steps);
            const int x = int(std::lround(x0 + (x1 - x0) * t));
            const int y = int(std::lround(y0 + (y1 - y0) * t));
            for (int dy = -thickness / 2; dy <= thickness / 2; ++dy)
                for (int dx = -thickness / 2; dx <= thickness / 2; ++dx) {
                    const int px = x + dx, py = y + dy;
                    if (px >= 0 && px < img.width && py >= 0 && py < img.height) {
                        std::memcpy(img.at(py, px), color, 3);
                    }
                }
        }
    };
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        drawLine(quad[i][0], quad[i][1], quad[j][0], quad[j][1]);
    }
}
