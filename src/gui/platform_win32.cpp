// Skrongle — Win32 platform backend.
#if defined(_WIN32)

#include "gui.hpp"
#include "../../clap/include/clap/clap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

static const wchar_t *kClassName = L"SkrongleGuiClass";
static bool s_classRegistered = false;

struct GuiPlatform {
    ParamHost *host = nullptr;
    GuiCore   *core = nullptr;
    HWND       hwnd = nullptr;
    bool       visible = false;
    BITMAPINFO bmi{};

    // Fallback ~30 Hz driver (used only when the host has no timer-support).
    std::thread       fbThread;
    std::atomic<bool> fbRun{false};
};

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    GuiPlatform *g = (GuiPlatform *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!g) return DefWindowProcW(hwnd, msg, wp, lp);

    const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        coreMouseDown(g->core, x, y, 1, shift);
        return 0;
    case WM_LBUTTONDBLCLK:
        coreDoubleClick(g->core, x, y);
        return 0;
    case WM_RBUTTONDOWN:
        coreMouseDown(g->core, x, y, 3, shift);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        ReleaseCapture();
        coreMouseUp(g->core, x, y);
        return 0;
    case WM_MOUSEMOVE:
        coreMouseMove(g->core, x, y);
        return 0;
    case WM_MOUSEWHEEL: {
        // wheel coords are screen-relative; convert
        POINT pt{ x, y };
        ScreenToClient(hwnd, &pt);
        float dy = (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        coreWheel(g->core, pt.x, pt.y, dy);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const uint32_t *fb = coreRender(g->core);
        StretchDIBits(dc, 0, 0, GUI_W, GUI_H, 0, 0, GUI_W, GUI_H,
                      fb, &g->bmi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // we repaint everything
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

GuiPlatform *guiCreatePlatform(ParamHost *host) {
    GuiPlatform *g = new GuiPlatform();
    g->host = host;
    g->core = coreCreate(host);

    g->bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    g->bmi.bmiHeader.biWidth       = GUI_W;
    g->bmi.bmiHeader.biHeight      = -GUI_H;  // top-down
    g->bmi.bmiHeader.biPlanes      = 1;
    g->bmi.bmiHeader.biBitCount    = 32;
    g->bmi.bmiHeader.biCompression = BI_RGB;

    if (!s_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = wndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        s_classRegistered = true;
    }
    return g;
}

void guiDestroy(GuiPlatform *g) {
    if (!g) return;
    if (g->hwnd) DestroyWindow(g->hwnd);
    coreDestroy(g->core);
    delete g;
}

bool guiAttach(GuiPlatform *g, const clap_window *parent) {
    if (!g || !parent) return false;
    HWND parentHwnd = (HWND)parent->win32;
    g->hwnd = CreateWindowExW(0, kClassName, L"Skrongle",
                              WS_CHILD | WS_VISIBLE, 0, 0, GUI_W, GUI_H,
                              parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g->hwnd) return false;
    SetWindowLongPtrW(g->hwnd, GWLP_USERDATA, (LONG_PTR)g);
    g->visible = true;
    return true;
}

bool guiSetVisible(GuiPlatform *g, bool visible) {
    if (!g || !g->hwnd) return false;
    ShowWindow(g->hwnd, visible ? SW_SHOW : SW_HIDE);
    g->visible = visible;
    return true;
}

void guiTick(GuiPlatform *g) {
    if (!g || !g->hwnd || !g->visible) return;
    InvalidateRect(g->hwnd, nullptr, FALSE); // WM_PAINT renders + blits
}

void guiStartFallbackTimer(GuiPlatform *g) {
    if (!g || g->fbRun.load()) return;
    g->fbRun.store(true);
    g->fbThread = std::thread([g] {
        while (g->fbRun.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            if (g->fbRun.load()) guiTick(g); // InvalidateRect is thread-safe
        }
    });
}

void guiStopFallbackTimer(GuiPlatform *g) {
    if (!g) return;
    g->fbRun.store(false);
    if (g->fbThread.joinable()) g->fbThread.join();
}

#endif // _WIN32
