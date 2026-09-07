// region_select.cpp: 全屏框选核心实现(无第三方依赖, Win32 API)。
#include "region_select.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>

namespace {

struct PickerState {
    bool selecting = false;
    int startX = 0, startY = 0;  // 客户区(=虚拟屏原点)坐标
    int curX = 0, curY = 0;
    bool picked = false;
    ScreenRect result;
};

int g_virtX = 0;
int g_virtY = 0;
int g_virtW = 0;
int g_virtH = 0;
PickerState g_state;

// 进程级 DPI aware(与 screen.cpp、ocr_capture_init() 同一策略/判定)。
// 失败说明进程仍处于"未感知"(虚拟化), 此时框选坐标会偏(如 150% 缩放偏 2/3),
// 直接放弃框选而不是返回错误坐标。
bool ensureDpiAware() {
    static bool ok = false;
    static bool tried = false;
    if (tried) return ok;
    tried = true;

    typedef BOOL(WINAPI* FnSetLegacy)();
    bool setOk = false;
    const HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        FnSetLegacy f2 =
            (FnSetLegacy)(void*)GetProcAddress(u, "SetProcessDPIAware");
        if (f2 && f2()) setOk = true;
    }
    if (setOk) {
        ok = true;
        return ok;
    }
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

void normalizeRect(int& x1, int& y1, int& x2, int& y2) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
}

void drawScene(HDC hdc) {
    RECT rc{0, 0, g_virtW, g_virtH};
    HBRUSH veil = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, veil);
    DeleteObject(veil);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    const wchar_t* hint = L"按住左键拖拽框选区域, 松开完成; Esc/右键取消";
    RECT tr{12, 8, g_virtW, 36};
    DrawTextW(hdc, hint, -1, &tr, DT_LEFT | DT_NOCLIP);

    if (g_state.selecting) {
        int x1 = g_state.startX, y1 = g_state.startY;
        int x2 = g_state.curX, y2 = g_state.curY;
        normalizeRect(x1, y1, x2, y2);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 220, 255));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, x1, y1, x2, y2);
        wchar_t dim[64];
        swprintf(dim, 64, L"%dx%d", x2 - x1, y2 - y1);
        RECT dr{x1, y1 - 22, x1 + 160, y1};
        DrawTextW(hdc, dim, -1, &dr, DT_LEFT | DT_NOCLIP);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
    SelectObject(hdc, oldFont);
}

void completeSelection() {
    int x1 = g_state.startX, y1 = g_state.startY;
    int x2 = g_state.curX, y2 = g_state.curY;
    normalizeRect(x1, y1, x2, y2);
    const int w = x2 - x1;
    const int h = y2 - y1;
    if (w < 2 || h < 2) {  // 过小视为取消
        g_state.picked = false;
        PostQuitMessage(0);
        return;
    }
    g_state.result = ScreenRect{g_virtX + x1, g_virtY + y1, w, h};
    g_state.picked = true;
    PostQuitMessage(0);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, g_virtW, g_virtH);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            drawScene(mem);
            BitBlt(hdc, 0, 0, g_virtW, g_virtH, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            g_state.selecting = true;
            g_state.startX = GET_X_LPARAM(lp);
            g_state.startY = GET_Y_LPARAM(lp);
            g_state.curX = g_state.startX;
            g_state.curY = g_state.startY;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (g_state.selecting) {
                g_state.curX = GET_X_LPARAM(lp);
                g_state.curY = GET_Y_LPARAM(lp);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_state.selecting) {
                g_state.curX = GET_X_LPARAM(lp);
                g_state.curY = GET_Y_LPARAM(lp);
                ReleaseCapture();
                g_state.selecting = false;
                completeSelection();
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (g_state.selecting) ReleaseCapture();
                g_state.selecting = false;
                PostQuitMessage(0);
            }
            return 0;
        case WM_RBUTTONDOWN:
            if (g_state.selecting) ReleaseCapture();
            g_state.selecting = false;
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool pickScreenRect(ScreenRect& out, int autoExitMs) {
    if (!ensureDpiAware()) return false;  // 未处于物理像素, 拒绝返回错误坐标
    g_state = PickerState{};
    g_virtX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_virtY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_virtW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_virtH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = L"RegionPickerOverlay";
    if (!RegisterClassW(&wc)) return false;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, wc.lpszClassName,
        L"region_select", WS_POPUP, g_virtX, g_virtY, g_virtW, g_virtH,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return false;
    SetLayeredWindowAttributes(hwnd, 0, 150, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (autoExitMs > 0) {
        Sleep(autoExitMs);  // 自检: 到时按取消退出
        PostQuitMessage(0);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    if (g_state.picked) {
        out = g_state.result;
        return true;
    }
    return false;
}
