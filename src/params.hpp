// Skrongle — parameter table
// Shared between the CLAP shell, the DSP engine and the GUI.
#pragma once
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

enum ParamId : uint32_t {
    // --- clock / sequencer ---
    P_RATE = 0,        // step rate (enum, tempo synced)
    P_STEPS,           // number of steps 1..16
    P_SWING,           // 0..1
    P_STEP_0,          // 16 step probabilities, 0..1
    P_STEP_15 = P_STEP_0 + 15,

    // --- glitch engine (mod delay) ---
    P_DELAY_MS,        // base delay time, ms
    P_FEEDBACK,        // idle feedback
    P_FREEZE_FB,       // feedback while glitch is active
    P_KILL_INPUT,      // mute delay input while glitching (bool)
    P_PINGPONG,        // stereo cross-feedback 0..1
    P_GLITCH_WET,      // how far the blend goes during a glitch
    P_BASE_WET,        // blend position when idle

    // --- pitch glitch (random S&H on delay time) ---
    P_PITCH_DEPTH,     // 0..1  (-> +-60ms)
    P_PITCH_RATE,      // Hz
    P_PITCH_SMOOTH,    // 0 = stepped, 1 = full crossfade

    // --- resonant filter before the delay ---
    P_FILTER_ON,
    P_FILTER_TYPE,     // enum LP/BP/HP/Notch
    P_CUTOFF,          // Hz (log)
    P_RESO,            // 0..1
    P_FILT_MOD,        // random mod depth 0..1
    P_FILT_RATE,       // Hz
    P_FILT_STEREO,     // invert right channel mod (bool)

    // --- quantizer / crusher before the delay ---
    P_CRUSH_ON,
    P_BITS,            // 1..16
    P_DOWNSAMPLE,      // 1..64x
    P_CRUSH_MIX,

    // --- mayhem ---
    P_REVERSE_CH,      // chance a glitch step plays the buffer backwards
    P_TAPESTOP_CH,     // chance a glitch step does a tape-stop drop
    P_RATCHET_CH,      // chance of sub-step re-grab stutters
    P_CHAOS,           // master randomness macro
    P_FORCE,           // momentary "glitch now" (bool)

    // --- output ---
    P_DRIVE,           // wet saturation
    P_DUCK_DRY,        // duck the dry signal during glitches
    P_GAIN_DB,         // output gain

    // --- UI ---
    P_GUIFX,           // broken-screen intensity (saved with the project)

    P_COUNT
};

enum { RATE_1_1, RATE_1_2, RATE_1_4, RATE_1_8, RATE_1_16, RATE_1_32,
       RATE_1_8T, RATE_1_16T, RATE_1_8D, RATE_1_16D, RATE_COUNT };

enum { FILT_LP, FILT_BP, FILT_HP, FILT_NOTCH, FILT_TYPE_COUNT };

struct ParamDef {
    uint32_t    id;
    const char *name;
    const char *module;
    double      minv, maxv, defv;
    bool        stepped;
    const char *unit;
};

inline double rateToBeats(int r) {
    switch (r) {
        case RATE_1_1:   return 4.0;
        case RATE_1_2:   return 2.0;
        case RATE_1_4:   return 1.0;
        case RATE_1_8:   return 0.5;
        case RATE_1_16:  return 0.25;
        case RATE_1_32:  return 0.125;
        case RATE_1_8T:  return 1.0 / 3.0;
        case RATE_1_16T: return 1.0 / 6.0;
        case RATE_1_8D:  return 0.75;
        case RATE_1_16D: return 0.375;
        default:         return 0.25;
    }
}
inline const char *rateName(int r) {
    static const char *n[RATE_COUNT] = {"1/1","1/2","1/4","1/8","1/16","1/32",
                                        "1/8T","1/16T","1/8.","1/16."};
    return (r >= 0 && r < RATE_COUNT) ? n[r] : "?";
}
inline const char *filtName(int t) {
    static const char *n[FILT_TYPE_COUNT] = {"LOWPASS","BANDPASS","HIGHPASS","NOTCH"};
    return (t >= 0 && t < FILT_TYPE_COUNT) ? n[t] : "?";
}

// Default step pattern: keep the downbeat clean (like the video — don't
// glitch over the main kick), get busier towards the end of the bar.
inline double defaultStep(int i) {
    static const double d[16] = {0.00, 0.00, 0.35, 0.15,
                                 0.00, 0.45, 0.20, 0.60,
                                 0.00, 0.30, 0.50, 0.25,
                                 0.10, 0.65, 0.40, 0.80};
    return d[i & 15];
}

inline const ParamDef *paramDef(uint32_t id) {
    static ParamDef defs[P_COUNT];
    static bool init = false;
    if (!init) {
        init = true;
        auto set = [&](uint32_t i, const char *n, const char *m, double lo,
                       double hi, double dv, bool st, const char *u) {
            defs[i] = {i, n, m, lo, hi, dv, st, u};
        };
        set(P_RATE,        "Step Rate",       "Sequencer", 0, RATE_COUNT - 1, RATE_1_16, true,  "");
        set(P_STEPS,       "Steps",           "Sequencer", 1, 16, 16, true,  "");
        set(P_SWING,       "Swing",           "Sequencer", 0, 1, 0.0, false, "%");
        for (int i = 0; i < 16; i++) {
            static char names[16][16];
            snprintf(names[i], sizeof(names[i]), "Step %d", i + 1);
            set(P_STEP_0 + i, names[i], "Sequencer", 0, 1, defaultStep(i), false, "%");
        }
        set(P_DELAY_MS,    "Delay Time",      "Engine", 1, 500, 60, false, "ms");
        set(P_FEEDBACK,    "Feedback",        "Engine", 0, 1, 0.35, false, "%");
        set(P_FREEZE_FB,   "Freeze FB",       "Engine", 0, 1, 1.0, false, "%");
        set(P_KILL_INPUT,  "Kill Input",      "Engine", 0, 1, 1, true,  "");
        set(P_PINGPONG,    "Ping-Pong",       "Engine", 0, 1, 0.0, false, "%");
        set(P_GLITCH_WET,  "Glitch Blend",    "Engine", 0, 1, 1.0, false, "%");
        set(P_BASE_WET,    "Idle Blend",      "Engine", 0, 1, 0.0, false, "%");
        set(P_PITCH_DEPTH, "Pitch Depth",     "Pitch", 0, 1, 0.35, false, "%");
        set(P_PITCH_RATE,  "Pitch Rate",      "Pitch", 0.2, 60, 9, false, "Hz");
        set(P_PITCH_SMOOTH,"Pitch Smooth",    "Pitch", 0, 1, 1.0, false, "%");
        set(P_FILTER_ON,   "Filter On",       "Filter", 0, 1, 1, true, "");
        set(P_FILTER_TYPE, "Filter Type",     "Filter", 0, FILT_TYPE_COUNT - 1, FILT_BP, true, "");
        set(P_CUTOFF,      "Cutoff",          "Filter", 40, 18000, 1200, false, "Hz");
        set(P_RESO,        "Resonance",       "Filter", 0, 1, 0.92, false, "%");
        set(P_FILT_MOD,    "Filter Mod",      "Filter", 0, 1, 0.6, false, "%");
        set(P_FILT_RATE,   "Filter Rate",     "Filter", 0.2, 40, 6, false, "Hz");
        set(P_FILT_STEREO, "Stereo Invert",   "Filter", 0, 1, 1, true, "");
        set(P_CRUSH_ON,    "Crush On",        "Crush", 0, 1, 0, true, "");
        set(P_BITS,        "Bit Depth",       "Crush", 1, 16, 8, true, "bit");
        set(P_DOWNSAMPLE,  "Downsample",      "Crush", 1, 64, 4, false, "x");
        set(P_CRUSH_MIX,   "Crush Mix",       "Crush", 0, 1, 1, false, "%");
        set(P_REVERSE_CH,  "Reverse",         "Mayhem", 0, 1, 0.25, false, "%");
        set(P_TAPESTOP_CH, "Tape Stop",       "Mayhem", 0, 1, 0.12, false, "%");
        set(P_RATCHET_CH,  "Ratchet",         "Mayhem", 0, 1, 0.30, false, "%");
        set(P_CHAOS,       "Chaos",           "Mayhem", 0, 1, 0.5, false, "%");
        set(P_FORCE,       "Force Glitch",    "Mayhem", 0, 1, 0, true, "");
        set(P_DRIVE,       "Drive",           "Output", 0, 1, 0.15, false, "%");
        set(P_DUCK_DRY,    "Duck Dry",        "Output", 0, 1, 1.0, false, "%");
        set(P_GAIN_DB,     "Output Gain",     "Output", -24, 24, 0, false, "dB");
        set(P_GUIFX,       "Screen Damage",   "UI", 0, 1, 0.6, false, "%");
    }
    return (id < P_COUNT) ? &defs[id] : nullptr;
}

// Pretty-printing for hosts and the GUI readout strip.
inline void paramText(uint32_t id, double v, char *out, size_t n) {
    const ParamDef *d = paramDef(id);
    if (!d) { snprintf(out, n, "?"); return; }
    switch (id) {
        case P_RATE:        snprintf(out, n, "%s", rateName((int)(v + 0.5))); return;
        case P_FILTER_TYPE: snprintf(out, n, "%s", filtName((int)(v + 0.5))); return;
        case P_STEPS:       snprintf(out, n, "%d", (int)(v + 0.5)); return;
        case P_BITS:        snprintf(out, n, "%d bit", (int)(v + 0.5)); return;
        case P_KILL_INPUT: case P_FILTER_ON: case P_FILT_STEREO:
        case P_CRUSH_ON:   case P_FORCE:
            snprintf(out, n, v >= 0.5 ? "ON" : "OFF"); return;
        case P_DELAY_MS:    snprintf(out, n, "%.1f ms", v); return;
        case P_CUTOFF:      v >= 1000 ? snprintf(out, n, "%.2f kHz", v / 1000)
                                      : snprintf(out, n, "%.0f Hz", v); return;
        case P_PITCH_RATE: case P_FILT_RATE: snprintf(out, n, "%.1f Hz", v); return;
        case P_DOWNSAMPLE:  snprintf(out, n, "%.0fx", v); return;
        case P_GAIN_DB:     snprintf(out, n, "%+.1f dB", v); return;
        default:
            if (!strcmp(d->unit, "%")) snprintf(out, n, "%.0f %%", v * 100);
            else                        snprintf(out, n, "%.2f", v);
    }
}
