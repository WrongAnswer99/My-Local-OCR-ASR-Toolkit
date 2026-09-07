// example.cpp - 最小用法示例(纯消费者, 只 include 一个头文件, 不链接任何库)。
// 流程: 框选屏幕区域 -> 持续识别 -> 该区域出现"你好" -> 输出"你好，世界"并退出。
//
// 本文件与 sdk/ocr.hpp 均为 UTF-8(现代工具链 VSCode/Trae 默认), 无需任何编码参数:
//   g++ -O2 -I sdk sdk\examples\example.cpp -o dist\example.exe
// 运行: 直接双击 example.exe(真实控制台), 程序第一行 SetConsoleOutputCP(CP_UTF8) 保证中文正常显示。
#include "ocr.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    // 控制台按 UTF-8 输出; 必须在第一次输出之前调用(放 main 最开头)
    SetConsoleOutputCP(CP_UTF8);

    ocr::OCR e;  // C++ 封装: 内部 LoadLibrary 动态加载 ocr.dll
    std::string err;
    // [必须] 创建任何窗口之前先让进程按物理像素工作(150% 缩放的屏幕必需)
    if (!e.loadDll("ocr.dll", &err) || !e.captureInit(&err) ||
        !e.init("", "", "", 2, 4, 0, 0.5, &err)) {
        std::fprintf(stderr, "初始化失败: %s\n", err.c_str());
        return 1;
    }

    // 1) 框选要监听的区域(全屏遮罩, 鼠标拖拽; Esc/右键取消)
    ocr::Rect r;
    if (!e.pickBox(r, &err)) {
        std::fprintf(stderr, "框选取消: %s\n", err.c_str());
        return 1;
    }
    std::printf("已框选 %d,%d %dx%d, 等待出现\"你好\"...\n", r.x, r.y, r.w, r.h);

    // 2) 阻塞监听: 每 200ms 抓屏+识别, 出现"你好"即返回(0=超时; timeoutMs=0 无限等)
    const std::vector<std::string> keys = {"你好"};
    std::vector<ocr::Line> hits;
    if (e.watch(&r, keys, 200, 0, hits, &err) <= 0) {
        std::fprintf(stderr, "监听失败: %s\n", err.c_str());
        return 1;
    }

    // 3) 命中目标文字
    std::printf("你好，世界\n");
    return 0;
}
