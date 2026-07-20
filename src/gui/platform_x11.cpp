// Skrongle — X11 platform backend.
// Owns its own Display connection, creates a child window of the host-provided
// parent XID, pumps events + renders + blits on guiTick (driven by host timer).
#if !defined(_WIN32) && !defined(__APPLE__)

#include "gui.hpp"
#include "../../clap/include/clap/clap.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>

struct GuiPlatform {
    ParamHost *host = nullptr;
    GuiCore   *core = nullptr;

    Display *dpy = nullptr;
    Window   win = 0;
    GC       gc  = 0;
    XImage  *img = nullptr;
    uint32_t *blitBuf = nullptr;   // owned by us, wrapped by img
    bool visible = false;
    Time lastClick = 0;
    int  lastCx = -1, lastCy = -1;

    // Fallback ~30 Hz driver (used only when the host has no timer-support).
    std::thread       fbThread;
    std::atomic<bool> fbRun{false};
};

GuiPlatform *guiCreatePlatform(ParamHost *host) {
    // Xlib must be told about threads before its first use, so the fallback
    // timer thread can share the Display with the host thread safely. Runs once.
    static bool xthreads = [] { XInitThreads(); return true; }();
    (void)xthreads;
    GuiPlatform *g = new GuiPlatform();
    g->host = host;
    g->core = coreCreate(host);
    return g;
}

void guiDestroy(GuiPlatform *g) {
    if (!g) return;
    if (g->img) { g->img->data = nullptr; XDestroyImage(g->img); }  // we free blitBuf ourselves
    free(g->blitBuf);
    if (g->gc && g->dpy)  XFreeGC(g->dpy, g->gc);
    if (g->win && g->dpy) XDestroyWindow(g->dpy, g->win);
    if (g->dpy) XCloseDisplay(g->dpy);
    coreDestroy(g->core);
    delete g;
}

bool guiAttach(GuiPlatform *g, const clap_window *parent) {
    if (!g || !parent) return false;
    g->dpy = XOpenDisplay(nullptr);
    if (!g->dpy) return false;

    Window parentWin = (Window)parent->x11;
    int screen = DefaultScreen(g->dpy);

    g->win = XCreateSimpleWindow(g->dpy, parentWin, 0, 0, GUI_W, GUI_H, 0,
                                 BlackPixel(g->dpy, screen),
                                 BlackPixel(g->dpy, screen));
    XSelectInput(g->dpy, g->win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask);
    g->gc = XCreateGC(g->dpy, g->win, 0, nullptr);

    // 32-bit ARGB framebuffer wrapped in an XImage. On a 24-depth visual the
    // alpha byte is ignored, which is fine — we render opaque.
    g->blitBuf = (uint32_t *)malloc((size_t)GUI_W * GUI_H * 4);
    g->img = XCreateImage(g->dpy, DefaultVisual(g->dpy, screen),
                          (unsigned)DefaultDepth(g->dpy, screen), ZPixmap, 0,
                          (char *)g->blitBuf, GUI_W, GUI_H, 32, GUI_W * 4);
    if (!g->img) return false;

    XMapWindow(g->dpy, g->win);
    XFlush(g->dpy);
    g->visible = true;
    return true;
}

bool guiSetVisible(GuiPlatform *g, bool visible) {
    if (!g || !g->dpy || !g->win) return false;
    if (visible) XMapWindow(g->dpy, g->win);
    else         XUnmapWindow(g->dpy, g->win);
    XFlush(g->dpy);
    g->visible = visible;
    return true;
}

static void pumpEvents(GuiPlatform *g) {
    while (XPending(g->dpy)) {
        XEvent ev;
        XNextEvent(g->dpy, &ev);
        switch (ev.type) {
        case ButtonPress: {
            XButtonEvent &b = ev.xbutton;
            bool shift = (b.state & ShiftMask) != 0;
            if (b.button == Button1) {
                // double-click detection: <350ms and within 4px of previous
                bool dbl = (b.time - g->lastClick) < 350 &&
                           abs(b.x - g->lastCx) < 4 && abs(b.y - g->lastCy) < 4;
                g->lastClick = b.time; g->lastCx = b.x; g->lastCy = b.y;
                if (dbl) coreDoubleClick(g->core, b.x, b.y);
                else     coreMouseDown(g->core, b.x, b.y, 1, shift);
            } else if (b.button == Button3) {
                coreMouseDown(g->core, b.x, b.y, 3, shift);
            } else if (b.button == Button4) {
                coreWheel(g->core, b.x, b.y, 1.0f);
            } else if (b.button == Button5) {
                coreWheel(g->core, b.x, b.y, -1.0f);
            }
            break;
        }
        case ButtonRelease:
            if (ev.xbutton.button == Button1 || ev.xbutton.button == Button3)
                coreMouseUp(g->core, ev.xbutton.x, ev.xbutton.y);
            break;
        case MotionNotify:
            // compress motion events: keep only the latest
            while (XPending(g->dpy)) {
                XEvent peek;
                XPeekEvent(g->dpy, &peek);
                if (peek.type != MotionNotify) break;
                XNextEvent(g->dpy, &ev);
            }
            coreMouseMove(g->core, ev.xmotion.x, ev.xmotion.y);
            break;
        default: break;
        }
    }
}

void guiTick(GuiPlatform *g) {
    if (!g || !g->dpy || !g->visible) return;
    pumpEvents(g);
    const uint32_t *fb = coreRender(g->core);
    memcpy(g->blitBuf, fb, (size_t)GUI_W * GUI_H * 4);
    XPutImage(g->dpy, g->win, g->gc, g->img, 0, 0, 0, 0, GUI_W, GUI_H);
    XFlush(g->dpy);
}

void guiStartFallbackTimer(GuiPlatform *g) {
    if (!g || g->fbRun.load()) return;
    g->fbRun.store(true);
    g->fbThread = std::thread([g] {
        while (g->fbRun.load()) {
            // Sleep first: guiAttach (which sets dpy/visible) runs after
            // guiCreate on the host thread, and guiTick no-ops until then.
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            if (g->fbRun.load()) guiTick(g);
        }
    });
}

void guiStopFallbackTimer(GuiPlatform *g) {
    if (!g) return;
    g->fbRun.store(false);
    if (g->fbThread.joinable()) g->fbThread.join();
}

#endif // linux/x11
