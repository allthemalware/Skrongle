// Renders the GUI off-screen for many frames with simulated mouse activity,
// checks the framebuffer stays sane. Links against gui_core.o directly.
#include "../src/gui/gui.hpp"
#include "../src/params.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct FakeHost : ParamHost {
    double vals[P_COUNT];
    int frame = 0;
    FakeHost() { for (int i = 0; i < P_COUNT; i++) vals[i] = paramDef(i)->defv; }
    double getParam(uint32_t id) override { return id < P_COUNT ? vals[id] : 0; }
    void setParamFromGui(uint32_t id, double v, int kind) override {
        if (kind == 1 && id < P_COUNT) vals[id] = v;
    }
    float uiGlitchLevel() override { return (frame / 40) % 2 ? 0.9f : 0.05f; }
    int   uiStepIndex()   override { return (frame / 8) % 16; }
    int   uiLastMode()    override { return frame % 3; }
    bool  takeDirty()     override { return frame % 50 == 0; }
};

int main() {
    FakeHost host;
    GuiCore *core = coreCreate(&host);
    if (!core) { fprintf(stderr, "coreCreate failed\n"); return 1; }

    unsigned long long checksum = 0;
    for (int f = 0; f < 300; f++) {
        host.frame = f;
        // poke the UI: drag a knob, paint steps, scroll, click around
        if (f == 10)  coreMouseDown(core, 80, 300, 1, false);   // somewhere in ENGINE
        if (f > 10 && f < 30) coreMouseMove(core, 80, 300 - (f - 10) * 3);
        if (f == 30)  coreMouseUp(core, 80, 240);
        if (f == 40)  coreMouseDown(core, 400, 150, 1, false);  // step bars
        if (f > 40 && f < 60) coreMouseMove(core, 400 + (f - 40) * 20, 130);
        if (f == 60)  coreMouseUp(core, 800, 130);
        if (f == 70)  coreWheel(core, 80, 300, 1.0f);
        if (f == 80)  coreDoubleClick(core, 80, 300);
        if (f == 90)  coreMouseDown(core, 5000, -50, 1, false); // out of bounds
        if (f == 91)  coreMouseUp(core, -10, 9999);
        if (f == 100) coreMouseDown(core, 460, 540, 1, false);  // PANIC area
        if (f == 120) coreMouseUp(core, 460, 540);

        const uint32_t *fb = coreRender(core);
        if (!fb) { fprintf(stderr, "coreRender returned null at frame %d\n", f); return 1; }
        // sample sparse pixels; all must have opaque alpha
        for (int i = 0; i < GUI_W * GUI_H; i += 9973) {
            if ((fb[i] >> 24) != 0xFF) {
                fprintf(stderr, "non-opaque pixel at frame %d idx %d: %08x\n", f, i, fb[i]);
                return 1;
            }
            checksum += fb[i] & 0xFFFFFF;
        }
    }
    printf("rendered 300 frames, checksum %llx\nGUI RENDER TEST PASSED\n", checksum);
    coreDestroy(core);
    return 0;
}
