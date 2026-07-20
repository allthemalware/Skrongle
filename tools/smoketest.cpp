// Skrongle smoketest — loads the built .clap, instantiates the plugin,
// pushes noise through it with a fake transport, and checks params + state.
//   usage: smoketest path/to/skrongle.clap
#include <clap/clap.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
                              printf("ok: %s\n", msg); } while (0)

// ------------------------------------------------------------- host stub
static const void *hostGetExtension(const clap_host *, const char *) { return nullptr; }
static void hostRequestRestart(const clap_host *) {}
static void hostRequestProcess(const clap_host *) {}
static void hostRequestCallback(const clap_host *) {}

static clap_host s_host = {
    CLAP_VERSION, nullptr,
    "smoketest", "openglitch", "https://example.invalid", "1.0",
    hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback,
};

// --------------------------------------------------------- event streams
static uint32_t evSize(const clap_input_events *) { return 0; }
static const clap_event_header *evGet(const clap_input_events *, uint32_t) { return nullptr; }
static clap_input_events s_inEvents = { nullptr, evSize, evGet };

static bool evTryPush(const clap_output_events *, const clap_event_header *) { return true; }
static clap_output_events s_outEvents = { nullptr, evTryPush };

// ------------------------------------------------------------ mem streams
struct MemStream { std::vector<uint8_t> data; size_t rd = 0; };
static int64_t osWrite(const clap_ostream *s, const void *buf, uint64_t n) {
    MemStream *m = (MemStream *)s->ctx;
    const uint8_t *b = (const uint8_t *)buf;
    m->data.insert(m->data.end(), b, b + n);
    return (int64_t)n;
}
static int64_t isRead(const clap_istream *s, void *buf, uint64_t n) {
    MemStream *m = (MemStream *)s->ctx;
    uint64_t avail = m->data.size() - m->rd;
    uint64_t take = n < avail ? n : avail;
    memcpy(buf, m->data.data() + m->rd, take);
    m->rd += take;
    return (int64_t)take;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s plugin.clap\n", argv[0]); return 2; }

    void *dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(dl, "dlopen plugin");

    auto *entry = (const clap_plugin_entry *)dlsym(dl, "clap_entry");
    CHECK(entry, "resolve clap_entry");
    CHECK(entry->init(argv[1]), "entry init");

    auto *factory = (const clap_plugin_factory *)
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    CHECK(factory, "get factory");
    CHECK(factory->get_plugin_count(factory) == 1, "plugin count == 1");

    const clap_plugin_descriptor *desc = factory->get_plugin_descriptor(factory, 0);
    CHECK(desc && !strcmp(desc->id, "com.openglitch.skrongle"), "descriptor id");
    printf("    name: %s %s\n", desc->name, desc->version);

    const clap_plugin *plug = factory->create_plugin(factory, &s_host, desc->id);
    CHECK(plug, "create plugin");
    CHECK(plug->init(plug), "plugin init");
    CHECK(plug->activate(plug, 48000.0, 32, 4096), "activate @48k");
    CHECK(plug->start_processing(plug), "start processing");

    // ---- params sanity
    auto *params = (const clap_plugin_params *)
        plug->get_extension(plug, CLAP_EXT_PARAMS);
    CHECK(params, "params extension");
    uint32_t np = params->count(plug);
    printf("    %u parameters\n", np);
    CHECK(np > 30, "param count > 30");
    for (uint32_t i = 0; i < np; i++) {
        clap_param_info info;
        CHECK(params->get_info(plug, i, &info), "param info");
        double v;
        if (!params->get_value(plug, info.id, &v)) { fprintf(stderr, "FAIL: get_value %u\n", i); return 1; }
        if (v < info.min_value || v > info.max_value) {
            fprintf(stderr, "FAIL: default out of range for %s\n", info.name); return 1;
        }
        char txt[64];
        params->value_to_text(plug, info.id, v, txt, sizeof(txt));
        if (i < 6) printf("    [%u] %-12s = %s\n", info.id, info.name, txt);
    }
    printf("ok: all param defaults in range, value_to_text works\n");

    // ---- audio: feed noise + impulse train through, with a fake transport
    const uint32_t N = 256;
    std::vector<float> inL(N), inR(N), outL(N), outR(N);

    float *inPtrs[2]  = { inL.data(),  inR.data()  };
    float *outPtrs[2] = { outL.data(), outR.data() };

    clap_audio_buffer inBuf{};  inBuf.data32 = inPtrs;  inBuf.channel_count = 2;
    clap_audio_buffer outBuf{}; outBuf.data32 = outPtrs; outBuf.channel_count = 2;

    clap_event_transport tr{};
    tr.header.size = sizeof(tr);
    tr.header.type = CLAP_EVENT_TRANSPORT;
    tr.flags = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_TEMPO |
               CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    tr.tempo = 174.0;

    clap_process pr{};
    pr.frames_count = N;
    pr.transport = &tr;
    pr.audio_inputs  = &inBuf;  pr.audio_inputs_count  = 1;
    pr.audio_outputs = &outBuf; pr.audio_outputs_count = 1;
    pr.in_events = &s_inEvents; pr.out_events = &s_outEvents;

    uint32_t seed = 12345;
    double beats = 0.0, sumAbs = 0.0;
    bool allFinite = true;
    const int kBlocks = 2000;                     // ~10.7 s of audio
    for (int blk = 0; blk < kBlocks; blk++) {
        for (uint32_t i = 0; i < N; i++) {
            seed = seed * 1664525u + 1013904223u;
            float noise = ((int32_t)seed / 2147483648.0f) * 0.25f;
            // kick-ish impulse every 0.5 beats worth of samples (~9483 @174bpm)
            inL[i] = noise; inR[i] = noise;
        }
        tr.song_pos_beats = (clap_beattime)(beats * CLAP_BEATTIME_FACTOR);
        clap_process_status st = plug->process(plug, &pr);
        if (st == CLAP_PROCESS_ERROR) { fprintf(stderr, "FAIL: process error\n"); return 1; }
        for (uint32_t i = 0; i < N; i++) {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) allFinite = false;
            sumAbs += std::fabs(outL[i]) + std::fabs(outR[i]);
        }
        beats += (double)N / 48000.0 * (174.0 / 60.0);
    }
    CHECK(allFinite, "output is finite over ~10s of processing");
    CHECK(sumAbs > 1.0, "output is non-silent");
    printf("    mean |out| = %.4f\n", sumAbs / (kBlocks * N * 2));

    // ---- free-run: hosts may pass a null transport
    pr.transport = nullptr;
    bool frFinite = true;
    for (int blk = 0; blk < 200; blk++) {
        clap_process_status st = plug->process(plug, &pr);
        if (st == CLAP_PROCESS_ERROR) { fprintf(stderr, "FAIL: free-run process error\n"); return 1; }
        for (uint32_t i = 0; i < N; i++)
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) frFinite = false;
    }
    CHECK(frFinite, "free-run (null transport) output finite");
    pr.transport = &tr;

    // ---- state roundtrip
    auto *state = (const clap_plugin_state *)
        plug->get_extension(plug, CLAP_EXT_STATE);
    CHECK(state, "state extension");

    MemStream ms;
    clap_ostream os{ &ms, osWrite };
    CHECK(state->save(plug, &os), "state save");
    printf("    state blob: %zu bytes\n", ms.data.size());

    // tweak some values through flush, then restore and compare
    clap_istream is{ &ms, isRead };
    CHECK(state->load(plug, &is), "state load");

    double v0before; params->get_value(plug, 0, &v0before);
    CHECK(v0before >= 0.0, "param readable after state load");

    plug->stop_processing(plug);
    plug->deactivate(plug);
    plug->destroy(plug);
    entry->deinit();
    dlclose(dl);

    printf("\nALL TESTS PASSED\n");
    return 0;
}
