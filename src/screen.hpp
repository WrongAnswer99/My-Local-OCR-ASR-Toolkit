// 屏幕区域截图：GDI BitBlt 抓取桌面指定矩形(虚拟屏幕坐标，物理像素)，输出 BGR Image。
#pragma once

#include <string>

#include "image.hpp"

struct ScreenRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// 主显示器虚拟屏幕范围(用于把"屏幕内区域"换算为全局坐标)
ScreenRect virtualScreen();

// 截取整个虚拟屏幕中 (x,y,w,h) 矩形；失败返回空 Image 并给出 err
Image captureScreen(const ScreenRect& rect, std::string& err);
