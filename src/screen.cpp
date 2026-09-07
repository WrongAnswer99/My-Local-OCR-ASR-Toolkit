#include "screen.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
// 让进程按物理像素工作(DPI aware)。与 ocr_capture_init()/可用 exe 同一策略
// (SetProcessDPIAware 系统级)。三种结果都视为"已物理像素":
//   1) 本次设置成功;  2) 进程已被设置成系统级/每显示器感知;
//   3) 设置被拒(返回 FALSE)但 GetProcessDpiAwareness 显示 mode>=1。
// 仅当确实仍处于"未感知"(虚拟化)时才返回 false, 由调用方明确报错。
bool ensureDpiAware() {
    static bool ok = false;
    static bool tried = false;
    if (tried) return ok;
    tried = true;

    typedef BOOL(WINAPI* FnSetLegacy)();
    bool setOk = false;
    const HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        FnSetLegacy f2 = (FnSetLegacy)(void*)GetProcAddress(u, "SetProcessDPIAware");
        if (f2 && f2()) setOk = true;
    }
    if (setOk) {
        ok = true;
        return ok;
    }
    // 设置被拒: 检查是否已是某种感知模式(0=未感知 1=系统 2=每显示器)
    typedef HRESULT(WINAPI* FnGetA)(int*);
    const HMODULE sh = GetModuleHandleW(L"shcore.dll");
    int mode = 0;
    bool aware = false;
    if (sh) {
        FnGetA f = (FnGetA)(void*)GetProcAddress(sh, "GetProcessDpiAwareness");
        if (f && SUCCEEDED(f(&mode)) && mode != 0) aware = true;
    }
    ok = aware;
    return ok;
}
}  // namespace

Image captureScreen(const ScreenRect& rect, std::string& err) {
    if (!ensureDpiAware()) {
        err = "DPI: SetProcessDPIAware 失败——进程已被固定为不感知, 请在 main "
              "最开头先调用 ocr_capture_init()";
        return {};
    }
    if (rect.w <= 0 || rect.h <= 0) {
        err = "invalid capture rect";
        return {};
    }
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        err = "GetDC failed";
        return {};
    }
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, rect.w, rect.h);
    Image img;
    if (!memDc || !bmp) {
        err = "CreateCompatibleDC/CreateCompatibleBitmap failed";
        if (bmp) DeleteObject(bmp);
        if (memDc) DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return {};
    }
    HGDIOBJ old = SelectObject(memDc, bmp);
    const bool ok = BitBlt(memDc, 0, 0, rect.w, rect.h, screenDc, rect.x, rect.y,
                           SRCCOPY) != 0;
    if (!ok) {
        err = "BitBlt failed";
    } else {
        // 注意: 24bpp DIB 每行按 4 字节对齐, 用 w*3 的紧凑缓冲会越界(行宽非 4 倍数时)。
        // 改用 32bpp(top-down 无行填充), 再从 BGRA 中取前 3 字节即为 BGR。
        img = Image(rect.w, rect.h);
        std::vector<uint8_t> raw(size_t(rect.w) * rect.h * 4);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = rect.w;
        bi.bmiHeader.biHeight = -rect.h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (!GetDIBits(memDc, bmp, 0, UINT(rect.h), raw.data(), &bi,
                       DIB_RGB_COLORS)) {
            err = "GetDIBits failed";
            img = Image();
        } else {
            const uint8_t* src = raw.data();
            uint8_t* dst = img.data.data();
            const size_t n = size_t(rect.w) * rect.h;
            for (size_t i = 0; i < n; ++i, src += 4, dst += 3) {
                dst[0] = src[0];  // B
                dst[1] = src[1];  // G
                dst[2] = src[2];  // R
            }
        }
    }
    SelectObject(memDc, old);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return img;
}

ScreenRect virtualScreen() {
    ensureDpiAware();
    ScreenRect r;
    r.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}
