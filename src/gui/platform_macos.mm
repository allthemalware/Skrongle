// Skrongle — macOS Cocoa platform backend (compile as Objective-C++).
#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#include "gui.hpp"
#include "../../clap/include/clap/clap.h"
#include <cstring>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>

struct GuiPlatform;

@interface SWView : NSView {
@public
    GuiPlatform *plat;
}
@end

struct GuiPlatform {
    ParamHost *host = nullptr;
    GuiCore   *core = nullptr;
    SWView    *view = nil;
    uint32_t  *fbCopy = nullptr;
    bool       visible = false;

    // Fallback ~30 Hz driver (used only when the host has no timer-support).
    // The shared flag lets a still-queued main-queue block bail out safely if
    // the GUI is torn down before it runs.
    std::thread                        fbThread;
    std::shared_ptr<std::atomic<bool>> fbAlive;
};

@implementation SWView

- (BOOL)isFlipped { return YES; }  // y-down to match framebuffer
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

- (void)mousePoint:(NSEvent *)e x:(int *)x y:(int *)y {
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    *x = (int)p.x; *y = (int)p.y;
}

- (void)mouseDown:(NSEvent *)e {
    int x, y; [self mousePoint:e x:&x y:&y];
    bool shift = ([e modifierFlags] & NSEventModifierFlagShift) != 0;
    if ([e clickCount] == 2) coreDoubleClick(plat->core, x, y);
    else                     coreMouseDown(plat->core, x, y, 1, shift);
}
- (void)rightMouseDown:(NSEvent *)e {
    int x, y; [self mousePoint:e x:&x y:&y];
    bool shift = ([e modifierFlags] & NSEventModifierFlagShift) != 0;
    coreMouseDown(plat->core, x, y, 3, shift);
}
- (void)mouseDragged:(NSEvent *)e {
    int x, y; [self mousePoint:e x:&x y:&y];
    coreMouseMove(plat->core, x, y);
}
- (void)mouseMoved:(NSEvent *)e {
    int x, y; [self mousePoint:e x:&x y:&y];
    coreMouseMove(plat->core, x, y);
}
- (void)mouseUp:(NSEvent *)e {
    int x, y; [self mousePoint:e x:&x y:&y];
    coreMouseUp(plat->core, x, y);
}
- (void)rightMouseUp:(NSEvent *)e {
    int x, y; [self mousePoint:e x:&x y:&y];
    coreMouseUp(plat->core, x, y);
}
- (void)scrollWheel:(NSEvent *)e {
    int x, y; [self mousePoint:e x:&x y:&y];
    float dy = (float)[e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) dy *= 0.05f;
    coreWheel(plat->core, x, y, dy);
}

- (void)drawRect:(NSRect)dirty {
    if (!plat || !plat->core) return;
    const uint32_t *fb = coreRender(plat->core);
    memcpy(plat->fbCopy, fb, (size_t)GUI_W * GUI_H * 4);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bm = CGBitmapContextCreate(plat->fbCopy, GUI_W, GUI_H, 8,
                                            GUI_W * 4, cs,
                                            kCGImageAlphaNoneSkipFirst |
                                            kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(bm);

    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    // view is flipped; un-flip for the image draw so it isn't mirrored
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, GUI_H);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, GUI_W, GUI_H), img);
    CGContextRestoreGState(ctx);

    CGImageRelease(img);
    CGContextRelease(bm);
    CGColorSpaceRelease(cs);
}
@end

GuiPlatform *guiCreatePlatform(ParamHost *host) {
    GuiPlatform *g = new GuiPlatform();
    g->host = host;
    g->core = coreCreate(host);
    g->fbCopy = (uint32_t *)malloc((size_t)GUI_W * GUI_H * 4);
    return g;
}

void guiDestroy(GuiPlatform *g) {
    if (!g) return;
    @autoreleasepool {
        if (g->view) { [g->view removeFromSuperview]; g->view = nil; }
    }
    free(g->fbCopy);
    coreDestroy(g->core);
    delete g;
}

bool guiAttach(GuiPlatform *g, const clap_window *parent) {
    if (!g || !parent) return false;
    @autoreleasepool {
        NSView *parentView = (__bridge NSView *)parent->cocoa;
        if (!parentView) return false;
        SWView *v = [[SWView alloc] initWithFrame:NSMakeRect(0, 0, GUI_W, GUI_H)];
        v->plat = g;
        [parentView addSubview:v];
        g->view = v;
        g->visible = true;

        NSTrackingArea *ta = [[NSTrackingArea alloc]
            initWithRect:NSMakeRect(0, 0, GUI_W, GUI_H)
                 options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                          NSTrackingInVisibleRect)
                   owner:v userInfo:nil];
        [v addTrackingArea:ta];
    }
    return true;
}

bool guiSetVisible(GuiPlatform *g, bool visible) {
    if (!g || !g->view) return false;
    @autoreleasepool { [g->view setHidden:!visible]; }
    g->visible = visible;
    return true;
}

void guiTick(GuiPlatform *g) {
    if (!g || !g->view || !g->visible) return;
    @autoreleasepool { [g->view setNeedsDisplay:YES]; }
}

void guiStartFallbackTimer(GuiPlatform *g) {
    if (!g || (g->fbAlive && g->fbAlive->load())) return;
    g->fbAlive = std::make_shared<std::atomic<bool>>(true);
    auto alive = g->fbAlive;
    GuiPlatform *gp = g;
    g->fbThread = std::thread([alive, gp] {
        while (alive->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            if (!alive->load()) break;
            // AppKit must be touched on the main thread; hop there. The block
            // re-checks `alive`, so it can't touch a torn-down GUI.
            dispatch_async(dispatch_get_main_queue(), ^{
                if (alive->load()) guiTick(gp);
            });
        }
    });
}

void guiStopFallbackTimer(GuiPlatform *g) {
    if (!g) return;
    if (g->fbAlive) g->fbAlive->store(false);
    if (g->fbThread.joinable()) g->fbThread.join();
}

#endif // __APPLE__
