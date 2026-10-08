#include "ocr_core.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>
#include <string>

#include "onnx_session.hpp"

// 调试: 设 OCR_DEBUG=1 打印各阶段尺寸, 便于定位崩溃帧
namespace {
bool ocrDebugEnabled() {
    const char* e = std::getenv("OCR_DEBUG");
    return e && std::strcmp(e, "1") == 0;
}
#define OCR_DBG(...)                         \
    do {                                     \
        if (ocrDebugEnabled())               \
            std::fprintf(stderr, __VA_ARGS__); \
    } while (0)
}  // namespace

namespace {

// ---------- 基础几何/数值辅助 ----------

struct Pt {
    double x = 0.0;
    double y = 0.0;
};
using Quad = Pt[4];  // 约定顺序: tl, tr, br, bl

// python round 是"四舍六入五取偶"
int roundHalfEven(double v) {
    const double f = std::floor(v);
    const double frac = v - f;
    if (frac > 0.5) return int(f + 1.0);
    if (frac < 0.5) return int(f);
    return int(std::fmod(f, 2.0) == 0.0 ? f : f + 1.0);
}

// 整数缩放到 32 倍数(python: int(round(v/32)*32))
int scaleTo32(int v) { return roundHalfEven(double(v) / 32.0) * 32; }

double quadArea(const Pt q[4]) {
    double a = 0.0;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        a += q[i].x * q[j].y - q[j].x * q[i].y;
    }
    return std::fabs(a) * 0.5;
}

double quadPerimeter(const Pt q[4]) {
    double len = 0.0;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        len += std::hypot(q[i].x - q[j].x, q[i].y - q[j].y);
    }
    return len;
}

double pointDist(const Pt& a, const Pt& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// 点是否在(凸)四边形内(叉积同号判定，含边界)
bool pointInQuad(const Pt q[4], double px, double py) {
    double sign = 0.0;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        const double cross = (q[j].x - q[i].x) * (py - q[i].y) -
                             (q[j].y - q[i].y) * (px - q[i].x);
        if (std::fabs(cross) < 1e-9) continue;
        if (sign == 0.0) sign = cross > 0 ? 1.0 : -1.0;
        else if ((cross > 0 ? 1.0 : -1.0) != sign) return false;
    }
    return true;
}

// 按 python order_points_clockwise 思路给四角排序 -> tl,tr,br,bl
void orderQuadClockwise(const Pt in[4], Quad& out) {
    int idx[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            if (in[idx[j]].x < in[idx[i]].x) std::swap(idx[i], idx[j]);
    // 左侧两点按 y 升序: tl=smaller y, bl=larger y
    Pt left[2] = {in[idx[0]], in[idx[1]]};
    Pt right[2] = {in[idx[2]], in[idx[3]]};
    if (left[1].y < left[0].y) std::swap(left[0], left[1]);
    if (right[1].y < right[0].y) std::swap(right[0], right[1]);
    out[0] = left[0];   // tl
    out[1] = right[0];  // tr
    out[2] = right[1];  // br
    out[3] = left[1];   // bl
}

// ---------- 最小外接矩形(旋转卡尺式，暴力枚举凸包边) ----------

struct MiniRectResult {
    double w = 0.0, h = 0.0;
    Pt corners[4];  // 顺序未定，需再排序
};

std::vector<Pt> convexHull(const std::vector<Pt>& pts) {
    std::vector<Pt> p = pts;
    std::sort(p.begin(), p.end(), [](const Pt& a, const Pt& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    std::vector<Pt> hull;
    hull.reserve(p.size() * 2);
    auto cross = [](const Pt& o, const Pt& a, const Pt& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    for (const Pt& pt : p) {
        while (hull.size() >= 2 &&
               cross(hull[hull.size() - 2], hull.back(), pt) <= 0)
            hull.pop_back();
        hull.push_back(pt);
    }
    const size_t lower = hull.size();
    for (int i = int(p.size()) - 2; i >= 0; --i) {
        const Pt& pt = p[size_t(i)];
        while (hull.size() > lower &&
               cross(hull[hull.size() - 2], hull.back(), pt) <= 0)
            hull.pop_back();
        hull.push_back(pt);
    }
    if (hull.size() > 1) hull.pop_back();
    return hull;
}

bool minRectOfPoints(const std::vector<Pt>& pts, MiniRectResult& res) {
    if (pts.size() < 3) return false;
    const std::vector<Pt> hull = convexHull(pts);
    if (hull.size() < 3) return false;

    double bestArea = std::numeric_limits<double>::infinity();
    double bestAmin = 0, bestAmax = 0, bestBmin = 0, bestBmax = 0;
    Pt bestU{1, 0}, bestV{0, 1};

    for (size_t i = 0; i < hull.size(); ++i) {
        const Pt& a = hull[i];
        const Pt& b = hull[(i + 1) % hull.size()];
        const double len = pointDist(a, b);
        if (len < 1e-9) continue;
        const double ux = (b.x - a.x) / len;
        const double uy = (b.y - a.y) / len;
        const double vx = -uy;
        const double vy = ux;
        double amin = std::numeric_limits<double>::infinity();
        double amax = -amin, bmin = amin, bmax = -amin;
        for (const Pt& pt : hull) {
            const double pa = pt.x * ux + pt.y * uy;
            const double pb = pt.x * vx + pt.y * vy;
            amin = std::min(amin, pa); amax = std::max(amax, pa);
            bmin = std::min(bmin, pb); bmax = std::max(bmax, pb);
        }
        const double area = (amax - amin) * (bmax - bmin);
        if (area < bestArea) {
            bestArea = area;
            bestAmin = amin; bestAmax = amax;
            bestBmin = bmin; bestBmax = bmax;
            bestU = {ux, uy}; bestV = {vx, vy};
        }
    }
    if (!std::isfinite(bestArea) || bestArea < 1e-12) return false;

    auto cornerAt = [&](double pa, double pb) {
        return Pt{bestU.x * pa + bestV.x * pb, bestU.y * pa + bestV.y * pb};
    };
    res.corners[0] = cornerAt(bestAmin, bestBmin);
    res.corners[1] = cornerAt(bestAmax, bestBmin);
    res.corners[2] = cornerAt(bestAmax, bestBmax);
    res.corners[3] = cornerAt(bestAmin, bestBmax);
    res.w = bestAmax - bestAmin;
    res.h = bestBmax - bestBmin;
    return true;
}

// 像素取值(依模型而定):
//   0 = (v/127.5 - 1) PP-OCRv4 系;  1 = 原值 0..255;  2 = v/255 (PP-OCRv6 系)
float normV(uint8_t v, int mode) {
    if (mode == 2) return float(v) / 255.f;
    if (mode == 1) return float(v);
    return float(v) / 127.5f - 1.f;
}

}  // namespace

namespace {
// 检测输出的一行(已在“送检测的图”尺度上)
struct DetBox {
    Quad quad;  // tl,tr,br,bl
    double score = 0.0;
};
}  // namespace

// ---------- 引擎实现 ----------

struct OCR::Impl {
    OCRConfig cfg;
    OnnxSession det;
    OnnxSession rec;
    OnnxSession cls;
    std::vector<std::string>* characters = nullptr;

    // 载入字典并构造 characters(blank 在 0，空格在末尾)
    bool loadDict(std::string& err) {
        std::ifstream in(cfg.dictPath);
        if (!in.is_open()) {
            err = "cannot open dict: " + cfg.dictPath;
            return false;
        }
        std::vector<std::string> raw;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            raw.push_back(line);
        }
        characters->clear();
        characters->reserve(raw.size() + 2);
        characters->push_back("blank");
        for (const std::string& s : raw) characters->push_back(s);
        characters->push_back(" ");
        return true;
    }

    // ---------- 主预处理(参考 RapidOCR main.preprocess) ----------
    // 输出 work 图 + 记录: padTop(letterbox), ratioH/ratioW(相对原图缩放)
    struct PrepInfo {
        int padTop = 0;
        double ratioH = 1.0;  // 原图H / workH(扣除padding前) ...
        double ratioW = 1.0;
    };

    static void resizeImageKeepRatio(const Image& src, double ratio, Image& dst) {
        int h = int(double(src.height) * ratio);
        int w = int(double(src.width) * ratio);
        h = scaleTo32(h);
        w = scaleTo32(w);
        if (h < 1) h = 1;
        if (w < 1) w = 1;
        dst = resizeBilinear(src, w, h);
    }

    Image preprocess(const Image& src, PrepInfo& info) const {
        Image cur = src;
        // 1) 过大的图先缩小
        if (std::max(cur.width, cur.height) > 2000) {
            const double ratio = cur.height > cur.width
                                     ? 2000.0 / cur.height
                                     : 2000.0 / cur.width;
            Image next;
            resizeImageKeepRatio(cur, ratio, next);
            info.ratioH *= double(cur.height) / double(next.height);
            info.ratioW *= double(cur.width) / double(next.width);
            cur = next;
        }
        // 2) 太小的图放大
        if (std::min(cur.width, cur.height) < 30) {
            const double ratio = cur.height < cur.width
                                     ? 30.0 / cur.height
                                     : 30.0 / cur.width;
            Image next;
            resizeImageKeepRatio(cur, ratio, next);
            info.ratioH *= double(cur.height) / double(next.height);
            info.ratioW *= double(cur.width) / double(next.width);
            cur = next;
        }
        // 3) 全景/矮图上下对称补黑色(与 python add_round_letterbox 一致)
        const int h = cur.height, w = cur.width;
        const double kRatio = 8.0;
        const bool useLimitRatio = double(w) / double(h) > kRatio;
        if (h <= 30 || useLimitRatio) {
            const int newH = std::max(int(double(w) / kRatio), 30) * 2;
            info.padTop = int(std::fabs(double(newH - h)) / 2.0);
            const int workH = h + 2 * info.padTop;  // python 上下各 padTop
            Image work(w, workH);
            for (int y = 0; y < h; ++y)
                std::memcpy(work.at(y + info.padTop, 0), cur.at(y, 0),
                            size_t(w) * 3);
            cur = work;
        }
        return cur;
    }

    // ---------- 检测 ----------
    // work 是可能带 padding 的图；boxes 坐标基于 work
    std::vector<DetBox> textDetect(const Image& work, std::string& err) {
        const int ih = work.height, iw = work.width;
        // 检测输入缩放: detLimitSideLen>0 -> 短边不足则放大(全图默认策略 736);
        // =0(默认) -> 按原尺寸直接识别, 仅对过小输入(<16)做最小保护, 再对齐到 32 倍数
        const int mn = std::min(ih, iw);
        double ratio = 1.0;
        if (cfg.detLimitSideLen > 0 && mn < cfg.detLimitSideLen) {
            ratio = ih < iw ? double(cfg.detLimitSideLen) / double(ih)
                            : double(cfg.detLimitSideLen) / double(iw);
        } else if (cfg.detLimitSideLen <= 0 && mn < 16) {
            ratio = ih < iw ? 16.0 / double(ih) : 16.0 / double(iw);
        }
        int gh = int(double(ih) * ratio);
        int gw = int(double(iw) * ratio);
        gh = scaleTo32(gh);
        gw = scaleTo32(gw);
        if (gh < 8) gh = 8;
        if (gw < 8) gw = 8;
        const Image grid = resizeBilinear(work, gw, gh);

        std::vector<float> input(size_t(gh) * gw * 3);
        const uint8_t* src = grid.data.data();
        // 填 CHW(通道次序不影响数值, 但保持 BGR)
        for (int y = 0; y < gh; ++y) {
            for (int x = 0; x < gw; ++x) {
                const uint8_t* p = src + (size_t(y) * gw + x) * 3;
                input[0 * size_t(gh) * gw + size_t(y) * gw + x] =
                    normV(p[0], cfg.pixelRange);
                input[1 * size_t(gh) * gw + size_t(y) * gw + x] =
                    normV(p[1], cfg.pixelRange);
                input[2 * size_t(gh) * gw + size_t(y) * gw + x] =
                    normV(p[2], cfg.pixelRange);
            }
        }
        std::vector<int64_t> inShape = {1, 3, gh, gw};
        std::vector<int64_t> outShape;
        std::vector<float> outData;
        if (!det.run(inShape, input.data(), outShape, outData, err)) return {};
        if (ocrDebugEnabled()) {
            std::fprintf(stderr, "[dbg] det grid=%dx%d -> out", gw, gh);
            for (int64_t d : outShape) std::fprintf(stderr, " %lld", (long long)d);
            std::fprintf(stderr, "\n");
        }
        // 期望输出 [1,1,gh,gw]
        if (outShape.size() != 4) {
            err = "unexpected det output dims";
            return {};
        }
        const int ogh = int(outShape[2]);
        const int ogw = int(outShape[3]);
        if (ogh != gh || ogw != gw) {
            err = "det output size mismatch";
            return {};
        }
        const float* prob = outData.data();  // [0][0] 平面即起始
        return dbPostprocess(prob, gh, gw, work.width, work.height);
    }

    // DB 后处理(参考 RapidOCR DBPostProcess)
    std::vector<DetBox> dbPostprocess(const float* prob, int gh, int gw,
                                      int destW, int destH) {
        const double thresh = 0.3;
        const double boxThresh = 0.5;
        const double unclipRatio = 1.6;
        const int minSize = 3;

        // 二值 + 2x2 dilation(向右下扩 1px；从快照读、写新数组，避免级联扩张)
        std::vector<uint8_t> seg(size_t(gh) * gw, 0);
        for (size_t i = 0; i < seg.size(); ++i) seg[i] = prob[i] > thresh ? 1 : 0;
        std::vector<uint8_t> dilate = seg;
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                if (!seg[size_t(y) * gw + x]) continue;
                if (x + 1 < gw) dilate[size_t(y) * gw + x + 1] = 1;
                if (y + 1 < gh) {
                    dilate[size_t(y + 1) * gw + x] = 1;
                    if (x + 1 < gw) dilate[size_t(y + 1) * gw + x + 1] = 1;
                }
            }

        // 连通域(8邻域)
        std::vector<uint8_t> visited(size_t(gh) * gw, 0);
        std::vector<DetBox> boxes;
        const int dy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        const int dx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
        for (int y0 = 0; y0 < gh; ++y0) {
            for (int x0 = 0; x0 < gw; ++x0) {
                const size_t idx = size_t(y0) * gw + x0;
                if (!dilate[idx] || visited[idx]) continue;
                // BFS 收集连通域像素
                std::vector<Pt> pts;
                std::queue<std::pair<int, int>> q;
                q.push({x0, y0});
                visited[idx] = 1;
                while (!q.empty()) {
                    const auto [x, y] = q.front();
                    q.pop();
                    pts.push_back(Pt{double(x), double(y)});
                    for (int k = 0; k < 8; ++k) {
                        const int nx = x + dx8[k];
                        const int ny = y + dy8[k];
                        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                        const size_t ni = size_t(ny) * gw + nx;
                        if (dilate[ni] && !visited[ni]) {
                            visited[ni] = 1;
                            q.push({nx, ny});
                        }
                    }
                }
                if (pts.size() < 3) continue;

                MiniRectResult rect;
                if (!minRectOfPoints(pts, rect)) continue;
                if (std::min(rect.w, rect.h) < minSize) continue;

                Quad q0;
                orderQuadClockwise(rect.corners, q0);
                // box_score_fast: 四边形区域内 prob 均值
                double xmin = q0[0].x, xmax = q0[0].x;
                double ymin = q0[0].y, ymax = q0[0].y;
                for (int i = 1; i < 4; ++i) {
                    xmin = std::min(xmin, q0[i].x); xmax = std::max(xmax, q0[i].x);
                    ymin = std::min(ymin, q0[i].y); ymax = std::max(ymax, q0[i].y);
                }
                int bx0 = std::max(0, int(std::floor(xmin)));
                int bx1 = std::min(gw - 1, int(std::ceil(xmax)));
                int by0 = std::max(0, int(std::floor(ymin)));
                int by1 = std::min(gh - 1, int(std::ceil(ymax)));
                double sum = 0.0;
                int cnt = 0;
                for (int y = by0; y <= by1; ++y)
                    for (int x = bx0; x <= bx1; ++x)
                        if (pointInQuad(q0, double(x), double(y))) {
                            sum += prob[size_t(y) * gw + x];
                            ++cnt;
                        }
                const double score = cnt > 0 ? sum / double(cnt) : 0.0;
                if (boxThresh > score) continue;

                // unclip: 按文本框长/短轴方向各向外扩 dist(近似 pyclipper 对矩形的外扩;
                // 不用 clipper 库: 其在本工程所编译形态下存在越界写(实测堆损坏))
                const double dist =
                    quadArea(q0) * unclipRatio / quadPerimeter(q0);
                Pt e[4];
                const double wTop = pointDist(q0[0], q0[1]);
                const double wBot = pointDist(q0[2], q0[3]);
                const double hLft = pointDist(q0[0], q0[3]);
                const double hRgt = pointDist(q0[1], q0[2]);
                const double w0 = 0.5 * (wTop + wBot);
                const double h0 = 0.5 * (hLft + hRgt);
                const double w2 = w0 + 2.0 * dist;
                const double h2 = h0 + 2.0 * dist;
                Pt u{0, 0}, v{0, 0};
                if (w0 > 1e-6) u = Pt{(q0[1].x - q0[0].x) / w0,
                                      (q0[1].y - q0[0].y) / w0};
                if (h0 > 1e-6) v = Pt{(q0[3].x - q0[0].x) / h0,
                                      (q0[3].y - q0[0].y) / h0};
                const Pt c{(q0[0].x + q0[1].x + q0[2].x + q0[3].x) * 0.25,
                           (q0[0].y + q0[1].y + q0[2].y + q0[3].y) * 0.25};
                const Pt a{u.x * (w2 * 0.5), u.y * (w2 * 0.5)};
                const Pt b{v.x * (h2 * 0.5), v.y * (h2 * 0.5)};
                e[0] = Pt{c.x - a.x - b.x, c.y - a.y - b.y};
                e[1] = Pt{c.x + a.x - b.x, c.y + a.y - b.y};
                e[2] = Pt{c.x + a.x + b.x, c.y + a.y + b.y};
                e[3] = Pt{c.x - a.x + b.x, c.y - a.y + b.y};
                std::vector<Pt> expanded = {e[0], e[1], e[2], e[3]};
                MiniRectResult rect2;
                if (expanded.empty()) continue;
                if (!minRectOfPoints(expanded, rect2)) continue;
                if (std::min(rect2.w, rect2.h) < minSize + 2) continue;

                Quad q1;
                orderQuadClockwise(rect2.corners, q1);
                DetBox box;
                box.score = score;
                for (int i = 0; i < 4; ++i) {
                    // 缩放到 dest(work) 尺寸
                    const int mx = int(std::lround(q1[i].x / double(gw) * destW));
                    const int my = int(std::lround(q1[i].y / double(gh) * destH));
                    box.quad[i] = Pt{double(std::min(std::max(mx, 0), destW)),
                                     double(std::min(std::max(my, 0), destH))};
                }
                boxes.push_back(box);
            }
        }
        return boxes;
    }

    // ---------- 检测结果过滤/排序(参考 filter_tag_det_res + sorted_boxes) ----------
    std::vector<DetBox> filterAndSortBoxes(std::vector<DetBox> in, int imgH,
                                           int imgW) {
        std::vector<DetBox> kept;
        for (DetBox& b : in) {
            for (int i = 0; i < 4; ++i) {
                b.quad[i].x = std::min(std::max(b.quad[i].x, 0.0), double(imgW - 1));
                b.quad[i].y = std::min(std::max(b.quad[i].y, 0.0), double(imgH - 1));
            }
            const double w = std::max(pointDist(b.quad[0], b.quad[1]),
                                      pointDist(b.quad[2], b.quad[3]));
            const double h = std::max(pointDist(b.quad[0], b.quad[3]),
                                      pointDist(b.quad[1], b.quad[2]));
            if (w <= 3 || h <= 3) continue;
            kept.push_back(b);
        }
        // 排序: 上到下，同行内左到右
        std::sort(kept.begin(), kept.end(), [](const DetBox& a, const DetBox& b) {
            if (a.quad[0].y != b.quad[0].y) return a.quad[0].y < b.quad[0].y;
            return a.quad[0].x < b.quad[0].x;
        });
        for (size_t i = 1; i < kept.size(); ++i) {
            size_t j = i;
            while (j > 0 &&
                   std::fabs(kept[j].quad[0].y - kept[j - 1].quad[0].y) < 10 &&
                   kept[j].quad[0].x < kept[j - 1].quad[0].x) {
                std::swap(kept[j], kept[j - 1]);
                --j;
            }
        }
        return kept;
    }

    // ---------- 识别 ----------
    // crops 与结果一一对应返回 (text, score)
    std::vector<std::pair<std::string, double>> textRecognize(
        const std::vector<Image>& crops, std::string& err) {
        const int imgH = 48;
        const double baseRatio = 320.0 / 48.0;
        const int batchNum = 6;
        const size_t n = crops.size();
        std::vector<std::pair<std::string, double>> res(n, {"", 0.0});

        // 宽高比排序(与 python 一致)
        std::vector<size_t> order(n);
        std::vector<double> whRatio(n);
        for (size_t i = 0; i < n; ++i) {
            order[i] = i;
            whRatio[i] = crops[i].height > 0
                             ? double(crops[i].width) / double(crops[i].height)
                             : 1.0;
        }
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return whRatio[a] < whRatio[b]; });

        for (size_t beg = 0; beg < n; beg += batchNum) {
            const size_t end = std::min(n, beg + size_t(batchNum));
            double maxRatio = baseRatio;
            for (size_t k = beg; k < end; ++k)
                maxRatio = std::max(maxRatio, whRatio[order[k]]);
            const int imgW = int(double(imgH) * maxRatio);

            // 构造 batch: [batch,3,48,W]
            const size_t bnum = end - beg;
            std::vector<float> batch(bnum * 3 * size_t(imgH) * size_t(imgW), 0.f);
            for (size_t k = 0; k < bnum; ++k) {
                const Image& c = crops[order[beg + k]];
                const double ratio = double(c.width) / double(c.height);
                int rw = int(std::ceil(double(imgH) * ratio));
                if (rw > imgW) rw = imgW;
                if (rw < 1) rw = 1;
                const Image resized = resizeBilinear(c, rw, imgH);
                for (int y = 0; y < imgH; ++y)
                    for (int x = 0; x < rw; ++x) {
                        const uint8_t* p = resized.at(y, x);
                        float* dst = batch.data() +
                                     (k * 3 * size_t(imgH) * imgW +
                                      size_t(y) * imgW + x);
                        dst[0] = normV(p[0], cfg.pixelRange);
                        dst[size_t(imgH) * imgW] = normV(p[1], cfg.pixelRange);
                        dst[2 * size_t(imgH) * imgW] =
                            normV(p[2], cfg.pixelRange);
                    }
            }
            std::vector<int64_t> inShape = {int64_t(bnum), 3, imgH, imgW};
            std::vector<int64_t> outShape;
            std::vector<float> outData;
            OCR_DBG("[dbg] rec batch: N=%zu W=%d\n", bnum, imgW);
            if (!rec.run(inShape, batch.data(), outShape, outData, err)) {
                OCR_DBG("[dbg] rec run FAILED: %s\n", err.c_str());
                return {};
            }
            if (ocrDebugEnabled()) {
                std::fprintf(stderr, "[dbg] rec out: N=%zu", bnum);
                for (int64_t d : outShape) std::fprintf(stderr, " %lld", (long long)d);
                std::fprintf(stderr, "\n");
            }
            if (outShape.size() != 3) {
                err = "unexpected rec output dims";
                return {};
            }
            const int C = int(outShape[2]);
            const int T = int(outShape[1]);
            if (size_t(C) > characters->size()) {
                err = "rec class count > dict size";
                return {};
            }
            for (size_t k = 0; k < bnum; ++k) {
                const float* row = outData.data() + k * size_t(T) * C;
                auto [text, conf] = ctcDecode(row, T, C);
                res[order[beg + k]] = {text, conf};
            }
        }
        return res;
    }

    std::pair<std::string, double> ctcDecode(const float* row, int T, int C) {
        std::vector<int> idx;
        std::vector<float> prob;
        idx.resize(size_t(T));
        prob.resize(size_t(T));
        for (int t = 0; t < T; ++t) {
            const float* p = row + size_t(t) * C;
            int best = 0;
            float bestVal = p[0];
            for (int c = 1; c < C; ++c)
                if (p[c] > bestVal) { bestVal = p[c]; best = c; }
            idx[size_t(t)] = best;
            prob[size_t(t)] = bestVal;
        }
        std::string text;
        double sum = 0.0;
        int count = 0;
        for (int t = 0; t < T; ++t) {
            if (idx[size_t(t)] == 0) continue;              // blank
            if (t > 0 && idx[size_t(t)] == idx[size_t(t - 1)]) continue;  // 去重
            text += (*characters)[size_t(idx[size_t(t)])];
            sum += prob[size_t(t)];
            ++count;
        }
        if (count == 0) return {"", 0.0};
        return {text, sum / double(count)};
    }
};

// ---------- 公共接口 ----------

OCR::OCR() { impl_ = new Impl(); }
OCR::~OCR() { destroyImpl(); }

void OCR::destroyImpl() {
    delete impl_;
    impl_ = nullptr;
}

bool OCR::load(const OCRConfig& cfg, std::string& err) {
    err.clear();
    if (!OnnxSession::ensureInitialized(err)) return false;
    impl_->cfg = cfg;
    impl_->characters = &characters_;

    if (!impl_->det.load(cfg.detModelPath, err, cfg.threads, cfg.device, cfg.gpuDeviceId)) return false;
    if (!impl_->rec.load(cfg.recModelPath, err, cfg.threads, cfg.device, cfg.gpuDeviceId)) return false;
    if (cfg.useCls && !cfg.clsModelPath.empty()) {
        if (!impl_->cls.load(cfg.clsModelPath, err, cfg.threads, cfg.device, cfg.gpuDeviceId)) return false;
    }
    if (!impl_->loadDict(err)) return false;

    // 简单校验识别模型类别数与字典长度匹配
    std::vector<int64_t> inShape, outShape;
    if (impl_->rec.inputShape(inShape, err)) {
        // 只打印提示，不强校验(输出类别在 run 时校验)
    }
    return true;
}

std::vector<OCRLine> OCR::runPath(const std::string& imagePath,
                                        std::string& err) {
    Image img;
    if (!loadImageBgr(imagePath, img)) {
        err = "load image failed: " + imagePath;
        return {};
    }
    return runImage(img, err);
}

std::vector<OCRLine> OCR::runImage(const Image& src, std::string& err) {
    Impl* P = impl_;
    if (!P) { err = "OCR not loaded"; return {}; }

    const int rawW = src.width, rawH = src.height;
    Impl::PrepInfo info;
    const auto started = std::chrono::steady_clock::now();
    const Image work = P->preprocess(src, info);
    const auto preprocessed = std::chrono::steady_clock::now();

    std::vector<DetBox> detBoxes = P->textDetect(work, err);
    if (!err.empty() && detBoxes.empty()) return {};
    detBoxes = P->filterAndSortBoxes(std::move(detBoxes), work.height,
                                     work.width);
    const auto detected = std::chrono::steady_clock::now();
    OCR_DBG("[dbg] work=%dx%d padTop=%d ratio=%.3f detBoxes=%zu\n", work.width,
            work.height, info.padTop, info.ratioH, detBoxes.size());

    // 裁剪文本行(python get_crop_img_list)
    std::vector<Image> crops;
    for (const DetBox& b : detBoxes) {
        double wTop = pointDist(b.quad[0], b.quad[1]);
        double wBottom = pointDist(b.quad[2], b.quad[3]);
        double hLeft = pointDist(b.quad[0], b.quad[3]);
        double hRight = pointDist(b.quad[1], b.quad[2]);
        int cw = std::max(2, int(std::max(wTop, wBottom)));
        int ch = std::max(2, int(std::max(hLeft, hRight)));
        const Pt* q = b.quad;
        double qArr[4][2] = {{q[0].x, q[0].y},
                             {q[1].x, q[1].y},
                             {q[2].x, q[2].y},
                             {q[3].x, q[3].y}};
        Image crop = warpPerspectiveBgr(work, qArr, cw, ch);
        if (double(ch) / double(cw) >= 1.5)
            crop = rotate90CounterClockwise(crop);
        OCR_DBG("[dbg] crop#%zu img %dx%d (ratio %.2f)\n", crops.size(),
                crop.width, crop.height,
                crop.height > 0
                    ? double(crop.width) / double(crop.height)
                    : 0.0);
        crops.push_back(std::move(crop));
    }

    const auto cropped = std::chrono::steady_clock::now();
    auto recRes = P->textRecognize(crops, err);
    const auto recognized = std::chrono::steady_clock::now();
    if (!err.empty()) return {};

    // 输出: 把 padded 坐标映射回原图，并按置信度过滤
    std::vector<OCRLine> lines;
    const double hRatio = info.ratioH, wRatio = info.ratioW;
    for (size_t i = 0; i < detBoxes.size() && i < recRes.size(); ++i) {
        const double score = recRes[i].second;
        if (score < P->cfg.textScore) continue;
        OCRLine line;
        line.text = recRes[i].first;
        line.score = score;
        for (int k = 0; k < 4; ++k) {
            double x = detBoxes[i].quad[k].x;
            double y = detBoxes[i].quad[k].y - info.padTop;  // 去掉 letterbox
            x *= wRatio;   // 缩放回原图
            y *= hRatio;
            x = std::min(std::max(x, 0.0), double(rawW));
            y = std::min(std::max(y, 0.0), double(rawH));
            line.box[k].x = x;
            line.box[k].y = y;
        }
        lines.push_back(std::move(line));
    }
    const char* timing = std::getenv("OCR_TIMING");
    if (timing && std::strcmp(timing, "1") == 0) {
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::fprintf(stderr, "[ocr-timing] device=%s preprocess=%.3f detect=%.3f crop=%.3f recognize=%.3f total=%.3f lines=%zu\n",
            ocrDeviceName(P->cfg.device), ms(started, preprocessed),
            ms(preprocessed, detected), ms(detected, cropped), ms(cropped, recognized),
            ms(started, recognized), lines.size());
    }
    return lines;
}
