// Skrongle — GUI core.
//
// A software-rendered "broken screen": the wallpaper layer gets live
// pixel-sorting melts and block displacements (they intensify when the audio
// engine actually glitches), the control layer is drawn on top at full
// contrast, then a thin glass-damage pass (cracks, stuck pixels, scanlines,
// short band tears) sits over everything — deliberately kept light where it
// matters so every label and value stays readable.
#include "gui.hpp"
#include "../params.hpp"
#include "font8x8_basic.h"
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cctype>

// ------------------------------------------------------------- palette
static const uint32_t C_BG      = 0xFF0A0D12;
static const uint32_t C_PANEL   = 0xFF12181F;
static const uint32_t C_PANEL2  = 0xFF161E27;
static const uint32_t C_BORDER  = 0xFF2A3542;
static const uint32_t C_TEXT    = 0xFFD9E7E2;
static const uint32_t C_DIM     = 0xFF6E8190;
static const uint32_t C_CYAN    = 0xFF2EE6FF;
static const uint32_t C_MAGENTA = 0xFFFF3DA6;
static const uint32_t C_AMBER   = 0xFFFFB300;
static const uint32_t C_DANGER  = 0xFFFF4444;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline float fclmp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline double dclamp(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }

struct FxRng {
    uint64_t s = 0xBADC0DE5;
    uint64_t n64() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    float uni() { return (float)((n64() >> 11) * (1.0 / 9007199254740992.0)); }
    int   ri(int lo, int hi) { return lo + (int)(uni() * (hi - lo + 1)); }
};
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

// ------------------------------------------------------------- widgets
enum WType { W_KNOB, W_TOGGLE, W_COMBO, W_STEPS, W_HOLD, W_ACTION };
enum WAction { A_DICE = 1, A_CLEAR = 2 };
struct Widget {
    int type; uint32_t pid; int x, y, w, h;
    const char *label; int action; bool big;
};
struct Seg { float x0, y0, x1, y1; uint8_t a; };

struct GuiCore {
    ParamHost *host = nullptr;
    std::vector<uint32_t> fb, wall;
    std::vector<Widget> ws;
    std::vector<Seg> cracks;
    struct DeadPx { int x, y; uint32_t c; };
    std::vector<DeadPx> deadpx;

    uint32_t frame = 0;
    FxRng rng;
    float glitchSm = 0;
    float scope[160] = {0};
    int scopeHead = 0;

    // interaction
    int hoverW = -1, dragW = -1, dragStepIx = -1;
    int lastMx = 0, lastMy = 0;
    double dragStart = 0; bool dragShift = false;
    char readout[96] = "TOUCH A CONTROL";
    uint32_t readoutAge = 0;

    // band-tear state
    int tearY = 0, tearH = 0, tearDx = 0, tearLife = 0;
};

// ------------------------------------------------------- draw helpers
static inline void px(GuiCore *c, int x, int y, uint32_t col) {
    if ((unsigned)x < GUI_W && (unsigned)y < GUI_H) c->fb[y * GUI_W + x] = col;
}
static inline void pxA(GuiCore *c, int x, int y, uint32_t col, int a) {
    if ((unsigned)x >= GUI_W || (unsigned)y >= GUI_H) return;
    uint32_t &d = c->fb[y * GUI_W + x];
    uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    uint32_t sr = (col >> 16) & 255, sg = (col >> 8) & 255, sb = col & 255;
    d = 0xFF000000 | ((dr + (sr - dr) * a / 255) << 16)
                   | ((dg + (sg - dg) * a / 255) << 8)
                   |  (db + (sb - db) * a / 255);
}
static void fillRect(GuiCore *c, int x, int y, int w, int h, uint32_t col) {
    int x1 = imax(0, x), y1 = imax(0, y), x2 = imin(GUI_W, x + w), y2 = imin(GUI_H, y + h);
    for (int j = y1; j < y2; j++) {
        uint32_t *row = &c->fb[j * GUI_W];
        for (int i = x1; i < x2; i++) row[i] = col;
    }
}
static void blendRect(GuiCore *c, int x, int y, int w, int h, uint32_t col, int a) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) pxA(c, i, j, col, a);
}
static void frameRect(GuiCore *c, int x, int y, int w, int h, uint32_t col) {
    fillRect(c, x, y, w, 1, col); fillRect(c, x, y + h - 1, w, 1, col);
    fillRect(c, x, y, 1, h, col); fillRect(c, x + w - 1, y, 1, h, col);
}
static void lineA(GuiCore *c, float x0, float y0, float x1, float y1,
                  uint32_t col, int a) {
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)std::fmax(std::fabs(dx), std::fabs(dy)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        pxA(c, (int)(x0 + dx * t), (int)(y0 + dy * t), col, a);
    }
}
// bitmap text (public-domain font8x8)
static void text(GuiCore *c, const char *s, int x, int y, int scale,
                 uint32_t col, int alpha = 255) {
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 128) {
            const unsigned char *g = font8x8_basic[ch];
            for (int r = 0; r < 8; r++)
                for (int b = 0; b < 8; b++)
                    if (g[r] & (1 << b))
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                pxA(c, x + b * scale + sx, y + r * scale + sy, col, alpha);
        }
        x += 8 * scale;
    }
}
static int textW(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }
static void textC(GuiCore *c, const char *s, int cx, int y, int scale,
                  uint32_t col, int alpha = 255) {
    text(c, s, cx - textW(s, scale) / 2, y, scale, col, alpha);
}

// ----------------------------------------------------- value mapping
static bool isLog(uint32_t id) {
    return id == P_CUTOFF || id == P_PITCH_RATE || id == P_FILT_RATE;
}
static double toNorm(uint32_t id, double v) {
    const ParamDef *d = paramDef(id);
    if (isLog(id))
        return (std::log(v) - std::log(d->minv)) / (std::log(d->maxv) - std::log(d->minv));
    return (v - d->minv) / (d->maxv - d->minv);
}
static double fromNorm(uint32_t id, double n) {
    const ParamDef *d = paramDef(id);
    n = fclmp((float)n, 0.f, 1.f);
    double v = isLog(id)
        ? std::exp(std::log(d->minv) + n * (std::log(d->maxv) - std::log(d->minv)))
        : d->minv + n * (d->maxv - d->minv);
    if (d->stepped) v = std::floor(v + 0.5);
    return v;
}

// ----------------------------------------------------------- layout
static void addKnob(GuiCore *c, uint32_t pid, int x, int y, const char *l,
                    bool big = false) {
    c->ws.push_back({W_KNOB, pid, x, y, big ? 96 : 78, big ? 102 : 84, l, 0, big});
}
static void buildLayout(GuiCore *c) {
    // sequencer strip
    c->ws.push_back({W_COMBO,  P_RATE,  26, 102, 96, 26, "RATE", 0, false});
    c->ws.push_back({W_COMBO,  P_STEPS, 26, 152, 96, 26, "STEPS", 0, false});
    addKnob(c, P_SWING, 134, 92, "SWING");
    c->ws.push_back({W_ACTION, 0, 134, 182, 44, 20, "DICE",  A_DICE,  false});
    c->ws.push_back({W_ACTION, 0, 184, 182, 52, 20, "CLEAR", A_CLEAR, false});
    c->ws.push_back({W_STEPS,  P_STEP_0, 252, 84, 770, 124, "", 0, false});

    int y1 = 252;   // middle row of panels
    // ENGINE
    addKnob(c, P_DELAY_MS,   28, y1, "TIME");
    addKnob(c, P_FEEDBACK,  112, y1, "FEEDBK");
    addKnob(c, P_FREEZE_FB, 196, y1, "FREEZE");
    addKnob(c, P_PINGPONG,  280, y1, "PINGPONG");
    addKnob(c, P_GLITCH_WET, 28, y1 + 92, "GLT BLEND");
    addKnob(c, P_BASE_WET,  112, y1 + 92, "IDLE BLEND");
    c->ws.push_back({W_TOGGLE, P_KILL_INPUT, 206, y1 + 116, 64, 26, "KILL IN", 0, false});

    // PITCH
    addKnob(c, P_PITCH_DEPTH, 404, y1, "DEPTH");
    addKnob(c, P_PITCH_RATE,  488, y1, "RATE");
    addKnob(c, P_PITCH_SMOOTH,446, y1 + 92, "SMOOTH");

    // FILTER
    c->ws.push_back({W_TOGGLE, P_FILTER_ON,  618, y1 + 8, 52, 26, "ON", 0, false});
    c->ws.push_back({W_COMBO,  P_FILTER_TYPE,618, y1 + 56, 96, 26, "TYPE", 0, false});
    addKnob(c, P_CUTOFF, 726, y1, "CUTOFF");
    addKnob(c, P_RESO,   810, y1, "RESO");
    addKnob(c, P_FILT_MOD,  726, y1 + 92, "MOD");
    addKnob(c, P_FILT_RATE, 810, y1 + 92, "RATE");
    c->ws.push_back({W_TOGGLE, P_FILT_STEREO, 618, y1 + 140, 96, 26, "ST INVERT", 0, false});

    // bottom row
    int y2 = 478;
    c->ws.push_back({W_TOGGLE, P_CRUSH_ON, 28, y2 + 22, 52, 26, "ON", 0, false});
    addKnob(c, P_BITS,        92, y2, "BITS");
    addKnob(c, P_DOWNSAMPLE, 176, y2, "DOWNSMP");
    c->ws.push_back({W_TOGGLE, 0xFFFF, 0, 0, 0, 0, "", 0, false}); // spacer (unused)
    c->ws.pop_back();
    addKnob(c, P_CRUSH_MIX,  28, y2 + 60, "MIX");

    addKnob(c, P_REVERSE_CH,  306, y2, "REVERSE");
    addKnob(c, P_TAPESTOP_CH, 390, y2, "TAPESTOP");
    addKnob(c, P_RATCHET_CH,  474, y2, "RATCHET");
    addKnob(c, P_CHAOS,       306, y2 + 56, "CHAOS", true);
    c->ws.push_back({W_HOLD, P_FORCE, 420, y2 + 86, 130, 48, "PANIC", 0, true});

    addKnob(c, P_DRIVE,    600, y2, "DRIVE");
    addKnob(c, P_DUCK_DRY, 684, y2, "DUCK DRY");
    addKnob(c, P_GAIN_DB,  642, y2 + 60, "GAIN");

    // header: screen damage knob lives top-right
    addKnob(c, P_GUIFX, 944, -6, "DAMAGE");
}

// ------------------------------------------------------- wallpaper
static void buildWallpaper(GuiCore *c) {
    c->wall.resize(GUI_W * GUI_H);
    for (int y = 0; y < GUI_H; y++)
        for (int x = 0; x < GUI_W; x++) {
            float g = 10.f + 14.f * (float)y / GUI_H + 6.f * (float)x / GUI_W;
            uint32_t h = hash32(x * 7349 + y * 911);
            int n = (int)(h & 7);                      // grain
            int r = (int)g - 3 + n, gg = (int)g + n, b = (int)g + 6 + n;
            // faint grid
            if ((x % 40) == 0 || (y % 40) == 0) { r += 5; gg += 7; b += 9; }
            c->wall[y * GUI_W + x] = 0xFF000000 | (r << 16) | (gg << 8) | b;
        }
    // big ghost glyphs + streaks give the pixel sorter something to chew on
    GuiCore tmp = *c; // hack: reuse text() against fb — draw into wall directly
    std::swap(tmp.fb, c->wall);
    text(&tmp, "SK", 660, 20, 18, 0xFF1A2730, 110);
    text(&tmp, "GLITCH", 60, 250, 14, 0xFF18242E, 70);
    text(&tmp, "//", 470, 470, 16, 0xFF1A2730, 80);
    for (int i = 0; i < 90; i++) {
        int y = c->rng.ri(0, GUI_H - 1), x = c->rng.ri(0, GUI_W - 200);
        int len = c->rng.ri(20, 220);
        uint32_t col = (c->rng.uni() < 0.5f) ? 0xFF20303C : 0xFF101820;
        for (int k = 0; k < len; k++) pxA(&tmp, x + k, y, col, 120);
    }
    std::swap(tmp.fb, c->wall);
}

// --------------------------------------------------------- cracks
static void buildCracks(GuiCore *c) {
    float imp[2][2] = {{905, 80}, {120, 588}};
    for (int k = 0; k < 2; k++) {
        float cx = imp[k][0], cy = imp[k][1];
        int spokes = 7 + (int)(c->rng.uni() * 3);
        for (int s = 0; s < spokes; s++) {
            float a = (float)s / spokes * 6.2831f + c->rng.uni() * 0.5f;
            float x = cx, y = cy, len = 60 + c->rng.uni() * 300;
            float seg = 14 + c->rng.uni() * 18, trav = 0;
            while (trav < len) {
                float na = a + (c->rng.uni() - 0.5f) * 0.9f;
                float nx = x + std::cos(na) * seg, ny = y + std::sin(na) * seg;
                uint8_t al = (uint8_t)(58 * (1.f - trav / len) + 12);
                c->cracks.push_back({x, y, nx, ny, al});
                if (c->rng.uni() < 0.18f && trav > 30) {   // branch
                    float ba = na + (c->rng.uni() < 0.5f ? 0.9f : -0.9f);
                    c->cracks.push_back({nx, ny,
                        nx + std::cos(ba) * seg * 1.4f,
                        ny + std::sin(ba) * seg * 1.4f, (uint8_t)(al / 2)});
                }
                x = nx; y = ny; a = na; trav += seg;
            }
        }
        // concentric stress rings near the impact
        for (int r = 10; r < 46; r += 12)
            for (int d = 0; d < 40; d++) {
                float a0 = c->rng.uni() * 6.2831f;
                c->cracks.push_back({cx + std::cos(a0) * r, cy + std::sin(a0) * r,
                                     cx + std::cos(a0 + 0.15f) * r,
                                     cy + std::sin(a0 + 0.15f) * r, 26});
            }
    }
    for (int i = 0; i < 16; i++) {
        uint32_t cols[3] = {C_CYAN, C_MAGENTA, 0xFFFFFFFF};
        c->deadpx.push_back({c->rng.ri(4, GUI_W - 4), c->rng.ri(4, GUI_H - 4),
                             cols[c->rng.ri(0, 2)]});
    }
}

// --------------------------------------------------- glitch FX passes
static inline int lum(uint32_t p) {
    return (((p >> 16) & 255) * 3 + ((p >> 8) & 255) * 6 + (p & 255)) / 10;
}
// classic pixel sorting: sort horizontal runs of the wallpaper by luminance.
static void pixelSortPass(GuiCore *c, float amt) {
    int spans = (int)(amt * 26.f);
    static uint32_t tmp[420];
    for (int s = 0; s < spans; s++) {
        // positions hold for ~7 frames so the melt visibly "drips"
        uint32_t h = hash32((c->frame / 7) * 131 + s * 977);
        int y = (h % GUI_H);
        int x = (hash32(h) % (GUI_W - 64));
        int len = 64 + (int)(hash32(h ^ 0xABCD) % 320);
        len = imin(len, imin(420, GUI_W - x));
        // vertical drips too, every third span
        if (s % 3 == 2) {
            int vlen = imin(len, GUI_H - y);
            for (int k = 0; k < vlen; k++) tmp[k] = c->fb[(y + k) * GUI_W + x];
            std::sort(tmp, tmp + vlen, [](uint32_t a, uint32_t b) {
                return lum(a) > lum(b); });
            for (int k = 0; k < vlen; k++) c->fb[(y + k) * GUI_W + x] = tmp[k];
            continue;
        }
        uint32_t *row = &c->fb[y * GUI_W + x];
        memcpy(tmp, row, len * 4);
        std::sort(tmp, tmp + len, [](uint32_t a, uint32_t b) {
            return lum(a) > lum(b); });
        memcpy(row, tmp, len * 4);
    }
    // displaced blocks with channel swap
    int blocks = (int)(amt * 5.f);
    for (int b = 0; b < blocks; b++) {
        uint32_t h = hash32((c->frame / 9) * 53 + b * 7717);
        int w = 40 + (h % 130), hh = 8 + (hash32(h) % 26);
        int x = hash32(h ^ 1) % (GUI_W - w - 14);
        int y = hash32(h ^ 2) % (GUI_H - hh);
        int dx = 4 + (hash32(h ^ 3) % 12);
        for (int j = 0; j < hh; j++)
            for (int i = w - 1; i >= 0; i--) {
                uint32_t p = c->fb[(y + j) * GUI_W + x + i];
                uint32_t q = 0xFF000000 | ((p & 0xFF) << 16) | (p & 0xFF00)
                           | ((p >> 16) & 0xFF);          // swap R<->B
                c->fb[(y + j) * GUI_W + x + i + dx] = q;
            }
    }
}
// short horizontal band tears over the finished frame (kept subtle)
static void tearPass(GuiCore *c, float amt) {
    if (c->tearLife <= 0) {
        if (c->rng.uni() < amt * 0.10f) {
            c->tearY = c->rng.ri(0, GUI_H - 40);
            c->tearH = c->rng.ri(6, 30);
            c->tearDx = (c->rng.uni() < 0.5f ? -1 : 1) * c->rng.ri(3, (int)(4 + amt * 9));
            c->tearLife = c->rng.ri(2, 5);
        }
        return;
    }
    c->tearLife--;
    static uint32_t tmp[GUI_W];
    for (int j = c->tearY; j < imin(GUI_H, c->tearY + c->tearH); j++) {
        uint32_t *row = &c->fb[j * GUI_W];
        memcpy(tmp, row, sizeof(tmp));
        for (int i = 0; i < GUI_W; i++) {
            int srcI = i - c->tearDx;
            uint32_t p = tmp[imax(0, imin(GUI_W - 1, srcI))];
            // slight channel split inside the tear
            uint32_t p2 = tmp[imax(0, imin(GUI_W - 1, srcI + 2))];
            row[i] = 0xFF000000 | (p & 0x00FF00FF) | (p2 & 0x0000FF00);
        }
    }
}
static void glassPass(GuiCore *c, float amt) {
    // scanlines
    int sa = 8 + (int)(amt * 10);
    for (int y = 0; y < GUI_H; y += 3)
        for (int x = 0; x < GUI_W; x++) pxA(c, x, y, 0xFF000000, sa);
    // cracks + glints
    for (auto &s : c->cracks)
        lineA(c, s.x0, s.y0, s.x1, s.y1, 0xFFC9DEE8,
              (int)(s.a * (0.55f + amt * 0.8f)));
    for (auto &d : c->deadpx) {
        pxA(c, d.x, d.y, d.c, (int)(70 + amt * 90));
        pxA(c, d.x + 1, d.y, d.c, (int)(30 + amt * 50));
    }
    // sparkle static
    int n = (int)(amt * 160);
    for (int i = 0; i < n; i++) {
        uint32_t h = hash32(c->frame * 9176 + i * 31);
        pxA(c, h % GUI_W, hash32(h) % GUI_H, 0xFFFFFFFF, 26);
    }
    // vignette
    for (int y = 0; y < GUI_H; y++) {
        float vy = std::fabs(y - GUI_H * 0.5f) / (GUI_H * 0.5f);
        int a = (int)(vy * vy * 34);
        if (!a) continue;
        for (int x = 0; x < GUI_W; x += 1 + (x % 3)) pxA(c, x, y, 0xFF000000, a);
    }
}

// ------------------------------------------------------ widget drawing
static void setReadout(GuiCore *c, uint32_t pid) {
    char val[40];
    paramText(pid, c->host->getParam(pid), val, sizeof(val));
    snprintf(c->readout, sizeof(c->readout), "%s :: %s",
             paramDef(pid)->name, val);
    c->readoutAge = 0;
    for (char *p = c->readout; *p; p++) *p = (char)toupper(*p);
}
static void drawKnob(GuiCore *c, const Widget &w, bool hot, float glitch) {
    int r = w.big ? 30 : 22;
    int cx = w.x + w.w / 2, cy = w.y + 14 + r + (w.big ? 6 : 2);
    double nv = toNorm(w.pid, c->host->getParam(w.pid));
    textC(c, w.label, cx, w.y + 2, 1, hot ? C_TEXT : C_DIM);
    // body
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            int d2 = i * i + j * j;
            if (d2 <= r * r) pxA(c, cx + i, cy + j,
                                 d2 > (r - 2) * (r - 2) ? C_BORDER : C_PANEL2, 255);
        }
    // arc: -225deg .. +45deg
    const float a0 = 3.926f, a1 = 0.785f;        // radians (225°, -45° flipped)
    int ticks = w.big ? 96 : 72;
    for (int t = 0; t <= ticks; t++) {
        float f = (float)t / ticks;
        float a = a0 - f * (a0 - a1) * 0 - 0;     // placeholder, computed below
        a = 2.3562f + f * (6.2832f - 1.5708f);    // 135° -> 405°
        float xx = cx + std::cos(a) * (r + 4), yy = cy + std::sin(a) * (r + 4);
        bool lit = f <= (float)nv + 1e-6;
        uint32_t col = lit ? (f < 0.5f ? C_CYAN : C_MAGENTA) : C_BORDER;
        int al = lit ? 235 : 120;
        pxA(c, (int)xx, (int)yy, col, al);
        pxA(c, (int)xx + 1, (int)yy, col, al / 2);
        pxA(c, (int)xx, (int)yy + 1, col, al / 2);
    }
    // needle (jitters a touch while audio glitches)
    float na = 2.3562f + (float)nv * 4.7124f;
    if (glitch > 0.4f) na += (c->rng.uni() - 0.5f) * 0.05f * glitch;
    lineA(c, cx, cy, cx + std::cos(na) * (r - 4), cy + std::sin(na) * (r - 4),
          hot ? C_TEXT : 0xFFB6C9C3, 255);
    char val[32];
    paramText(w.pid, c->host->getParam(w.pid), val, sizeof(val));
    textC(c, val, cx, cy + r + 7, 1, hot ? C_CYAN : C_DIM);
}
static void drawToggle(GuiCore *c, const Widget &w, bool hot) {
    bool on = c->host->getParam(w.pid) >= 0.5;
    fillRect(c, w.x, w.y, w.w, w.h, on ? 0xFF1B3A35 : C_PANEL2);
    frameRect(c, w.x, w.y, w.w, w.h, on ? C_CYAN : (hot ? C_DIM : C_BORDER));
    textC(c, w.label, w.x + w.w / 2, w.y + (w.h - 8) / 2,
          1, on ? C_CYAN : (hot ? C_TEXT : C_DIM));
    if (on) fillRect(c, w.x + 3, w.y + 3, 3, 3, C_CYAN);
}
static void drawCombo(GuiCore *c, const Widget &w, bool hot) {
    char val[32];
    paramText(w.pid, c->host->getParam(w.pid), val, sizeof(val));
    text(c, w.label, w.x, w.y - 11, 1, C_DIM);
    fillRect(c, w.x, w.y, w.w, w.h, C_PANEL2);
    frameRect(c, w.x, w.y, w.w, w.h, hot ? C_CYAN : C_BORDER);
    textC(c, val, w.x + w.w / 2 - 5, w.y + (w.h - 8) / 2, 1, C_TEXT);
    text(c, "\x1F", w.x + w.w - 13, w.y + (w.h - 8) / 2, 1, hot ? C_CYAN : C_DIM);
}
static void drawHold(GuiCore *c, const Widget &w, bool hot, float glitch) {
    bool on = c->host->getParam(w.pid) >= 0.5;
    uint32_t bg = on ? 0xFF541528 : (hot ? 0xFF24161D : C_PANEL2);
    fillRect(c, w.x, w.y, w.w, w.h, bg);
    frameRect(c, w.x, w.y, w.w, w.h, on ? C_MAGENTA : (hot ? C_MAGENTA : C_BORDER));
    if (on) frameRect(c, w.x - 1, w.y - 1, w.w + 2, w.h + 2, C_MAGENTA);
    int jx = on ? (int)((c->rng.uni() - 0.5f) * 4) : 0;
    textC(c, w.label, w.x + w.w / 2 + jx, w.y + (w.h - 16) / 2, 2,
          on ? 0xFFFFD7E8 : (hot ? C_MAGENTA : C_TEXT));
    textC(c, on ? "!! GLITCHING !!" : "HOLD TO WRECK", w.x + w.w / 2,
          w.y + w.h + 4, 1, on ? C_MAGENTA : C_DIM);
    (void)glitch;
}
static void drawAction(GuiCore *c, const Widget &w, bool hot) {
    fillRect(c, w.x, w.y, w.w, w.h, hot ? 0xFF1E2A35 : C_PANEL2);
    frameRect(c, w.x, w.y, w.w, w.h, hot ? C_AMBER : C_BORDER);
    textC(c, w.label, w.x + w.w / 2, w.y + (w.h - 8) / 2, 1,
          hot ? C_AMBER : C_TEXT);
}
static void drawSteps(GuiCore *c, const Widget &w, float glitch) {
    int nsteps = (int)(c->host->getParam(P_STEPS) + 0.5);
    int cur = c->host->uiStepIndex();
    int bw = w.w / 16;
    for (int i = 0; i < 16; i++) {
        int x = w.x + i * bw;
        bool active = i < nsteps;
        bool isCur = active && (cur % imax(1, nsteps)) == i;
        float v = (float)c->host->getParam(P_STEP_0 + i);
        fillRect(c, x, w.y, bw - 4, w.h - 18, active ? C_PANEL2 : 0xFF0E1318);
        int bh = (int)(v * (w.h - 22));
        uint32_t col = v < 0.34f ? C_CYAN : (v < 0.67f ? C_AMBER : C_MAGENTA);
        if (active && bh > 0)
            fillRect(c, x + 2, w.y + (w.h - 20) - bh, bw - 8, bh, col);
        if (active && bh > 0)
            blendRect(c, x + 2, w.y + (w.h - 20) - bh, bw - 8, 2, 0xFFFFFFFF, 90);
        frameRect(c, x, w.y, bw - 4, w.h - 18,
                  isCur ? (glitch > 0.4f ? C_MAGENTA : C_CYAN) : C_BORDER);
        // step LED
        uint32_t led = !active ? 0xFF20262E
                     : isCur ? (glitch > 0.4f ? C_MAGENTA : C_CYAN) : 0xFF31404E;
        fillRect(c, x + bw / 2 - 5, w.y + w.h - 12, 8, 6, led);
        char n[4]; snprintf(n, 4, "%d", i + 1);
        textC(c, n, x + bw / 2 - 2, w.y + w.h - 36, 1,
              active ? 0xFF44586A : 0xFF2A3440, active ? 200 : 120);
    }
}

// ----------------------------------------------------------- panels
static void panel(GuiCore *c, int x, int y, int w, int h, const char *name,
                  uint32_t accent, float pulse = 0) {
    blendRect(c, x + 4, y + 4, w, h, 0xFF000000, 110);   // drop shadow
    fillRect(c, x, y, w, h, C_PANEL);
    frameRect(c, x, y, w, h, C_BORDER);
    if (pulse > 0.02f)
        frameRect(c, x - 1, y - 1, w + 2, h + 2, accent),
        blendRect(c, x + 1, y + 1, w - 2, 2, accent, (int)(pulse * 140));
    fillRect(c, x, y, w, 14, C_PANEL2);
    fillRect(c, x, y + 13, w, 1, C_BORDER);
    fillRect(c, x + 6, y + 4, 6, 6, accent);
    text(c, name, x + 18, y + 3, 1, C_TEXT);
}

// ------------------------------------------------------------ render
const uint32_t *coreRender(GuiCore *c) {
    c->frame++;
    float g = c->host->uiGlitchLevel();
    c->glitchSm += (g - c->glitchSm) * (g > c->glitchSm ? 0.6f : 0.12f);
    float dmg = (float)c->host->getParam(P_GUIFX);
    float amt = fclmp(dmg * (0.25f + c->glitchSm * 1.6f), 0.f, 1.3f);

    // scope history
    c->scope[c->scopeHead] = c->glitchSm;
    c->scopeHead = (c->scopeHead + 1) % 160;

    // 1) wallpaper + pixel-sorting melt (under the panels: always safe)
    memcpy(c->fb.data(), c->wall.data(), c->fb.size() * 4);
    pixelSortPass(c, amt);

    // 2) header
    {
        int jx = (c->glitchSm > 0.35f) ? (int)((c->rng.uni() - 0.5f) * 6 * c->glitchSm) : 0;
        int dx = 1 + (int)(c->glitchSm * 3 + dmg * 1.5f);
        const char *T = "SKRONGLE";
        text(c, T, 26 - dx + jx, 18, 4, C_CYAN, 150);
        text(c, T, 26 + dx + jx, 18, 4, C_MAGENTA, 150);
        text(c, T, 26 + jx, 18, 4, C_TEXT, 255);
        text(c, "PROBABILITY GLITCH ENGINE", 28, 52, 1, C_DIM);
        // activity lamp
        uint32_t lamp = c->glitchSm > 0.4f ? C_MAGENTA
                       : c->glitchSm > 0.05f ? C_AMBER : 0xFF2A3542;
        fillRect(c, 900, 26, 14, 14, lamp);
        frameRect(c, 899, 25, 16, 16, C_BORDER);
        text(c, "GLITCH", 868, 44, 1, C_DIM);
    }

    // 3) panels
    float p = c->glitchSm;
    panel(c, 12, 66, 1016, 152, "SEQUENCER :: GLITCH PROBABILITY PER STEP", C_CYAN, p * 0.5f);
    panel(c, 12, 226, 372, 186, "ENGINE :: MOD DELAY", C_CYAN, p);
    panel(c, 392, 226, 196, 186, "PITCH :: RANDOM S+H", C_AMBER, p);
    panel(c, 596, 226, 310, 186, "FILTER :: RES SVF", C_CYAN, 0);
    panel(c, 914, 226, 114, 186, "SCOPE", C_MAGENTA, 0);
    panel(c, 12, 452, 250, 176, "CRUSH :: QUANTIZER", C_AMBER, 0);
    panel(c, 270, 452, 310, 176, "MAYHEM", C_MAGENTA, p);
    panel(c, 588, 452, 256, 176, "OUTPUT", C_CYAN, 0);

    // info card bottom right
    {
        int x = 852, y = 452, w = 176, h = 176;
        panel(c, x, y, w, h, "STATUS", C_DIM, 0);
        const char *modes[3] = {"FREEZE", "REVERSE", "TAPESTOP"};
        int m = c->host->uiLastMode();
        text(c, "MODE", x + 10, y + 24, 1, C_DIM);
        text(c, modes[m >= 0 && m < 3 ? m : 0], x + 10, y + 36, 2,
             c->glitchSm > 0.2f ? C_MAGENTA : C_TEXT);
        text(c, "STEP", x + 10, y + 62, 1, C_DIM);
        char b[16]; snprintf(b, 16, "%02d", c->host->uiStepIndex() + 1);
        text(c, b, x + 10, y + 74, 2, C_CYAN);
        text(c, "ACTIVITY", x + 10, y + 100, 1, C_DIM);
        int bw = (int)(c->glitchSm * (w - 24));
        fillRect(c, x + 10, y + 112, w - 20, 8, C_PANEL2);
        if (bw > 0) fillRect(c, x + 10, y + 112, bw, 8, C_MAGENTA);
        frameRect(c, x + 10, y + 112, w - 20, 8, C_BORDER);
        text(c, "SW-1 REV 1.0", x + 10, y + h - 18, 1, 0xFF3A4A58);
    }

    // scope trace
    {
        int sx = 922, sy = 246, sw = 98, sh = 152;
        fillRect(c, sx, sy, sw, sh, 0xFF0C1116);
        frameRect(c, sx, sy, sw, sh, C_BORDER);
        for (int i = 0; i < sw - 4; i++) {
            float v = c->scope[(c->scopeHead + 160 - (sw - 4) + i) % 160];
            int hh = (int)(v * (sh - 8));
            uint32_t col = v > 0.5f ? C_MAGENTA : C_CYAN;
            if (hh > 0) fillRect(c, sx + 2 + i, sy + sh - 4 - hh, 1, hh, col);
        }
    }

    // 4) widgets
    for (size_t i = 0; i < c->ws.size(); i++) {
        const Widget &w = c->ws[i];
        bool hot = (int)i == c->hoverW || (int)i == c->dragW;
        switch (w.type) {
            case W_KNOB:   drawKnob(c, w, hot, c->glitchSm); break;
            case W_TOGGLE: drawToggle(c, w, hot); break;
            case W_COMBO:  drawCombo(c, w, hot); break;
            case W_STEPS:  drawSteps(c, w, c->glitchSm); break;
            case W_HOLD:   drawHold(c, w, hot, c->glitchSm); break;
            case W_ACTION: drawAction(c, w, hot); break;
        }
    }

    // 5) readout strip (always crisp — drawn before the damage pass on
    //    purpose? no: keep it after tears would be safer, but tears are rare
    //    and tiny; the strip itself is regenerated every frame)
    fillRect(c, 0, GUI_H - 24, GUI_W, 24, 0xFF0D1218);
    fillRect(c, 0, GUI_H - 24, GUI_W, 1, C_BORDER);
    text(c, "> ", 12, GUI_H - 17, 1, C_CYAN);
    text(c, c->readout, 28, GUI_H - 17, 1,
         c->readoutAge < 60 ? C_TEXT : C_DIM);
    textC(c, "PER-STEP CHANCE DECIDES WHEN THE DELAY FREEZES, WOBBLES, REVERSES OR TAPE-STOPS",
          GUI_W / 2 + 140, GUI_H - 17, 1, 0xFF3E4E5C);
    c->readoutAge++;

    // 6) glass damage on top of everything (thin, readability-aware)
    tearPass(c, amt);
    glassPass(c, fclmp(dmg, 0.f, 1.f));
    return c->fb.data();
}

// -------------------------------------------------------- interaction
static int hitTest(GuiCore *c, int x, int y) {
    for (int i = (int)c->ws.size() - 1; i >= 0; i--) {
        const Widget &w = c->ws[i];
        if (x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h) return i;
    }
    return -1;
}
static void stepsPaint(GuiCore *c, const Widget &w, int x, int y) {
    int bw = w.w / 16;
    int ix = imax(0, imin(15, (x - w.x) / imax(1, bw)));
    float v = fclmp(1.f - (float)(y - w.y) / (w.h - 22), 0.f, 1.f);
    uint32_t pid = P_STEP_0 + ix;
    if (c->dragStepIx != ix) { c->dragStepIx = ix; }
    c->host->setParamFromGui(pid, v, 1);
    setReadout(c, pid);
}
void coreMouseDown(GuiCore *c, int x, int y, int button, bool shift) {
    int i = hitTest(c, x, y);
    c->lastMx = x; c->lastMy = y; c->dragShift = shift;
    if (i < 0) return;
    Widget &w = c->ws[i];
    switch (w.type) {
        case W_KNOB:
            c->dragW = i;
            c->dragStart = toNorm(w.pid, c->host->getParam(w.pid));
            c->host->setParamFromGui(w.pid, c->host->getParam(w.pid), 0);
            setReadout(c, w.pid);
            break;
        case W_TOGGLE: {
            double v = c->host->getParam(w.pid) >= 0.5 ? 0 : 1;
            c->host->setParamFromGui(w.pid, v, 0);
            c->host->setParamFromGui(w.pid, v, 1);
            c->host->setParamFromGui(w.pid, v, 2);
            setReadout(c, w.pid);
        } break;
        case W_COMBO: {
            const ParamDef *d = paramDef(w.pid);
            double v = c->host->getParam(w.pid) + (button == 3 ? -1 : 1);
            if (v > d->maxv) v = d->minv;
            if (v < d->minv) v = d->maxv;
            c->host->setParamFromGui(w.pid, v, 0);
            c->host->setParamFromGui(w.pid, v, 1);
            c->host->setParamFromGui(w.pid, v, 2);
            setReadout(c, w.pid);
        } break;
        case W_STEPS:
            c->dragW = i; c->dragStepIx = -1;
            if (button == 3) {            // right-click clears one bar
                int bw = w.w / 16;
                int ix = imax(0, imin(15, (x - w.x) / imax(1, bw)));
                c->host->setParamFromGui(P_STEP_0 + ix, 0, 1);
            } else stepsPaint(c, w, x, y);
            break;
        case W_HOLD:
            c->dragW = i;
            c->host->setParamFromGui(w.pid, 1, 0);
            c->host->setParamFromGui(w.pid, 1, 1);
            snprintf(c->readout, sizeof(c->readout), "PANIC :: FORCING GLITCH");
            c->readoutAge = 0;
            break;
        case W_ACTION: {
            if (w.action == A_DICE) {
                for (int s = 0; s < 16; s++) {
                    float v = std::pow(c->rng.uni(), 1.6f);
                    if (s % 4 == 0) v *= 0.35f;        // keep downbeats calmer
                    c->host->setParamFromGui(P_STEP_0 + s, v, 1);
                }
                snprintf(c->readout, sizeof(c->readout), "DICE :: NEW PATTERN ROLLED");
            } else {
                for (int s = 0; s < 16; s++)
                    c->host->setParamFromGui(P_STEP_0 + s, 0, 1);
                snprintf(c->readout, sizeof(c->readout), "PATTERN CLEARED");
            }
            c->readoutAge = 0;
        } break;
    }
}
void coreMouseMove(GuiCore *c, int x, int y) {
    if (c->dragW >= 0) {
        Widget &w = c->ws[c->dragW];
        if (w.type == W_KNOB) {
            float speed = c->dragShift ? 0.0008f : 0.005f;
            c->dragStart -= (y - c->lastMy) * speed;
            c->dragStart = fclmp((float)c->dragStart, 0.f, 1.f);
            c->host->setParamFromGui(w.pid, fromNorm(w.pid, c->dragStart), 1);
            setReadout(c, w.pid);
        } else if (w.type == W_STEPS) {
            stepsPaint(c, w, x, y);
        }
        c->lastMx = x; c->lastMy = y;
        return;
    }
    c->lastMx = x; c->lastMy = y;
    int h = hitTest(c, x, y);
    if (h != c->hoverW) {
        c->hoverW = h;
        if (h >= 0 && c->ws[h].type != W_ACTION && c->ws[h].type != W_STEPS)
            setReadout(c, c->ws[h].pid);
    }
}
void coreMouseUp(GuiCore *c, int, int) {
    if (c->dragW >= 0) {
        Widget &w = c->ws[c->dragW];
        if (w.type == W_KNOB) c->host->setParamFromGui(w.pid, c->host->getParam(w.pid), 2);
        if (w.type == W_HOLD) {
            c->host->setParamFromGui(w.pid, 0, 1);
            c->host->setParamFromGui(w.pid, 0, 2);
        }
    }
    c->dragW = -1; c->dragStepIx = -1;
}
void coreWheel(GuiCore *c, int x, int y, float dy) {
    int i = hitTest(c, x, y);
    if (i < 0) return;
    Widget &w = c->ws[i];
    if (w.type == W_KNOB) {
        const ParamDef *d = paramDef(w.pid);
        double n = toNorm(w.pid, c->host->getParam(w.pid));
        n += dy * (d->stepped ? 1.0 / (d->maxv - d->minv) : 1.0 / 48.0);
        c->host->setParamFromGui(w.pid, fromNorm(w.pid, n), 1);
        setReadout(c, w.pid);
    } else if (w.type == W_COMBO) {
        const ParamDef *d = paramDef(w.pid);
        double v = c->host->getParam(w.pid) + (dy > 0 ? 1 : -1);
        v = dclamp(v, d->minv, d->maxv);
        c->host->setParamFromGui(w.pid, v, 1);
        setReadout(c, w.pid);
    }
}
void coreDoubleClick(GuiCore *c, int x, int y) {
    int i = hitTest(c, x, y);
    if (i < 0) return;
    Widget &w = c->ws[i];
    if (w.type == W_KNOB) {
        c->host->setParamFromGui(w.pid, paramDef(w.pid)->defv, 1);
        setReadout(c, w.pid);
    }
}

// ------------------------------------------------------------ lifecycle
GuiCore *coreCreate(ParamHost *host) {
    GuiCore *c = new GuiCore();
    c->host = host;
    c->fb.assign(GUI_W * GUI_H, 0xFF000000);
    buildWallpaper(c);
    buildCracks(c);
    buildLayout(c);
    return c;
}
void coreDestroy(GuiCore *c) { delete c; }
