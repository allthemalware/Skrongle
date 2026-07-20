// Skrongle — DSP engine
// The signal flow follows the Bitwig Grid patch from the video:
//
//   in -> [resonant SVF, stereo-inverted random mod] -> [quantizer/crusher]
//      -> [mod delay]  ... blend(dry, delay out) driven by a probability
//      step sequencer.  While a step "glitches": the blend goes wet, the
//      delay input is muted and the feedback is pinned high (freeze), and a
//      smoothed random S&H wobbles the delay time for pitch glitches.
//
// On top of that: reverse playback, tape-stops, ratchet re-grabs, ping-pong
// feedback, drive, swing and a chaos macro.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <atomic>
#include "params.hpp"

static inline float fclampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline double dclamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------- RNG
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    void seed(uint64_t v) { s = v ? v : 1; }
    uint64_t next64() {            // xorshift64*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    float uni()  { return (float)((next64() >> 11) * (1.0 / 9007199254740992.0)); } // [0,1)
    float bi()   { return uni() * 2.f - 1.f; }                                       // [-1,1)
    bool  roll(float p) { return uni() < p; }
};

// ------------------------------------------- random sample & hold LFO
// "Smooth" crossfades between held values exactly like the Grid module's
// smooth mode: 0 = hard steps, 1 = a full cosine ramp across the period.
struct ShLfo {
    Rng   *rng = nullptr;
    double phase = 0;
    float  prev = 0, target = 0;
    void reset(Rng *r) { rng = r; phase = 0; prev = 0; target = rng->bi(); }
    void jump() { prev = target; target = rng->bi(); phase = 0; }
    float tick(double rateHz, double sr, float smooth) {
        phase += rateHz / sr;
        if (phase >= 1.0) { phase -= std::floor(phase); jump(); }
        if (smooth <= 0.001f) return target;
        double w = dclamp(phase / dclamp((double)smooth, 0.001, 1.0), 0.0, 1.0);
        float x = 0.5f - 0.5f * (float)std::cos(w * M_PI);  // cosine xfade
        return prev + (target - prev) * x;
    }
};

// -------------------------------------------------- state variable filter
// Cytomic-style trapezoidal SVF; cheap, stable when modulated hard.
struct Svf {
    float ic1 = 0, ic2 = 0;
    float g = 0.1f, k = 1.f;
    void setup(float cutoffHz, float reso, float sr) {
        cutoffHz = fclampf(cutoffHz, 20.f, sr * 0.47f);
        g = std::tan((float)M_PI * cutoffHz / sr);
        // resonance 0..1 -> k 2..0.04 (don't allow full self-oscillation)
        k = 2.f - 1.96f * fclampf(reso, 0.f, 1.f);
    }
    float process(float v0, int type) {
        float a1 = 1.f / (1.f + g * (g + k));
        float a2 = g * a1, a3 = g * a2;
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        switch (type) {
            case FILT_LP:    return v2;
            case FILT_BP:    return v1;
            case FILT_HP:    return v0 - k * v1 - v2;
            default:         return v0 - k * v1;          // notch
        }
    }
    void clear() { ic1 = ic2 = 0; }
};

// ------------------------------------------------------------- crusher
struct Crusher {
    float heldL = 0, heldR = 0;
    float cnt = 1e9f;
    void process(float &l, float &r, float bits, float factor, float mix) {
        cnt += 1.f;
        if (cnt >= factor) {
            cnt = std::fmod(cnt, factor > 1.f ? factor : 1.f);
            float q = std::exp2(bits - 1.f);
            heldL = std::round(fclampf(l, -1.f, 1.f) * q) / q;
            heldR = std::round(fclampf(r, -1.f, 1.f) * q) / q;
        }
        l += (heldL - l) * mix;
        r += (heldR - r) * mix;
    }
};

// ----------------------------------------------------------- delay line
struct DelayLine {
    std::vector<float> buf;       // interleaved stereo
    int     len = 0;              // frames
    int64_t w  = 0;               // absolute write head (frames)
    void resize(int frames) {
        len = frames;
        buf.assign((size_t)frames * 2, 0.f);
        w = 0;
    }
    inline void write(float l, float r, float gain) {
        size_t i = (size_t)(w % len) * 2;
        buf[i]     += (l - buf[i])     * gain;   // gain 1 = overwrite, 0 = keep (freeze tape)
        buf[i + 1] += (r - buf[i + 1]) * gain;
    }
    inline void hardWrite(float l, float r) {
        size_t i = (size_t)(w % len) * 2;
        buf[i] = l; buf[i + 1] = r;
    }
    // read at absolute position (frames, fractional), linear interp
    inline void readAbs(double pos, float &l, float &r) const {
        double fl = std::floor(pos);
        float  fr = (float)(pos - fl);
        int64_t i0 = (int64_t)fl % len; if (i0 < 0) i0 += len;
        int64_t i1 = (i0 + 1) % len;
        const float *a = &buf[(size_t)i0 * 2], *b = &buf[(size_t)i1 * 2];
        l = a[0] + (b[0] - a[0]) * fr;
        r = a[1] + (b[1] - a[1]) * fr;
    }
    inline void readBack(double delayFrames, float &l, float &r) const {
        readAbs((double)w - dclamp(delayFrames, 1.0, (double)len - 4), l, r);
    }
};

// glitch step playback modes
enum GlitchMode { GM_FREEZE, GM_REVERSE, GM_TAPESTOP };

// ------------------------------------------------------------ the engine
struct Engine {
    double sr = 48000;
    DelayLine delay;
    Svf  svfL, svfR;
    Crusher crush;
    Rng  rng;
    ShLfo pitchLfo, filtLfo;

    // step clock
    double stepPos   = -1;     // global position in steps (fractional)
    double freePhase = 0;      // free-running clock when transport stopped
    int    lastStepI = -0x7fffffff;
    double lastTempo = 120.0;

    // glitch state
    bool   gate = false;
    int    mode = GM_FREEZE;
    double segPos = 0;         // absolute read pos for reverse/tape-stop
    double segSpeed = 1;
    double stepLenSamples = 1;
    int    ratchetSlot = 0;
    int    regrab = 0;         // samples left with the input window re-opened

    // per-sample smoothers
    float blendZ = 0, inGainZ = 1, fbZ = 0, wetEnvZ = 0;
    float delayMsZ = 60;

    // shared with the GUI (visuals react to the audio)
    std::atomic<float> *uiGlitch = nullptr;   // 0..1 activity
    std::atomic<int>   *uiStep   = nullptr;   // current step index
    std::atomic<int>   *uiMode   = nullptr;   // last glitch mode

    void activate(double sampleRate) {
        sr = sampleRate;
        delay.resize((int)(sr * 2.5) + 16);
        svfL.clear(); svfR.clear();
        rng.seed(0xC0FFEE ^ (uint64_t)(sr));
        pitchLfo.reset(&rng);
        filtLfo.reset(&rng);
        blendZ = 0; inGainZ = 1; fbZ = 0; wetEnvZ = 0;
        lastStepI = -0x7fffffff;
        gate = false; freePhase = 0;
    }

    // Decide what happens on a new step.
    void onStep(int idx, const double *p) {
        int nsteps = (int)(p[P_STEPS] + 0.5);
        int s = ((idx % nsteps) + nsteps) % nsteps;
        if (uiStep) uiStep->store(s, std::memory_order_relaxed);
        float chaos = (float)p[P_CHAOS];
        float prob = (float)p[P_STEP_0 + s];
        // chaos > 0.5 leaks extra probability everywhere
        prob = fclampf(prob + fclampf(chaos - 0.5f, 0.f, 0.5f) * 0.7f, 0.f, 1.f);
        bool force = p[P_FORCE] >= 0.5;
        gate = force || rng.roll(prob);
        ratchetSlot = 0;
        if (!gate) { mode = GM_FREEZE; return; }

        float rmul = 0.4f + 1.2f * chaos;     // chaos scales the weird modes
        if (rng.roll((float)p[P_TAPESTOP_CH] * rmul))      mode = GM_TAPESTOP;
        else if (rng.roll((float)p[P_REVERSE_CH] * rmul))  mode = GM_REVERSE;
        else                                               mode = GM_FREEZE;
        if (uiMode) uiMode->store(mode, std::memory_order_relaxed);

        if (mode == GM_REVERSE || mode == GM_TAPESTOP) {
            segPos = (double)delay.w - 4.0;   // play from "now" backwards/down
            segSpeed = 1.0;
        }
        pitchLfo.jump();                       // fresh pitch on every glitch
    }

    void process(const float *inL, const float *inR, float *outL, float *outR,
                 uint32_t n, const double *p,
                 bool playing, double tempo, double songPosBeats, bool hasTransport) {
        if (tempo > 1) lastTempo = tempo;
        const double beatsPerStep = rateToBeats((int)(p[P_RATE] + 0.5));
        const double stepHz = (lastTempo / 60.0) / beatsPerStep;
        stepLenSamples = sr / dclamp(stepHz, 0.01, 1000.0);

        const float swing = (float)p[P_SWING] * 0.4f;
        const float pitchDepthMs = (float)p[P_PITCH_DEPTH] *
                                   (40.f + 50.f * (float)p[P_CHAOS]);
        const float drive = (float)p[P_DRIVE];
        const float outGain = std::pow(10.f, (float)p[P_GAIN_DB] / 20.f);
        const bool  filterOn = p[P_FILTER_ON] >= 0.5;
        const bool  crushOn  = p[P_CRUSH_ON]  >= 0.5;
        const int   ftype = (int)(p[P_FILTER_TYPE] + 0.5);
        const bool  stInv = p[P_FILT_STEREO] >= 0.5;
        const float duck  = (float)p[P_DUCK_DRY];
        const float pp    = (float)p[P_PINGPONG];

        const float smoothA = 1.f - std::exp(-1.f / (0.003f * (float)sr)); // ~3ms ramps
        const float msA     = 1.f - std::exp(-1.f / (0.020f * (float)sr)); // knob slew

        float uiAcc = 0.f;

        for (uint32_t i = 0; i < n; i++) {
            // ---------------- clock ----------------
            double sp;
            if (hasTransport && playing) {
                sp = (songPosBeats + (double)i * (lastTempo / 60.0) / sr) / beatsPerStep;
                freePhase = sp;
            } else {
                freePhase += stepHz / sr;
                sp = freePhase;
            }
            // swing: delay every odd step
            double swung = sp - ((((int64_t)std::floor(sp)) & 1) ? swing : 0.0);
            int stepI = (int)std::floor(swung);
            double stepPhase = swung - stepI;
            if (stepI != lastStepI) { lastStepI = stepI; onStep(stepI, p); }

            // momentary force outside step boundaries too
            if (!gate && p[P_FORCE] >= 0.5) { gate = true; mode = GM_FREEZE; }

            // ratchet: chance to "re-grab" fresh audio mid-glitch (sub-stutter)
            int slot = (int)(stepPhase * 4.0);
            if (gate && slot != ratchetSlot) {
                ratchetSlot = slot;
                if (rng.roll((float)p[P_RATCHET_CH] * (0.5f + (float)p[P_CHAOS]))) {
                    regrab = (int)(sr * 0.012);
                    pitchLfo.jump();
                    if (mode != GM_FREEZE) segPos = (double)delay.w - 4.0;
                }
            }

            // ---------------- targets & smoothing ----------------
            bool open = regrab > 0; if (regrab > 0) regrab--;
            float gateT  = gate ? 1.f : 0.f;
            float blendT = gate ? (float)p[P_GLITCH_WET] : (float)p[P_BASE_WET];
            float inT    = (gate && p[P_KILL_INPUT] >= 0.5 && !open) ? 0.f : 1.f;
            float fbT    = gate ? (float)p[P_FREEZE_FB] : (float)p[P_FEEDBACK];
            blendZ  += (blendT - blendZ) * smoothA;
            inGainZ += (inT    - inGainZ) * smoothA;
            fbZ     += (fbT    - fbZ)     * smoothA;
            wetEnvZ += (gateT  - wetEnvZ) * smoothA;
            delayMsZ += ((float)p[P_DELAY_MS] - delayMsZ) * msA;

            // ---------------- input chain (filter -> crush) ----------------
            float dl = inL ? inL[i] : 0.f, dr = inR ? inR[i] : dl;
            float l = dl, r = dr;
            if (filterOn) {
                float m = filtLfo.tick(p[P_FILT_RATE], sr, 1.f) * (float)p[P_FILT_MOD];
                float base = (float)p[P_CUTOFF];
                float cl = base * std::exp2(m * 3.f);
                float cr = base * std::exp2((stInv ? -m : m) * 3.f);
                svfL.setup(cl, (float)p[P_RESO], (float)sr);
                svfR.setup(cr, (float)p[P_RESO], (float)sr);
                l = svfL.process(l, ftype);
                r = svfR.process(r, ftype);
            }
            if (crushOn)
                crush.process(l, r, (float)p[P_BITS],
                              fclampf((float)p[P_DOWNSAMPLE], 1.f, 64.f),
                              (float)p[P_CRUSH_MIX]);

            // ---------------- mod delay ----------------
            float wob = pitchLfo.tick(p[P_PITCH_RATE] * (0.5 + p[P_CHAOS]), sr,
                                      (float)p[P_PITCH_SMOOTH]);
            // wobble only matters while glitching (delay time stays put when idle)
            float dms = delayMsZ + wob * pitchDepthMs * wetEnvZ;
            double dSamp = dclamp((double)dms, 1.0, 2400.0) * 0.001 * sr;

            float wl, wr;
            bool tapeMode = gate && mode != GM_FREEZE;
            if (tapeMode) {
                // tape is frozen: read with our own head, write nothing
                if (mode == GM_REVERSE) segPos -= 1.0;          // -1 rel. to stalled tape
                else {
                    segSpeed += (-segSpeed) * (1.2 / stepLenSamples); // exp slow-down
                    segPos   += dclamp(segSpeed, 0.0, 1.0) - 0.0;
                    segPos   -= 0.0;
                }
                // keep the head inside the recorded past
                double back = (double)delay.w - segPos;
                if (back > (double)delay.len - 8) segPos = (double)delay.w - dSamp;
                if (back < 1) segPos = (double)delay.w - 1;
                delay.readAbs(segPos, wl, wr);
                delay.write(0, 0, 0.f);                         // freeze tape
            } else {
                delay.readBack(dSamp, wl, wr);
                float fl = wl, fr = wr;
                if (pp > 0.f) { float t = fl; fl += (fr - fl) * pp; fr += (t - fr) * pp; }
                float nl = l * inGainZ + fl * fbZ;
                float nr = r * inGainZ + fr * fbZ;
                // soft clip inside the loop so 100% feedback can't blow up
                nl = std::tanh(nl * 1.1f) * 0.909f;
                nr = std::tanh(nr * 1.1f) * 0.909f;
                delay.hardWrite(nl, nr);
            }
            delay.w++;

            // ---------------- wet shaping + blend ----------------
            if (drive > 0.001f) {
                float g = 1.f + drive * 9.f;
                float comp = 1.f / std::tanh(g * 0.8f);
                wl = std::tanh(wl * g) * comp * 0.8f;
                wr = std::tanh(wr * g) * comp * 0.8f;
            }
            float dryG = (1.f - blendZ) * (1.f - duck * wetEnvZ * blendZ);
            float ol = (dl * dryG + wl * blendZ) * outGain;
            float orr = (dr * dryG + wr * blendZ) * outGain;
            outL[i] = ol; outR[i] = orr;
            uiAcc = std::max(uiAcc, wetEnvZ);
        }
        if (uiGlitch) uiGlitch->store(uiAcc, std::memory_order_relaxed);
    }
};
