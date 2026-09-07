// region_select: 全屏框选(纯 Win32)。运行后显示半透明遮罩, 用户拖拽一个矩形。
// 供 region_picker(命令行输出) 与 ocrmon(框选后持续识别) 复用。
#pragma once

#include "screen.hpp"

// 阻塞直到用户完成一次框选。
// out: 选中区域(虚拟屏幕绝对坐标, 与 captureScreen 一致)。
// autoExitMs>0 仅用于自动化自检: 该毫秒数后自动按"取消"返回。
// 返回 true=用户松开左键且区域有效; false=取消/Esc/过小。
bool pickScreenRect(ScreenRect& out, int autoExitMs = 0);
