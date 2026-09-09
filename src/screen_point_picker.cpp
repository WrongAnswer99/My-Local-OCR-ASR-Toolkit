// screen_point_picker: observe real mouse clicks through a click-through overlay.
// The low-level hook never consumes an event, so the application below the
// overlay receives exactly the same click.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"ScreenPointPickerOverlay";
constexpr UINT_PTR kAutoExitTimer = 1;
constexpr std::size_t kMaxVisibleClicks = 64;

enum class MouseButton {
    Left,
    Right,
    Middle,
    X1,
    X2,
};

enum class DpiMode {
    Unaware = 0,
    System = 1,
    PerMonitor = 2,
    PerMonitorV2 = 3,
};

struct ClickRecord {
    POINT screen{};
    MouseButton button = MouseButton::Left;
    std::uint64_t number = 0;
};

HWND g_window = nullptr;
HHOOK g_mouseHook = nullptr;
int g_virtualX = 0;
int g_virtualY = 0;
int g_virtualW = 0;
int g_virtualH = 0;
std::uint64_t g_clickCount = 0;
std::vector<ClickRecord> g_clicks;
bool g_leftButtonDown = false;
bool g_rightButtonDown = false;
bool g_exitChordTriggered = false;
bool g_pauseOnExit = false;

// Use dynamic loading so the executable still starts on Windows versions that
// do not export the newer DPI APIs. All calls happen before the first HWND is
// created, which is required when setting process DPI awareness in code.
DpiMode queryDpiMode() {
    using FnGetThreadContext = HANDLE(WINAPI*)();
    using FnGetAwareness = int(WINAPI*)(HANDLE);
    using FnContextsEqual = BOOL(WINAPI*)(HANDLE, HANDLE);

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        const auto getThreadContext = reinterpret_cast<FnGetThreadContext>(
            GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
        const auto getAwareness = reinterpret_cast<FnGetAwareness>(
            GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext"));
        const auto contextsEqual = reinterpret_cast<FnContextsEqual>(
            GetProcAddress(user32, "AreDpiAwarenessContextsEqual"));
        if (getThreadContext && getAwareness) {
            const HANDLE current = getThreadContext();
            const HANDLE perMonitorV2 =
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
            if (contextsEqual && contextsEqual(current, perMonitorV2)) {
                return DpiMode::PerMonitorV2;
            }
            const int awareness = getAwareness(current);
            if (awareness == 2) return DpiMode::PerMonitor;
            if (awareness == 1) return DpiMode::System;
            return DpiMode::Unaware;
        }
    }

    // Windows 8.1 fallback for querying process awareness.
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using FnGetProcessAwareness = HRESULT(WINAPI*)(HANDLE, int*);
        const auto getProcessAwareness =
            reinterpret_cast<FnGetProcessAwareness>(
                GetProcAddress(shcore, "GetProcessDpiAwareness"));
        int awareness = 0;
        const bool queried = getProcessAwareness &&
                             SUCCEEDED(getProcessAwareness(nullptr, &awareness));
        FreeLibrary(shcore);
        if (queried) {
            if (awareness == 2) return DpiMode::PerMonitor;
            if (awareness == 1) return DpiMode::System;
        }
    }
    return DpiMode::Unaware;
}

DpiMode enableBestDpiAwareness() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using FnSetContext = BOOL(WINAPI*)(HANDLE);
        const auto setContext = reinterpret_cast<FnSetContext>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        const HANDLE perMonitorV2 =
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
        if (setContext && setContext(perMonitorV2)) {
            return DpiMode::PerMonitorV2;
        }

        // A manifest or an earlier call may already have fixed the mode.
        const DpiMode existing = queryDpiMode();
        if (existing != DpiMode::Unaware) return existing;
    }

    // Windows 8.1 fallback: Per Monitor V1.
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using FnSetProcessAwareness = HRESULT(WINAPI*)(int);
        const auto setProcessAwareness =
            reinterpret_cast<FnSetProcessAwareness>(
                GetProcAddress(shcore, "SetProcessDpiAwareness"));
        const HRESULT result = setProcessAwareness
                                   ? setProcessAwareness(2)
                                   : E_NOTIMPL;
        FreeLibrary(shcore);
        if (SUCCEEDED(result)) return DpiMode::PerMonitor;

        const DpiMode existing = queryDpiMode();
        if (existing != DpiMode::Unaware) return existing;
    }

    // Last-resort legacy mode. This is only fully safe when every display uses
    // the same scale, so main() rejects it for this multi-screen overlay.
    if (SetProcessDPIAware()) return DpiMode::System;
    return queryDpiMode();
}

const char* dpiModeName(DpiMode mode) {
    switch (mode) {
        case DpiMode::PerMonitorV2: return "per-monitor v2";
        case DpiMode::PerMonitor: return "per-monitor";
        case DpiMode::System: return "system";
        case DpiMode::Unaware: return "unaware";
    }
    return "unknown";
}

void readVirtualScreenBounds() {
    g_virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

void refreshOverlayBounds(HWND hwnd) {
    readVirtualScreenBounds();
    SetWindowPos(hwnd, HWND_TOPMOST, g_virtualX, g_virtualY, g_virtualW,
                 g_virtualH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, FALSE);
}

UINT getWindowDpi(HWND hwnd) {
    using FnGetWindowDpi = UINT(WINAPI*)(HWND);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto getDpi = user32
                            ? reinterpret_cast<FnGetWindowDpi>(
                                  GetProcAddress(user32, "GetDpiForWindow"))
                            : nullptr;
    return getDpi ? getDpi(hwnd) : 96;
}

bool validatePhysicalCoordinateSpace(HWND hwnd) {
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return false;
    if (windowRect.left != g_virtualX || windowRect.top != g_virtualY ||
        windowRect.right - windowRect.left != g_virtualW ||
        windowRect.bottom - windowRect.top != g_virtualH) {
        return false;
    }

    POINT screenPoint{};
    if (!GetCursorPos(&screenPoint)) return false;
    POINT clientPoint = screenPoint;
    if (!ScreenToClient(hwnd, &clientPoint)) return false;
    return clientPoint.x == screenPoint.x - g_virtualX &&
           clientPoint.y == screenPoint.y - g_virtualY;
}

const wchar_t* buttonLabel(MouseButton button) {
    switch (button) {
        case MouseButton::Left: return L"L";
        case MouseButton::Right: return L"R";
        case MouseButton::Middle: return L"M";
        case MouseButton::X1: return L"X1";
        case MouseButton::X2: return L"X2";
    }
    return L"?";
}

const char* buttonName(MouseButton button) {
    switch (button) {
        case MouseButton::Left: return "left";
        case MouseButton::Right: return "right";
        case MouseButton::Middle: return "middle";
        case MouseButton::X1: return "x1";
        case MouseButton::X2: return "x2";
    }
    return "unknown";
}

COLORREF buttonColor(MouseButton button) {
    switch (button) {
        case MouseButton::Left: return RGB(0, 230, 255);
        case MouseButton::Right: return RGB(255, 180, 40);
        case MouseButton::Middle: return RGB(210, 120, 255);
        case MouseButton::X1:
        case MouseButton::X2: return RGB(100, 255, 120);
    }
    return RGB(255, 255, 255);
}

bool decodeButton(WPARAM message, const MSLLHOOKSTRUCT& event,
                  MouseButton& button) {
    switch (message) {
        case WM_LBUTTONDOWN:
            button = MouseButton::Left;
            return true;
        case WM_RBUTTONDOWN:
            button = MouseButton::Right;
            return true;
        case WM_MBUTTONDOWN:
            button = MouseButton::Middle;
            return true;
        case WM_XBUTTONDOWN:
            button = HIWORD(event.mouseData) == XBUTTON1
                         ? MouseButton::X1
                         : MouseButton::X2;
            return true;
        default:
            return false;
    }
}

void recordClick(const POINT& screen, MouseButton button) {
    ++g_clickCount;
    g_clicks.push_back(ClickRecord{screen, button, g_clickCount});
    if (g_clicks.size() > kMaxVisibleClicks) {
        g_clicks.erase(g_clicks.begin(),
                       g_clicks.begin() + (g_clicks.size() - kMaxVisibleClicks));
    }

    std::printf("click #%llu: %s x=%ld y=%ld\n",
                static_cast<unsigned long long>(g_clickCount),
                buttonName(button), screen.x, screen.y);
    std::fflush(stdout);
    if (g_window) InvalidateRect(g_window, nullptr, FALSE);
}

LRESULT CALLBACK mouseHookProc(int code, WPARAM wp, LPARAM lp) {
    bool closeAfterForwarding = false;
    if (code == HC_ACTION) {
        const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);

        if (wp == WM_LBUTTONDOWN) g_leftButtonDown = true;
        if (wp == WM_RBUTTONDOWN) g_rightButtonDown = true;
        if (wp == WM_LBUTTONUP) g_leftButtonDown = false;
        if (wp == WM_RBUTTONUP) g_rightButtonDown = false;

        MouseButton button;
        if (decodeButton(wp, *event, button)) {
            recordClick(event->pt, button);
        }

        if (g_leftButtonDown && g_rightButtonDown && !g_exitChordTriggered) {
            g_exitChordTriggered = true;
            g_pauseOnExit = true;
            closeAfterForwarding = true;
            std::printf("left + right pressed: exiting after the real click.\n");
            std::fflush(stdout);
        }
    }

    // Observation only: never return a non-zero value and never alter the
    // event. The click continues to the real window below the overlay.
    const LRESULT result = CallNextHookEx(g_mouseHook, code, wp, lp);
    if (closeAfterForwarding && g_window) {
        PostMessageW(g_window, WM_CLOSE, 0, 0);
    }
    return result;
}

void drawClick(HDC hdc, const ClickRecord& click, bool newest) {
    const int x = click.screen.x - g_virtualX;
    const int y = click.screen.y - g_virtualY;
    const int radius = newest ? 11 : 8;
    const COLORREF color = newest ? RGB(80, 255, 120)
                                  : buttonColor(click.button);

    HPEN pen = CreatePen(PS_SOLID, newest ? 3 : 2, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, x - radius, y - radius, x + radius + 1, y + radius + 1);
    MoveToEx(hdc, x - radius - 4, y, nullptr);
    LineTo(hdc, x + radius + 5, y);
    MoveToEx(hdc, x, y - radius - 4, nullptr);
    LineTo(hdc, x, y + radius + 5);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    wchar_t label[96];
    std::swprintf(label, 96, L"#%llu %ls (%ld, %ld)",
                  static_cast<unsigned long long>(click.number),
                  buttonLabel(click.button), click.screen.x, click.screen.y);
    SetTextColor(hdc, color);
    RECT textRect{x + 14, y - 18, x + 270, y + 22};
    DrawTextW(hdc, label, -1, &textRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOCLIP);
}

void drawScene(HDC hdc) {
    RECT screenRect{0, 0, g_virtualW, g_virtualH};
    HBRUSH veil = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &screenRect, veil);
    DeleteObject(veil);

    SetBkMode(hdc, TRANSPARENT);
    HFONT font = CreateFontW(
        -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(hdc, font);

    SetTextColor(hdc, RGB(255, 255, 255));
    wchar_t status[160];
    std::swprintf(status, 160,
                  L"屏幕选点  |  已记录: %llu  |  同时按下左右键退出并 pause",
                  static_cast<unsigned long long>(g_clickCount));
    RECT statusRect{14, 9, g_virtualW - 14, 40};
    DrawTextW(hdc, status, -1, &statusRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOCLIP);

    for (std::size_t i = 0; i < g_clicks.size(); ++i) {
        drawClick(hdc, g_clicks[i], i + 1 == g_clicks.size());
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC target = BeginPaint(hwnd, &paint);
            HDC buffer = CreateCompatibleDC(target);
            HBITMAP bitmap = CreateCompatibleBitmap(target, g_virtualW, g_virtualH);
            HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
            drawScene(buffer);
            BitBlt(target, 0, 0, g_virtualW, g_virtualH, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(buffer);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_TIMER:
            if (wp == kAutoExitTimer) DestroyWindow(hwnd);
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            refreshOverlayBounds(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_window = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wp, lp);
    }
}

BOOL WINAPI consoleControlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        HWND window = g_window;
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        return TRUE;
    }
    return FALSE;
}

bool parseIntOption(const char* argument, const char* prefix, int& value) {
    const std::size_t prefixLength = std::strlen(prefix);
    if (std::strncmp(argument, prefix, prefixLength) != 0) return false;
    char* end = nullptr;
    const long parsed = std::strtol(argument + prefixLength, &end, 10);
    if (!end || *end != '\0') return false;
    value = static_cast<int>(parsed);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const DpiMode dpiMode = enableBestDpiAwareness();
    if (dpiMode < DpiMode::PerMonitor) {
        std::fprintf(stderr,
                     "per-monitor DPI awareness is required; current mode: %s\n",
                     dpiModeName(dpiMode));
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);

    int alpha = 96;
    int autoExitMs = 0;
    for (int i = 1; i < argc; ++i) {
        if (parseIntOption(argv[i], "--alpha=", alpha)) continue;
        if (parseIntOption(argv[i], "--auto-exit-ms=", autoExitMs)) continue;
        std::fprintf(stderr,
                     "usage: screen_point_picker [--alpha=0..255] "
                     "[--auto-exit-ms=N]\n");
        return 2;
    }
    alpha = std::clamp(alpha, 0, 255);
    autoExitMs = std::max(autoExitMs, 0);

    readVirtualScreenBounds();

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass)) {
        std::fprintf(stderr, "failed to register overlay window (error %lu)\n",
                     GetLastError());
        return 1;
    }

    g_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT |
            WS_EX_NOACTIVATE,
        kWindowClass, L"screen_point_picker", WS_POPUP, g_virtualX, g_virtualY,
        g_virtualW, g_virtualH, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        std::fprintf(stderr, "failed to create overlay window (error %lu)\n",
                     GetLastError());
        UnregisterClassW(kWindowClass, instance);
        return 1;
    }

    SetLayeredWindowAttributes(g_window, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
    SetWindowPos(g_window, HWND_TOPMOST, g_virtualX, g_virtualY, g_virtualW,
                 g_virtualH, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (!validatePhysicalCoordinateSpace(g_window)) {
        std::fprintf(stderr,
                     "DPI coordinate validation failed; refusing to record "
                     "misaligned points.\n");
        DestroyWindow(g_window);
        UnregisterClassW(kWindowClass, instance);
        return 1;
    }

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, instance, 0);
    if (!g_mouseHook) {
        std::fprintf(stderr, "failed to install mouse hook (error %lu)\n",
                     GetLastError());
        DestroyWindow(g_window);
        UnregisterClassW(kWindowClass, instance);
        return 1;
    }

    SetConsoleCtrlHandler(consoleControlHandler, TRUE);
    if (autoExitMs > 0) {
        SetTimer(g_window, kAutoExitTimer, static_cast<UINT>(autoExitMs), nullptr);
    }

    std::printf("screen point picker started; clicks pass through normally.\n");
    const UINT windowDpi = getWindowDpi(g_window);
    std::printf("DPI awareness: %s; window DPI: %u (%u%%); "
                "virtual screen: %d,%d %dx%d.\n",
                dpiModeName(dpiMode), windowDpi,
                static_cast<unsigned>((windowDpi * 100u + 48u) / 96u),
                g_virtualX, g_virtualY,
                g_virtualW, g_virtualH);
    std::printf("press left + right together to exit and pause; "
                "Ctrl+C exits without pause.\n");

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    SetConsoleCtrlHandler(consoleControlHandler, FALSE);
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    if (g_window) DestroyWindow(g_window);
    UnregisterClassW(kWindowClass, instance);
    if (g_pauseOnExit) {
        std::printf("screen point picker exited; click records are shown above.\n");
        std::fflush(stdout);
        std::system("pause");
    }
    return 0;
}
