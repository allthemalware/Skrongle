# Skrongle

A glitch / stutter-delay CLAP plugin. Probability-sequenced freeze delay with
pitch-wobble, resonant stereo filter, bitcrush, reverse, tape-stop and ratchet
mayhem — wrapped in a broken-screen UI that melts harder the more the audio
glitches.

No frameworks: pure C++17, a software-rendered GUI, and the CLAP headers.

```
src/plugin.cpp            CLAP shell: params, ports, state, GUI + timer glue
src/params.hpp            parameter table (49 params)
src/dsp.hpp               the engine
src/gui/gui_core.cpp      software renderer: widgets + broken-screen FX
src/gui/platform_x11.cpp  Linux backend
src/gui/platform_win32.cpp  Windows backend
src/gui/platform_macos.mm   macOS backend
tools/smoketest.cpp       headless plugin validator
tools/guitest.cpp         headless GUI render test
```

## How it maps to the Bitwig Grid technique

The plugin replicates (and extends) the classic Grid stutter patch:

- **Probability step sequencer → Blend.** Each of up to 16 steps has its own
  glitch chance. Defaults keep downbeats at 0% so the kick always lands clean.
  When a step fires, the wet blend snaps to `GLT BLEND`, the delay **input is
  muted** (`KILL IN`) and **feedback pins to `FREEZE`** (default 100%) — the
  delay freezes and loops whatever it last captured.
- **Random S&H pitch wobble.** A bipolar sample-and-hold LFO modulates delay
  time. `SMOOTH` is the cosine crossfade between held values — 0 gives hard
  pitch jumps, 1 gives gliding tape-warble. Only audible while a glitch is
  active, exactly like the mod-delay trick in the video.
- **Stereo-inverted filter mod.** The input runs through a resonant SVF
  (LP/BP/HP/Notch, default BP at high resonance). An LFO sweeps the cutoff and
  the right channel's modulation is **inverted** — the stereo split/invert/
  merge move from the video.
- **Quantizer / crusher.** Bit-depth + sample-rate reduction with a mix knob,
  the "optional quantizer" stage.

## Beyond the video

- **REVERSE** and **TAPE-STOP** glitch modes (per-step random pick, weighted
  by `CHAOS`) — the frozen buffer plays backwards or spins down in pitch.
- **RATCHET** — re-grabs the freeze at quarter-step slots for machine-gun fills.
- **CHAOS** macro — raises glitch probability and skews mode selection wilder.
- **PANIC** — hold to force a glitch right now, regardless of the sequencer.
- **Swing**, triplet/dotted sync rates, ping-pong feedback, wet drive,
  dry ducking, output trim.
- **DICE / CLEAR** — randomize (downbeat-aware) or wipe the pattern.
- **SCREEN DAMAGE** knob — the UI's pixel-sorting melt, tear lines, cracked
  glass and dead pixels scale with this knob *times the live glitch envelope*,
  so the interface wrecks itself in time with the audio.

## UI quick reference

- Knobs: drag vertically; **Shift** = fine; **double-click** = default;
  mouse wheel works.
- Step bars: click/paint probabilities; LEDs show the running step.
- Combos: click cycles forward, right-click cycles back.
- Footer strip always shows the hovered/dragged parameter's exact value.

## Building

The CLAP headers are vendored in `clap/` (MIT). No other dependencies beyond
your platform SDK.

### Linux

```sh
sudo apt install build-essential libx11-dev   # or distro equivalent
make            # builds build/skrongle.clap + the smoketest
make test       # headless validation
make install    # copies to ~/.clap
```

Or with CMake: `cmake -B build && cmake --build build`.

### macOS

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/skrongle.clap ~/Library/Audio/Plug-Ins/CLAP/
```

(System-wide path: `/Library/Audio/Plug-Ins/CLAP/`.)

### Windows (MSVC)

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
copy build\Release\skrongle.clap "%COMMONPROGRAMFILES%\CLAP\"
```

MinGW works too (`-G "MinGW Makefiles"`).

## Usage tips

- Start with drums looping, tempo-synced host. Defaults are the video patch:
  1/16 steps, 60 ms delay, BP filter at high resonance.
- Raise `PITCH DEPTH` + lower `SMOOTH` for hard pitch-jump stutters; raise
  `SMOOTH` for the smeared warble sound.
- Turn `CHAOS` past ~0.6 and the reverses/tape-stops start taking over.
- `DUCK DRY` keeps the dry drums out of the way while a glitch rings.
- It's a delay-freeze, not a slicer: it sounds best when the input keeps
  playing so each glitch grabs fresh material.

## License

Source: MIT. Vendored CLAP headers: MIT (see `clap/LICENSE`).
Bitmap font: `font8x8` by Daniel Hepper, public domain.
