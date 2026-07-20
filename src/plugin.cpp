// Skrongle — CLAP plugin shell
#include <clap/clap.h>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <cstdint>
#include "params.hpp"
#include "dsp.hpp"
#include "gui/gui.hpp"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #include <xmmintrin.h>
  #define SKR_X86_DENORMAL 1
#endif

// Flush denormals to zero for the duration of the audio callback: the SVF
// integrators and the exponential smoothers all decay toward zero on quiet
// input and would otherwise spike CPU (and, in a freeze delay, recirculate
// forever). Restores the caller's FP state on scope exit.
struct DenormalGuard {
#if defined(SKR_X86_DENORMAL)
    unsigned int saved;
    DenormalGuard()  { saved = _mm_getcsr(); _mm_setcsr(saved | 0x8040); } // FTZ|DAZ
    ~DenormalGuard() { _mm_setcsr(saved); }
#elif defined(__aarch64__)
    uint64_t saved;
    DenormalGuard()  {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved));
        uint64_t fz = saved | (1ULL << 24);                                // FZ
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fz));
    }
    ~DenormalGuard() { __asm__ __volatile__("msr fpcr, %0" : : "r"(saved)); }
#else
    DenormalGuard() {}
#endif
};

// ---------------------------------------------------------------- queues
// single-producer single-consumer ring (GUI thread -> audio thread)
struct ParamMsg { uint32_t id; double value; uint8_t kind; }; // 0 begin 1 value 2 end
struct ParamQueue {
    static const int N = 512;
    ParamMsg buf[N];
    std::atomic<int> head{0}, tail{0};
    bool push(const ParamMsg &m) {
        int h = head.load(std::memory_order_relaxed);
        int nh = (h + 1) & (N - 1);
        if (nh == tail.load(std::memory_order_acquire)) return false;
        buf[h] = m;
        head.store(nh, std::memory_order_release);
        return true;
    }
    bool pop(ParamMsg &m) {
        int t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        m = buf[t];
        tail.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }
};

// ---------------------------------------------------------------- plugin
struct Plugin : public ParamHost {
    clap_plugin_t plug{};
    const clap_host_t *host = nullptr;
    const clap_host_params_t        *hostParams = nullptr;
    const clap_host_timer_support_t *hostTimer  = nullptr;

    std::atomic<double> values[P_COUNT];
    ParamQueue toAudio;            // GUI -> audio (also forwarded to host)
    Engine engine;

    std::atomic<float> uiGlitch{0};
    std::atomic<int>   uiStep{0};
    std::atomic<int>   uiMode{0};
    std::atomic<uint32_t> uiDirty{1};

    GuiPlatform *gui = nullptr;
    clap_id guiTimer = CLAP_INVALID_ID;

    Plugin(const clap_host_t *h) : host(h) {
        for (uint32_t i = 0; i < P_COUNT; i++)
            values[i].store(paramDef(i)->defv);
        engine.uiGlitch = &uiGlitch;
        engine.uiStep   = &uiStep;
        engine.uiMode   = &uiMode;
    }

    // ---- ParamHost interface (used by the GUI) ----
    double getParam(uint32_t id) override { return values[id].load(std::memory_order_relaxed); }
    void   setParamFromGui(uint32_t id, double v, int kind) override {
        const ParamDef *d = paramDef(id);
        v = dclamp(v, d->minv, d->maxv);
        if (kind != 0 && kind != 2) values[id].store(v, std::memory_order_relaxed);
        toAudio.push({id, v, (uint8_t)kind});
        if (hostParams) hostParams->request_flush(host);
    }
    float uiGlitchLevel() override { return uiGlitch.load(std::memory_order_relaxed); }
    int   uiStepIndex()   override { return uiStep.load(std::memory_order_relaxed); }
    int   uiLastMode()    override { return uiMode.load(std::memory_order_relaxed); }
    bool  takeDirty()     override { return uiDirty.exchange(0) != 0; }

    // ---- event plumbing ----
    void handleInEvent(const clap_event_header_t *h) {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            auto *e = (const clap_event_param_value_t *)h;
            if (e->param_id < P_COUNT) {
                const ParamDef *d = paramDef(e->param_id);
                values[e->param_id].store(dclamp(e->value, d->minv, d->maxv),
                                          std::memory_order_relaxed);
                uiDirty.store(1, std::memory_order_relaxed);
            }
        }
    }
    void drainGuiQueue(const clap_output_events_t *out) {
        ParamMsg m;
        while (toAudio.pop(m)) {
            if (m.kind == 0 || m.kind == 2) {
                clap_event_param_gesture_t e{};
                e.header.size = sizeof(e);
                e.header.type = m.kind == 0 ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                            : CLAP_EVENT_PARAM_GESTURE_END;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.param_id = m.id;
                if (out) out->try_push(out, &e.header);
            } else {
                values[m.id].store(m.value, std::memory_order_relaxed);
                clap_event_param_value_t e{};
                e.header.size = sizeof(e);
                e.header.type = CLAP_EVENT_PARAM_VALUE;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.param_id = m.id;
                e.cookie = nullptr;
                e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
                e.value = m.value;
                if (out) out->try_push(out, &e.header);
            }
        }
    }
};

#define SELF ((Plugin *)plugin->plugin_data)

// ------------------------------------------------------------ descriptor
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                   CLAP_PLUGIN_FEATURE_GLITCH,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};
static const clap_plugin_descriptor_t s_desc = {
    CLAP_VERSION_INIT,
    "com.openglitch.skrongle",
    "Skrongle",
    "OpenGlitch",
    "https://example.com",
    "",
    "",
    "1.0.0",
    "Probability-sequenced stutter / glitch delay with pitch wobble, "
    "resonant filter mangling, bit crushing, reverse & tape-stop mayhem.",
    s_features,
};

// ------------------------------------------------------------- core impl
static bool plugInit(const clap_plugin *plugin) {
    Plugin *p = SELF;
    p->hostParams = (const clap_host_params_t *)
        p->host->get_extension(p->host, CLAP_EXT_PARAMS);
    p->hostTimer = (const clap_host_timer_support_t *)
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    return true;
}
static void plugDestroy(const clap_plugin *plugin) {
    Plugin *p = SELF;
    if (p->gui) { guiDestroy(p->gui); p->gui = nullptr; }
    delete p;
}
static bool plugActivate(const clap_plugin *plugin, double sr,
                         uint32_t, uint32_t) {
    SELF->engine.activate(sr);
    return true;
}
static void plugDeactivate(const clap_plugin *) {}
static bool plugStartProcessing(const clap_plugin *) { return true; }
static void plugStopProcessing(const clap_plugin *) {}
static void plugReset(const clap_plugin *plugin) {
    SELF->engine.activate(SELF->engine.sr);
}

static clap_process_status plugProcess(const clap_plugin *plugin,
                                       const clap_process_t *pr) {
    Plugin *p = SELF;
    DenormalGuard _denorm;   // flush denormals for this callback

    if (pr->in_events) {
        uint32_t n = pr->in_events->size(pr->in_events);
        for (uint32_t i = 0; i < n; i++)
            p->handleInEvent(pr->in_events->get(pr->in_events, i));
    }
    p->drainGuiQueue(pr->out_events);

    double snap[P_COUNT];
    for (uint32_t i = 0; i < P_COUNT; i++)
        snap[i] = p->values[i].load(std::memory_order_relaxed);

    bool playing = false, hasTr = false;
    double tempo = 0, beats = 0;
    if (pr->transport) {
        hasTr = true;
        playing = pr->transport->flags & CLAP_TRANSPORT_IS_PLAYING;
        if (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO)
            tempo = pr->transport->tempo;
        if (pr->transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
            beats = (double)pr->transport->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
    }

    const float *inL = nullptr, *inR = nullptr;
    float *outL = nullptr, *outR = nullptr;
    if (pr->audio_inputs_count > 0 && pr->audio_inputs[0].channel_count > 0) {
        inL = pr->audio_inputs[0].data32[0];
        inR = pr->audio_inputs[0].channel_count > 1 ? pr->audio_inputs[0].data32[1] : inL;
    }
    if (pr->audio_outputs_count > 0 && pr->audio_outputs[0].channel_count > 0) {
        outL = pr->audio_outputs[0].data32[0];
        outR = pr->audio_outputs[0].channel_count > 1 ? pr->audio_outputs[0].data32[1] : outL;
    }
    if (!outL) return CLAP_PROCESS_CONTINUE;

    p->engine.process(inL, inR, outL, outR, pr->frames_count, snap,
                      playing, tempo, beats, hasTr);
    return CLAP_PROCESS_CONTINUE;
}

// --------------------------------------------------------------- params
static uint32_t paramsCount(const clap_plugin *) { return P_COUNT; }
static bool paramsInfo(const clap_plugin *, uint32_t index,
                       clap_param_info_t *info) {
    if (index >= P_COUNT) return false;
    const ParamDef *d = paramDef(index);
    memset(info, 0, sizeof(*info));
    info->id = d->id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | (d->stepped ? CLAP_PARAM_IS_STEPPED : 0);
    info->cookie = nullptr;
    snprintf(info->name, sizeof(info->name), "%s", d->name);
    snprintf(info->module, sizeof(info->module), "%s", d->module);
    info->min_value = d->minv;
    info->max_value = d->maxv;
    info->default_value = d->defv;
    return true;
}
static bool paramsValue(const clap_plugin *plugin, clap_id id, double *out) {
    if (id >= P_COUNT) return false;
    *out = SELF->values[id].load(std::memory_order_relaxed);
    return true;
}
static bool paramsValueToText(const clap_plugin *, clap_id id, double v,
                              char *out, uint32_t cap) {
    if (id >= P_COUNT) return false;
    paramText(id, v, out, cap);
    return true;
}
// Case-insensitive equality for short parameter-text tokens.
static bool ieq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}
static bool paramsTextToValue(const clap_plugin *, clap_id id,
                              const char *txt, double *out) {
    if (id >= P_COUNT || !txt) return false;
    const ParamDef *d = paramDef(id);
    while (*txt == ' ' || *txt == '\t') ++txt;   // skip leading space

    // Reverse the enum/bool formatting from value_to_text (paramText), so typing
    // back exactly what the host displays round-trips instead of going through
    // atof (which turned "BANDPASS" -> 0, "ON" -> 0, "1/8" -> 1).
    if (id == P_RATE) {
        for (int i = 0; i < RATE_COUNT; ++i)
            if (ieq(txt, rateName(i))) { *out = i; return true; }
    } else if (id == P_FILTER_TYPE) {
        for (int i = 0; i < FILT_TYPE_COUNT; ++i)
            if (ieq(txt, filtName(i))) { *out = i; return true; }
    }
    if (d->stepped && d->minv == 0 && d->maxv == 1) {   // ON/OFF toggles
        if (ieq(txt, "on")  || ieq(txt, "true")  || ieq(txt, "yes")) { *out = 1; return true; }
        if (ieq(txt, "off") || ieq(txt, "false") || ieq(txt, "no"))  { *out = 0; return true; }
    }

    // Numeric: parse the leading number, then apply the unit the display used.
    char *end = nullptr;
    double v = strtod(txt, &end);
    if (end == txt) return false;                       // no name matched, no number
    for (const char *s = end; *s; ++s)                  // Cutoff prints kHz above 1000
        if ((s[0] == 'k' || s[0] == 'K') && (s[1] == 'h' || s[1] == 'H')) { v *= 1000.0; break; }
    if (!strcmp(d->unit, "%") && !d->stepped) v /= 100.0;
    *out = dclamp(v, d->minv, d->maxv);
    return true;
}
static void paramsFlush(const clap_plugin *plugin,
                        const clap_input_events_t *in,
                        const clap_output_events_t *out) {
    Plugin *p = SELF;
    if (in) {
        uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; i++) p->handleInEvent(in->get(in, i));
    }
    p->drainGuiQueue(out);
}
static const clap_plugin_params_t s_params = {
    paramsCount, paramsInfo, paramsValue,
    paramsValueToText, paramsTextToValue, paramsFlush,
};

// ---------------------------------------------------------- audio ports
static uint32_t portsCount(const clap_plugin *, bool) { return 1; }
static bool portsGet(const clap_plugin *, uint32_t index, bool is_input,
                     clap_audio_port_info_t *info) {
    if (index != 0) return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t s_ports = {portsCount, portsGet};

// ----------------------------------------------------------------- state
// CLAP streams may transfer fewer bytes than requested per call (stream.h):
// loop until the whole buffer moves, treating 0 (EOF/no-progress) or -1 (error)
// as failure so a legal short read/write is not mistaken for corruption.
static bool writeAll(const clap_ostream_t *os, const void *buf, uint64_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint64_t done = 0; done < n;) {
        int64_t r = os->write(os, p + done, n - done);
        if (r <= 0) return false;
        done += (uint64_t)r;
    }
    return true;
}
static bool readAll(const clap_istream_t *is, void *buf, uint64_t n) {
    uint8_t *p = (uint8_t *)buf;
    for (uint64_t done = 0; done < n;) {
        int64_t r = is->read(is, p + done, n - done);
        if (r <= 0) return false;
        done += (uint64_t)r;
    }
    return true;
}
static bool stateSave(const clap_plugin *plugin, const clap_ostream_t *os) {
    Plugin *p = SELF;
    uint32_t magic = 0x53575231, count = P_COUNT;     // "SWR1"
    if (!writeAll(os, &magic, 4)) return false;
    if (!writeAll(os, &count, 4)) return false;
    for (uint32_t i = 0; i < P_COUNT; i++) {
        double v = p->values[i].load();
        if (!writeAll(os, &i, 4)) return false;
        if (!writeAll(os, &v, 8)) return false;
    }
    return true;
}
static bool stateLoad(const clap_plugin *plugin, const clap_istream_t *is) {
    Plugin *p = SELF;
    uint32_t magic = 0, count = 0;
    if (!readAll(is, &magic, 4) || magic != 0x53575231) return false;
    if (!readAll(is, &count, 4)) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id; double v;
        if (!readAll(is, &id, 4)) return false;
        if (!readAll(is, &v, 8)) return false;
        if (id < P_COUNT) {
            const ParamDef *d = paramDef(id);
            p->values[id].store(dclamp(v, d->minv, d->maxv));
        }
    }
    p->uiDirty.store(1);
    return true;
}
static const clap_plugin_state_t s_state = {stateSave, stateLoad};

// ------------------------------------------------------------------- gui
static bool guiIsApiSupported(const clap_plugin *, const char *api, bool floating) {
    return !floating && !strcmp(api, GUI_NATIVE_API);
}
static bool guiGetPreferredApi(const clap_plugin *, const char **api, bool *floating) {
    *api = GUI_NATIVE_API; *floating = false; return true;
}
static bool guiCreate(const clap_plugin *plugin, const char *api, bool floating) {
    Plugin *p = SELF;
    if (floating || strcmp(api, GUI_NATIVE_API)) return false;
    if (!p->gui) p->gui = guiCreatePlatform(p);
    if (!p->gui) return false;
    // Prefer the host timer; fall back to an internal timer thread if the host
    // has no timer-support extension or refuses the registration, so the GUI
    // is never silently frozen.
    bool timerOk = false;
    if (p->hostTimer && p->guiTimer == CLAP_INVALID_ID)
        timerOk = p->hostTimer->register_timer(p->host, 33, &p->guiTimer);
    if (!timerOk) {
        p->guiTimer = CLAP_INVALID_ID;
        guiStartFallbackTimer(p->gui);
    }
    return true;
}
static void guiDestroyFn(const clap_plugin *plugin) {
    Plugin *p = SELF;
    if (p->guiTimer != CLAP_INVALID_ID && p->hostTimer) {
        p->hostTimer->unregister_timer(p->host, p->guiTimer);
        p->guiTimer = CLAP_INVALID_ID;
    }
    if (p->gui) {
        guiStopFallbackTimer(p->gui);   // no-op if the host timer was used
        guiDestroy(p->gui);
        p->gui = nullptr;
    }
}
static bool guiSetScale(const clap_plugin *, double) { return false; }
static bool guiGetSize(const clap_plugin *, uint32_t *w, uint32_t *h) {
    *w = GUI_W; *h = GUI_H; return true;
}
static bool guiCanResize(const clap_plugin *) { return false; }
static bool guiGetResizeHints(const clap_plugin *, clap_gui_resize_hints_t *) { return false; }
static bool guiAdjustSize(const clap_plugin *, uint32_t *w, uint32_t *h) {
    *w = GUI_W; *h = GUI_H; return true;
}
static bool guiSetSize(const clap_plugin *, uint32_t w, uint32_t h) {
    return w == GUI_W && h == GUI_H;
}
static bool guiSetParent(const clap_plugin *plugin, const clap_window_t *win) {
    Plugin *p = SELF;
    return p->gui ? guiAttach(p->gui, win) : false;
}
static bool guiSetTransient(const clap_plugin *, const clap_window_t *) { return false; }
static void guiSuggestTitle(const clap_plugin *, const char *) {}
static bool guiShow(const clap_plugin *plugin) {
    return SELF->gui ? guiSetVisible(SELF->gui, true) : false;
}
static bool guiHide(const clap_plugin *plugin) {
    return SELF->gui ? guiSetVisible(SELF->gui, false) : false;
}
static const clap_plugin_gui_t s_gui = {
    guiIsApiSupported, guiGetPreferredApi, guiCreate, guiDestroyFn,
    guiSetScale, guiGetSize, guiCanResize, guiGetResizeHints,
    guiAdjustSize, guiSetSize, guiSetParent, guiSetTransient,
    guiSuggestTitle, guiShow, guiHide,
};

// ------------------------------------------------------------- timer ext
static void onTimer(const clap_plugin *plugin, clap_id id) {
    Plugin *p = SELF;
    if (p->gui && id == p->guiTimer) guiTick(p->gui);
}
static const clap_plugin_timer_support_t s_timer = {onTimer};

// ----------------------------------------------------------------- glue
static const void *plugGetExtension(const clap_plugin *, const char *id) {
    if (!strcmp(id, CLAP_EXT_PARAMS))        return &s_params;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))   return &s_ports;
    if (!strcmp(id, CLAP_EXT_STATE))         return &s_state;
    if (!strcmp(id, CLAP_EXT_GUI))           return &s_gui;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &s_timer;
    return nullptr;
}
static void plugOnMainThread(const clap_plugin *) {}

static const clap_plugin_t *factoryCreate(const clap_plugin_factory *,
                                          const clap_host_t *host,
                                          const char *plugin_id) {
    if (strcmp(plugin_id, s_desc.id)) return nullptr;
    Plugin *p = new Plugin(host);
    p->plug.desc = &s_desc;
    p->plug.plugin_data = p;
    p->plug.init = plugInit;
    p->plug.destroy = plugDestroy;
    p->plug.activate = plugActivate;
    p->plug.deactivate = plugDeactivate;
    p->plug.start_processing = plugStartProcessing;
    p->plug.stop_processing = plugStopProcessing;
    p->plug.reset = plugReset;
    p->plug.process = plugProcess;
    p->plug.get_extension = plugGetExtension;
    p->plug.on_main_thread = plugOnMainThread;
    return &p->plug;
}
static uint32_t factoryCount(const clap_plugin_factory *) { return 1; }
static const clap_plugin_descriptor_t *factoryDesc(const clap_plugin_factory *,
                                                   uint32_t i) {
    return i == 0 ? &s_desc : nullptr;
}
static const clap_plugin_factory_t s_factory = {factoryCount, factoryDesc, factoryCreate};

static bool entryInit(const char *) { return true; }
static void entryDeinit() {}
static const void *entryGetFactory(const char *id) {
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &s_factory;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory,
};
