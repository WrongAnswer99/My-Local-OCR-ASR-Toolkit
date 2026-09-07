// region_picker: 全屏框选工具(命令行版)。
// 运行后显示半透明遮罩, 按住左键拖拽框选, 松开后输出
//   picked: --rect=x,y,w,h
// 并把该串复制到剪贴板, 供 ocrwatch --rect=... 直接使用。Esc/右键取消。
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "region_select.hpp"

int main(int argc, char* argv[]) {
    SetProcessDPIAware();
    SetConsoleOutputCP(CP_UTF8);

    int autoExitMs = 0;
    for (int i = 1; i < argc; ++i)
        if (std::sscanf(argv[i], "--auto-exit-ms=%d", &autoExitMs) == 1) break;

    ScreenRect rect;
    if (!pickScreenRect(rect, autoExitMs)) {
        if (autoExitMs > 0) {
            std::printf("[self-check] no pick (auto exit), ok\n");
            return 0;
        }
        std::printf("cancelled\n");
        return 1;
    }

    char line[128] = {0};
    std::snprintf(line, sizeof(line), "--rect=%d,%d,%d,%d", rect.x, rect.y,
                  rect.w, rect.h);
    std::printf("picked: %s\n", line);
    std::printf("use it: ocrwatch %s --key=... \n", line);

    // 复制到剪贴板(UTF-16)
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
    if (wideLen > 1) {
        std::wstring wide(size_t(wideLen - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wide.data(), wideLen);
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (mem) {
                wchar_t* dst = static_cast<wchar_t*>(GlobalLock(mem));
                if (dst) {
                    std::memcpy(dst, wide.c_str(), bytes);
                    GlobalUnlock(mem);
                    SetClipboardData(CF_UNICODETEXT, mem);
                } else {
                    GlobalFree(mem);
                }
            }
            CloseClipboard();
        }
    }
    return 0;
}
