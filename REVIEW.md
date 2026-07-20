# Mangle & Skrongle — Code Review, Roadmap & Cross-Pollination

A full review of two CLAP audio plugins:

- **Mangle** — granular texture instrument, Rust / nih-plug (`/home/voyd/git/Mangle/mangle (3)`)
- **Skrongle** — glitch / stutter freeze-delay, C++17, no framework (this repo)

plus a two-way analysis of what each codebase should borrow from the other.

> **Method & confidence.** This combines direct code reading of most of both
> codebases with a multi-agent review pass. Findings marked **[confirmed]** were
> verified firsthand; **[reviewer]** findings come from a single automated
> reviewer whose cited evidence checks out on inspection but was not independently
> cross-verified. All defect locations were current as of this review.

---

## Fixes applied this session

All **critical and major** findings below have been fixed in-tree (minor/nit
findings and the roadmaps are left for later). Summary:

### Mangle
1. **[critical] Loop-point clamp panic → host crash.** `Loop Start = Loop End = 100%`
   drove a `clamp(min > max)` on the audio thread. Fixed in `crates/mangle/src/lib.rs`
   and `crates/mangle-dsp/src/engine.rs` (`resolve_window`), with a regression test.
2. **[major] NaN sticks the Squeeze leveler forever.** Added a non-finite guard in
   `squeeze.rs` + a recovery test.
3. **[major] Same NaN bug in the output EnvFollower** (and `.min(1.0)` masked it as a
   constant 1.0). Guarded in `modengine.rs` + a test.
4. **[major] RT-unsafe pending-load drain.** Allocation + blocking `RwLock` writes on the
   audio thread moved to the GUI thread; the drain is now a pure pointer swap.
5. **[major] Voice-steal click.** Stolen voices now fade out over ~5 ms (`Lane::soft_reset`
   + a fade pool) instead of a hard clear; regression test added.
6. **[major] Unsmoothed master Level / Stereo width.** Now one-pole smoothed (~5 ms) per
   sample, matching the parameter-map contract.
7. **[major] Note-expression Volume/Pan unhandled.** Added `PolyVolume`/`PolyPan` handling
   (per-voice gain + balance pan).

### Skrongle
8. **[major] `atof`-only `text_to_value`.** Now reverse-matches the display strings
   (`1/16`, `BANDPASS`, `ON`, `1.20 kHz`, …) before numeric parsing. Verified: all 49
   params round-trip `value_to_text → text_to_value`.
9. **[major] No denormal guard.** Added an FTZ/DAZ RAII guard around `plugProcess`.
10. **[major] GUI silently dead without host timer-support.** `register_timer` result is
    now checked; a fallback ~30 Hz timer thread drives the GUI otherwise (X11 made
    thread-safe via `XInitThreads`; win32/macOS covered too, macOS dispatches to the main
    queue).
11. **[major] State I/O treated legal short reads/writes as fatal.** Added looping
    `readAll`/`writeAll` helpers per the CLAP stream contract.
12. **[major] macOS built a flat dylib, not a `.clap` bundle.** CMake now assembles
    `skrongle.clap/Contents/{MacOS,Info.plist,PkgInfo}` on Apple; also defaults to a Release
    build and links `Threads`.

**Verification:** `cargo test -p mangle-dsp` (103 pass) + `-p mangle-loader` (10 pass) and
`cargo build -p mangle` clean; Skrongle `make && make test` pass under both Make and CMake,
plus a dlopen round-trip check of `text_to_value` across all 49 params.

---

## Mangle

Granular texture instrument. Four engine modes (Granular / Stretch / Spectral / a
Plaits-style reinterpretation of one fixed control set), a 380-parameter append-only table,
a 32-slot stereo-diverging mod matrix, and a phase-vocoder Spectral mode with identity phase
locking and cepstral formant preservation.

### Strengths
- The grain **snapshot rule** is applied cleanly and consistently — everything a grain needs
  is frozen at spawn, so no per-sample parameter read can glitch a grain mid-flight; the
  envelope coefficients even skip `powf` at the default Shape.
- The **Spectral mode** is well beyond a typical v1 phase vocoder: Laroche–Dolson identity
  phase locking, cepstral formant preservation, note-on priming for zero-latency onset, a
  mono shared-vocoder fast path, and a proper overlap-add release drain — over a 162-line
  dependency-free FFT that checks out against a direct DFT.
- **Squeeze** (output leveler + limiter) is small, honest, RT-safe, and genuinely
  zero-lookahead; the inter-sample-peak tradeoff is documented rather than hidden.
- The **loader resampler** is a correct alias-filtered arbitrary-ratio design: kernel
  stretched by ratio when decimating, per-phase unity-DC normalization, symmetric kernel
  (zero phase offset), verified by a 30 kHz alias-rejection test.
- The 380-param **ID scheme** (`module<<16 | instance<<8 | index`) leaves clean append room
  everywhere; the state format has forward/backward-compat tests. Growth was designed in.
- The **RANDOM engine** is a real feature: per-unit locks that also fence the matrix
  generator, amount-as-morph semantics, and a seeded RNG making rolls recallable.

### Outstanding minor / nit findings (not fixed)
- **[confirmed] `mono_overrides` / `set_mod_offset` silently drop beyond capacity** (256 /
  128) — no warning when modulation rows exceed the cap.
- **[confirmed] Transport-locked LFO phase jumps on tempo change** (`pos_seconds × rate_hz`).
- **[reviewer] Squeeze bypass is an unramped ~15.6 dB step** with stale state on re-engage.
- **[reviewer] Gapless decode disabled** — mp3 encoder delay/padding sits in the default loop.
- **[reviewer] Failed re-load after a host rate change leaves a stale, silently pitch-shifted slot.**
- **[reviewer] Multichannel decode transiently buffers every channel** up to the 300 s cap.
- **[reviewer] §7 roadmap unimplemented** (relink dialog, embed-samples); missing-file / hash
  mismatch is log-only, invisible in the GUI.
- **[reviewer] GUI-thread synchronous decode+resample** freezes the editor on long files.
- **Test-coverage gaps:** reverse (negative Direction) in Stretch/Spectral; mid-note Mode
  switching (leaving Spectral mid-ring pins the voice alive); decode limited to self-written
  float WAV; loose resampler SNR thresholds; Squeeze non-finite / downward-leg / SR-invariance.
- **[reviewer] GUI load before first activate** resamples to rate 0 → misleading "zero samples".

### Improvement roadmap (31 ideas, by area)

**Signal chain (Mangle has no filter/reverb/delay/saturation at all):**
- Per-engine state-variable filter (LP/BP/HP), matrix-modulatable cutoff — *high / M*.
- Clouds-style diffuser + reverb as the first post-FX module — *high / M*.
- Shimmer/feedback bus (a 7th "BUS" slot recording Mangle's own output) — *high / L*.
- Tempo-synced feedback delay — *medium / M*. Drive/saturation inside Squeeze — *medium / S*.

**Playability & modulation (no envelope generator; continuous pitch never lands in tune):**
- Per-voice attack/release envelope on the note gate — *high / M*.
- One-shot flag on LFOs → the existing 256-pt curves become drawable MSEGs — *high / S*.
- Pitch quantize / scale lock per engine — *high / S* (cheapest way to make RANDOM musical).
- Global FREEZE latch — *high / S*. Per-grain reverse/octave probability — *medium / S*.
- Euclidean emission clock — *medium / M*. MPE completion / microtuning / multi-out — *medium / M*.

**New engine modes:** SpectralMorph (two-slot spectral interpolation) — *high / L*;
Scan (wavetable-style single-cycle reading) — *medium / M*.

**Workflow & presets (largest multipliers):** preset system + in-GUI browser + CLAP
preset-discovery — *high / L*; live-input recording mode — *high / L*; RANDOM history/undo +
A/B (and fix REROLL, which currently doesn't recall) — *high / M*; embed-samples + relink
(§7) — *high / L*; slot utilities, mode-aware dead-cell dimming, skip redundant re-decode,
surface loader warnings — *medium / S each*.

**Squeeze & output polish:** asymmetric leveler envelope — *medium / S*; zero-latency ISP
safety stage — *medium / M*; GR metering — *medium / S*; document/tame Level-after-limiter,
Squeeze amount macro — *low*.

**Loader:** polyphase resampler table — *medium / M*; read WAV/AIFF loop+root metadata —
*medium / M*; surround downmix instead of channel-drop — *low / S*.

---

## Skrongle

Glitch / stutter freeze-delay, raw CLAP, pure C++17. A probability step sequencer driving a
freeze delay with pitch wobble, resonant stereo filter, bitcrush, reverse, tape-stop and
ratchet — wrapped in a broken-screen software UI that melts in time with the audio.

### Strengths
- Solid raw-CLAP shell: textbook GUI→audio SPSC param queue with correct begin/perform/end
  gesture forwarding, lock-free atomics for all 49 values, forward-compatible id/value state.
- RT-safe throughout — no allocations/locks in `process()`, tanh soft-clip *inside* the
  feedback loop so 100% freeze feedback is unconditionally bounded, ~3 ms smoothing so glitch
  transitions don't click.
- Careful clocking: sample-accurate step boundaries, transport-following with a
  phase-continuous free-run fallback, and musical defaults (downbeats clean at 0%).
- `tools/smoketest.cpp` is a genuinely useful dlopen soak harness.
- The audio-reactive broken-screen UI is a real differentiator: damage scales with the knob
  *times* the live glitch envelope; the footer always shows the exact hovered value.

### Outstanding minor / nit findings (not fixed)
- **[confirmed] Events handled per-block, timestamps ignored** — no sample-accurate splitting;
  automation edges land up to a block late (matters for PANIC / step rate in a timing plugin).
- **[reviewer] `reset()` runs `engine.activate` on the audio thread** (`vector::assign`) —
  benign today but relies on the frame count never changing.
- **[reviewer] `paramDef()` lazy init uses a non-atomic bool guard** reachable from audio.
- **[reviewer] GUI param-queue overflow is silent** — a dropped `GESTURE_END` can strand host
  automation recording.
- **[reviewer] Momentary PANIC (`P_FORCE`) is persisted in state** — a project saved mid-PANIC
  reloads force-glitching.
- **Test gaps:** the smoketest state "roundtrip" never mutates/compares values; no input
  events / `flush` / `text_to_value` are exercised; `guitest.cpp` isn't built by either build
  system; guitest's "PANIC" click actually lands on the TAPESTOP knob.
- **[nit] DAMAGE knob label half-clipped off the top of the window** (placed at y = −6).
- **[nit] Placeholder descriptor metadata** (example.com URL, empty manual/support URLs).
- **[nit] Dead placeholder arithmetic** (`* 0 - 0`, `-= 0.0`) in `drawKnob` and the tape-stop branch.

### Improvement roadmap (24 ideas, by area)

The gap to dBlue Glitch / Effectrix / Stutter Edit is feature depth, not quality.

**Sync & sequencing:** tempo-synced loop/stutter length — *high / S* (repeats never land on
the grid today); per-step mode/length/pitch lanes — *high / L*; pattern pages/scenes with
quantized switching — *high / M*; Euclidean fill + graded DICE, step conditions,
time-signature awareness — *medium*.

**Glitch modes:** pitched repeats + per-repeat pitch ramps — *high / M*; gate/chop, granular
shuffle, buffer scrub — *high / L*; a tape-stop that actually stops + vinyl-brake variant —
*medium / S*; keep recording through reverse/tape-stop steps — *medium / S*.

**Performance & MIDI:** MIDI note triggering (no note ports today) — *high / M*; note port for
PANIC — *medium / M*; PANIC quantize/latch/choke — *medium / S*.

**Presets & modulation:** preset system + factory bank — *high / M*;
`CLAP_PARAM_IS_MODULATABLE` (the most on-brand feature for a Bitwig-heritage plugin) —
*high / M*; internal envelope follower → probability/CHAOS — *medium / M*.

**Mix / UI / shell:** master mix + wet-solo — *medium / M*; buffer waveform visualizer with
read/write heads — *medium / M*; posix-fd + HiDPI scale — *medium / M*;
`CLAP_PARAM_IS_ENUM` for the two enums, `CLAP_EXT_TAIL`, constant-mask/silence flags, build
hygiene — *low–medium*.

---

## Cross-pollination

Every transfer is an **algorithm/design port** — never FFI. Both are small, dependency-free
codebases, and each unit translates by hand.

### Skrongle → Mangle (13 transfers)
The big three: Skrongle's ~30-line **Cytomic SVF** fills Mangle's most glaring gap (no filter
anywhere), and its stereo-inverted filter-mod trick comes along free via Mangle's per-channel
matrix; Skrongle's **dlopen CLAP smoketest** is exactly the host-integration harness Mangle's
BUILD_NOTES admits it lacks; the **probability step sequencer** maps onto Mangle as a new
matrix source routed to the already-existing Grain Gate destination — a groove machine with no
engine surgery.

| Transfer | Impact / effort |
|---|---|
| Resonant SVF master filter (Cutoff/Reso as matrix dests) | high / M |
| Headless CLAP host smoketest (libloading + clap-sys) | high / M |
| Probability step sequencer as a mod-matrix source | high / L |
| PANIC-style momentary FREEZE override (Direction→0) | medium / S |
| Bitcrusher / downsampler master FX (fractional-hold Crusher) | medium / S |
| Audio-reactive UI (engine→GUI atomics + asymmetric smoothing) | medium / M |
| Tape-stop spin-down as a Direction brake | medium / M |
| Smoothed random S&H mod source | medium / M |
| Feedback / ping-pong freeze delay post-FX (tanh-bounded loop) | medium / M |
| Headless GUI render test (egui_kittest) | medium / M |
| Sidechain duck, reframed for an instrument (aux input + SC-Env source) | medium / M |
| Footer value readout strip + double-click-to-default | low / S |
| Swing on tempo-synced grain emission | low / S |

### Mangle → Skrongle (12 transfers)
Highest impact: **Hermite/Sinc-8 delay-line interpolation** upgrades every glitch voice's core
sound (pitch wobble, reverse, tape-stop are all fractional linear-interp reads today); the
80-line **Squeeze limiter** is a ready-made output governor for 100%-feedback freezes; Mangle's
**denormal-flush + NaN hygiene** addresses a real bug class here (one NaN in the frozen buffer
recirculates forever) — *now partly addressed by the FTZ guard fix above.*

| Transfer | Impact / effort |
|---|---|
| Hermite / Sinc-8 fractional-read interpolation for glitch reads | high / M |
| Squeeze limiter as an output safety stage | high / S |
| Denormal flush + NaN quarantine (partly done: FTZ guard) | high / S |
| Seeded reproducible RNG stored in state (recallable DICE, deterministic bounces) | high / M |
| Real spectral-freeze glitch mode via a small phase vocoder | high / L |
| Versioned append-only state blob with a non-param section (enables seeds/locks/curves) | medium / S |
| Per-section DICE locks + Amount/Complexity randomizer semantics | medium / M |
| Mod-matrix-lite (glitch envelope + env follower as sources) | medium / M |
| 31-division tempo-sync table + synced wobble/filter rates | medium / S |
| Drawable 256-pt curves for the pitch-wobble shape | medium / L |
| Parameter-map contract doc + explicit append-only ID rule | medium / S |
| Fixed control-rate tick for coefficients/modulation (stop calling `tan()` twice/sample) | low / S |
