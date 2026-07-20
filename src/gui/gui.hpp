// Skrongle — GUI interface (platform-agnostic side)
#pragma once
#include <cstdint>

enum { GUI_W = 1040, GUI_H = 640 };

#if defined(_WIN32)
  #define GUI_NATIVE_API "win32"
#elif defined(__APPLE__)
  #define GUI_NATIVE_API "cocoa"
#else
  #define GUI_NATIVE_API "x11"
#endif

struct clap_window;

// What the GUI needs from the plugin. kind: 0=gesture begin, 1=value, 2=end
struct ParamHost {
    virtual double getParam(uint32_t id) = 0;
    virtual void   setParamFromGui(uint32_t id, double v, int kind) = 0;
    virtual float  uiGlitchLevel() = 0;   // live glitch envelope from audio
    virtual int    uiStepIndex()   = 0;   // current sequencer step
    virtual int    uiLastMode()    = 0;   // last glitch mode (freeze/rev/stop)
    virtual bool   takeDirty()     = 0;   // host changed a param since last ask
    virtual ~ParamHost() {}
};

struct GuiPlatform;                       // opaque, one per backend
GuiPlatform *guiCreatePlatform(ParamHost *host);
void guiDestroy(GuiPlatform *g);
bool guiAttach(GuiPlatform *g, const clap_window *parent);
bool guiSetVisible(GuiPlatform *g, bool visible);
void guiTick(GuiPlatform *g);             // ~30 Hz: pump input, render, blit
// Internal ~30 Hz driver used only when the host offers no timer-support, so the
// GUI still animates. No-ops when the host timer is driving guiTick.
void guiStartFallbackTimer(GuiPlatform *g);
void guiStopFallbackTimer(GuiPlatform *g);

// Shared software renderer (implemented in gui_core.cpp)
struct GuiCore;
GuiCore *coreCreate(ParamHost *host);
void coreDestroy(GuiCore *c);
const uint32_t *coreRender(GuiCore *c);   // returns ARGB32 framebuffer
void coreMouseDown(GuiCore *c, int x, int y, int button, bool shift);
void coreMouseMove(GuiCore *c, int x, int y);
void coreMouseUp(GuiCore *c, int x, int y);
void coreWheel(GuiCore *c, int x, int y, float dy);
void coreDoubleClick(GuiCore *c, int x, int y);
