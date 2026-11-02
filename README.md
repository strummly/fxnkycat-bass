# fxnkycat bass (VST3 / AU)

A 6-stage bass processing chain in one plugin, built with JUCE 8. Based on fxnkycat vox.

| # | Stage | Controls | Fixed |
|---|-------|----------|-------|
| – | Timing (top bar) | Scrollable note value: 2 bar … 1/16t (default 1/4) | Tempo comes from the project automatically (120 BPM if the host doesn't report one). Sets the compressor's release and the limiter's release (1/4 of the Timing note, so a 1/16 note at the default) |
| 1 | Opto Comp (LA-2A style) | Ratio (stacked: 4:1 top, 3:1 [default], 2:1 bottom), Peak Reduction (0–100, starts at 0) | Stereo-linked, RMS detection, 6 dB soft knee. Peak Reduction sets the threshold (0 to -40 dB). Attack 50 ms. Two-stage, program-dependent release timed from the tempo: a fast part over 1/2 of the Timing note, plus a slow part that builds during sustained compression and lets go over 4 x the note. Gain matched: the output is kept as loud as the input (~1 s averages, 0 to +24 dB, held during silence). Meter shows gain reduction |
| 2 | Harmonics | Sub Bass, Tape, Overdrive (left to right); all start at 0 | The three run side by side on the same input and are then added together: output = Tape + Sub Bass layer + Overdrive layer. Tape (main path; at 0 it is the dry signal): asymmetric tape-style soft clipping (up to +18 dB drive, level-compensated), small 90 Hz head bump and top-end roll-off. Sub Bass and Overdrive are both made from the fundamental only (the note, isolated below 300 Hz and tracked by its zero crossings), not the string's upper harmonics. Sub Bass: a pure sine exactly one octave below the fundamental, phase-locked to the note so it follows note changes, with a shallow low-pass (6 dB/oct) at the Stereo stage's Mono Below frequency (it follows that setting even when the Stereo stage is off); its level follows the playing, and at 100 % it is about as loud as the fundamental. Overdrive (a gentle saturation): the fundamental, at a steady level, pushed softly (+6 dB) into a slightly biased tube-style curve that adds mostly warm low harmonics; then filtered 12 dB/oct each side: a high-pass at the Stereo stage's Mono Below frequency (200 / 250 / 300 Hz; it follows that setting even when the Stereo stage is off) and a low-pass that moves with the Overdrive knob, from 2 kHz near 0 % down to 800 Hz at 100 %. Its level follows the playing (100 % = the layer peaks about as high as the note). Three meter bars, one per knob: left = Sub Bass, middle = Tape, right = Overdrive |
| 3 | Vintage EQ (Pultec EQP-1A style) | Low Freq (20/30/60/100), Low Boost, Low Atten, High Freq (3/4/5/8/10/12/16k), High Boost, Bandwidth, Atten Select (5/10/20k), High Atten | Knobs are 0–10 like the hardware |
| 4 | Limiter | Threshold (-24 to 0 dB, starts at 0) | After the Harmonics and Vintage EQ. Stereo-linked brickwall peak limiter, 2 ms look-ahead (no overshoot), release = 1/4 of the Timing note. Turning the threshold down pushes the level into it: peaks always land at the -0.3 dBFS ceiling. The look-ahead is reported to the DAW and is always there, so the latency never changes when the stage is switched |
| 5 | Stereo | Mono Below: 300 / 250 [default] / 200 Hz (top to bottom) | Everything below the frequency is made mono and centred; above it the stereo image is untouched. Mid/side with a 24 dB/oct Linkwitz-Riley crossover (mid and side stay in phase). Meter shows how much low-end stereo is being folded to the centre. Mono tracks pass straight through |
| 6 | Filters | High pass (0 Hz–1 kHz; 0 = off), Low pass (500 Hz–20 kHz) | 24 dB/octave Butterworth each; default fully open (0 Hz / 20 kHz) |

Each stage's meter bar shows how much of its effect is happening, never just a volume change: dynamics stages show their gain reduction, and the others show what they add or change in the sound with any plain level change taken out.

Click a stage's name or its LED to turn it on (gold) or off (grey). Clicking, dragging or scrolling any control in a stage that is off turns it on. All stages start OFF.

**Top bar**
- **PRESET** menu + **SAVE**: "-INIT-" at the top resets every setting to its default (bypass untouched), then 9 user slots, shared by every project and DAW. Pick a slot to load it; SAVE asks for a name and a slot. Files live in `%APPDATA%\fxnkycat bass\Presets` (Windows) or `~/Library/Application Support/fxnkycat bass/Presets` (Mac).
- **Colour arrow** (right of SAVE): accent colour - Gold, Red, Blue, Green or Purple. Shared by every copy of the plugin and remembered (saved next to the presets).
- **BYPASS**: turns the whole plugin off (input passes straight through, with a short crossfade so it never clicks). Also works as the DAW's own bypass for the plugin. Presets don't change it.
- **AUTO GAIN**: trims the output so it stays about as loud as the input (~1 s average, ±18 dB, holds during silence).
- **INPUT HPF**: high-passes the signal before stage 1 (0 Hz = Off, up to 500 Hz, 24 dB/octave). The analyzer's BEFORE trace is taken ahead of it.
- **INPUT LPF**: low-passes the signal before stage 1 (500 Hz–20 kHz, 24 dB/octave; all the way right = Off).
- **HARMONIC/EQ SWAP** light: off (default) = Harmonics is stage 2, then the Vintage EQ (3); on = Vintage EQ first, then the Harmonics. The two panels trade places to match. The Limiter always comes after both.
- **TIMING** and the project tempo readout.

A spectrum analyzer across the top overlays the plugin's input (BEFORE, grey) and output (AFTER, gold), 20 Hz–20 kHz, -96 to 0 dB.

**External plugin insert** (strip at the top-left of the analyzer): host one VST3 inside the chain.
- **LOAD VST3...** opens a browser of the VST3s in the standard folders: a **search** box (matches name or maker), **A-Z** or **MAKER** sorting, and **BROWSE...** for any other .vst3. Click a plugin to load it; its window opens and the INSERT light switches on. **OPEN** shows the window again, **REMOVE** empties the slot. Makers are read from the plugins' own files (never by loading them), in the background the first time, and remembered in `plugin-makers.xml` next to the presets.
- **INSERT** light: off by default; switches on when a plugin is added (or click it). On = the plugin processes; off = it is bypassed (its own bypass, so delay compensation is kept).
- **AFTER** menu: which stage it comes after, by name - Compressor (default), Harmonics, Vintage EQ, Limiter, Stereo. (Harmonics / Vintage EQ follow those stages in either HARMONIC/EQ SWAP order. For after the Filters, put the plugin after fxnkycat bass in the DAW.)
- The plugin's processing delay is reported to the DAW (added to the limiter's look-ahead). The plugin and its settings are saved with the session (not in presets). A session whose plugin isn't installed opens with the slot empty.
- The hosted plugin runs inside fxnkycat bass: if it crashes, the DAW goes with it. Some copy-protected plugins refuse to load inside another plugin.
The window is resizable from the bottom-right corner.

## Build (Windows)

Needs: Visual Studio 2022 (or Build Tools) with "Desktop development with C++", and CMake 3.22+.
JUCE is downloaded automatically on the first configure.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\BassChain_artefacts\Release\VST3\fxnkycat bass.vst3`.
Copy it into `C:\Program Files\Common Files\VST3\` and rescan plugins.
If the project folder path is very long, build from a short path (e.g. `subst V: <folder>`), or MSBuild fails with FTK1011.

## Build (Mac) — VST3 + AU, universal (Apple Silicon + Intel)

### Option A: GitHub Actions (no Mac needed)

1. Put this folder in a GitHub repository (the `.github/workflows/build-mac.yml` file must be at the repo root).
2. On GitHub, open **Actions → Build Mac → Run workflow** (it also runs on every push to `main`).
3. When it finishes (~10–15 min), download **fxnkycat-bass-mac** from the run's **Artifacts** section.
   Unzip it once to get `fxnkycat-bass-mac.zip` — share that file.

### Option B: on a Mac

Needs Xcode (from the App Store) and CMake (`brew install cmake`).

```
cmake -B build -G Xcode
cmake --build build --config Release
```

Output:
- `build/BassChain_artefacts/Release/VST3/fxnkycat bass.vst3`
- `build/BassChain_artefacts/Release/AU/fxnkycat bass.component`

### Gatekeeper

The Mac build is ad-hoc signed, not notarized, so users must run the `xattr` commands in
`installer/mac/HOW TO INSTALL.txt` once. Removing that step needs an Apple Developer account ($99/yr)
to sign with a Developer ID certificate and notarize.

## Files

- `Source/DSP.h` — all the audio processing
- `Source/PluginProcessor.*` — parameters and the chain order
- `Source/PluginEditor.*` — the plugin window
- `Resources/logo.png` — analyzer watermark
- `installer/windows`, `installer/mac` — install notes shipped in the zips
- `.github/workflows/build-mac.yml` — Mac build on GitHub
