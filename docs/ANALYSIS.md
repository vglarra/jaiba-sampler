# Jaiba Sampler — Codebase Analysis

> Static analysis of the repository as of commit `cf292d3` (2026-06-14, `main`, 115 commits).
> Prepared 2026 — no source files were modified as part of this review.
> Scope: `src/` (~18.6 kLOC), `CMakeLists.txt`, `CMakePresets.json`, `README.md`,
> `docs/screenshots`, and the root-level development artifacts
> (`ai_deepseek_log.txt`, `code_ base_*_2026_02_22.txt`, `notas-juce.txt`, `test_terminal.bat`).

---

## 1. Executive summary

Jaiba Sampler (CMake target: `MySampler`) is a **standalone, MIDI-triggered 16-pad
audio sampler built on JUCE 8.0.12** (C++17, Windows/VS2022). What started in
Feb 2026 as a single-sample desktop player has grown — across 115 commits from
Feb to Jun 2026 — into a sophisticated instrument:

- 16 pads, each with its **own** audio engine (voice/synthesiser, 3-band EQ,
  ADSR, normalize, gain, FFT spectrum analyzer),
- a waveform editor with zoom/scroll, markers, transient detection and overlays,
- **pattern recording** with a beat-locked clock, metronome, quantization and
  live WAV capture,
- a **16-bank "Global Jaiba Map" (GJM)** session system plus 5 persistent
  **global loop pads** that survive bank switches,
- kit (`.jai`) files, fullscreen FFT visualizer, dual-channel level meter,
  MIDI-learn, and per-pad MIDI routing/freeze.

**Verdict.** The audio core is genuinely well engineered: a real-time-safe
custom `SynthesiserVoice` with lock-free atomics, a lock-free EQ coefficient
double buffer, per-engine FFT isolation, and careful mute/swap sequencing.
The main problems are at the **integration and maintainability layer**: two
6,000+-line monoliths (`MainComponent.cpp`, `SampleCard.h`), almost all logic in
headers, heavy copy-paste duplication, ~50 `printf` debug remnants, stale
comments and dead scaffolding, a handful of genuine data races, and UI-thread
freezes on several file-loading paths.

| Field | Value |
|---|---|
| Language / framework | C++17, JUCE 8.0.12 (git submodule @ `501c076`) |
| Source size | ~18,600 lines in `src/`; most modules header-only |
| Biggest files | `SampleCard.h` (6,181), `MainComponent.cpp` (6,164), `MainComponent.h` (755), `PadAudioEngine.h` (788), `LoopingSampler.h` (742) |
| Git | 115 commits, `main`; clean tree at review time; activity Feb–Jun 2026 (peak: 43 commits in Apr) |
| Build | CMake + VS2022 presets (Windows-only); app links `vfw32.lib` |
| Binaries/docs | App/binary named "My Sampler"/"MySampler"; UI name "Jaiba Sampler V001" v1.0.0 |

---

## 2. Repository & build hygiene

- **Naming drift.** `CMakeLists.txt:3-16` declares project `MySampler`, product
  "My Sampler", company "YourCompany", version **0.1.0**; `Main.cpp:14-15` names
  the application **"Jaiba Sampler V001"**, version **1.0.0**; README says
  "Jaiba Sampler". Pick one identity and align all four locations.
- **README is outdated.** It documents the *pre-16-pad* app (single sample
  card, "+" button) and references files that do not exist (`SampleCard.cpp`,
  `PadManager.cpp`, `assets/`). The Jun-2026 screenshots in `docs/screenshots`
  show the real UI. The "Project Structure" and "Usage" sections need a rewrite.
- **Windows-only build surface.** `CMakePresets.json` has only a VS2022 preset;
  there is no Linux/macOS preset despite JUCE being cross-platform.
- **Stale include.** `MainComponent.cpp:14` includes `<vfw.h>` for a
  `GetOpenFileNamePreviewA` note, but no such symbol is used; `vfw32.lib` is
  still linked (`CMakeLists.txt:52-54`).
- **Submodule hygiene.** `lib/JUCE` is a proper git submodule (pointer
  `501c076`), but it is **not initialized in a fresh clone without
  `--recursive`/`git submodule update --init`**; the README does say this.
- **Root clutter (tracked in git):** `notas-juce.txt`, `test_terminal.bat`,
  `ai_deepseek_log.txt`, `code_ base_summary_2026_02_22.txt`,
  `code_ base_ai_assesment_2026_02_22.txt` — see §9 for what they actually are.

---

## 3. Feature map (what the app does today)

### Pad grid — `TrianglePadGrid.h`
2×8 trapezoid pads (row geometry fixed-ratio, edge pads half-trapezoids).
Click = select (purple), click-hold on the selected pad = mouse trigger;
MIDI hits flash fuchsia; ice-blue dot = active MNFreeze loop.

### Global loop pads — `GlobalLoopColumn.h`
5 persistent loop slots (G1–G5) with **separate engines that survive bank
switches**. Single click edits the slot in the SampleCard; double click
toggles loop playback. Slots have their own MIDI note/channel.

### Sample editor — `SampleCard.h` (6,181 lines)
Four tabs:
- **Controls** — pitch −/display/+ with step sizes (100/50/25/33 ¢),
  auto-Tune (YIN pitch detection on a background thread), Freeze, Loop,
  OneShot, Reverse, Bounce, MNFreeze, CRA transient detection with Sens knob,
  Start/End markers with transient snapping, grid snap, Trim.
- **ADSR** — custom 4-stage envelope (used instead of the click-elimination
  default when enabled), with per-loop-cycle retrigger handling.
- **EQ** — interactive magnitude-response display (biquad curves for 3 bands ×
  6 filter modes: low-cut/low-shelf/bell/notch/high-shelf/high-cut) with a
  live spectrum overlay of the *current pad's* audio, plus a pad-gain knob.
- **Rec** — tap tempo (BPM 60–360), metronome + volume, target pad 0–15,
  Record/Stop; in G-pad mode it becomes the transfer UI ("Load Kit Pad" /
  Drop Pad).

### Waveform editor — `WaveformComponent` (in SampleCard)
Zoomable/scrollable display painted purely from **8,192 pre-computed peak bins**
(`WaveformPeakBin`, `kWaveformPeakBins=8192`) — zero disk I/O in `paint()`.
Stereo L/R split paths, transient ticks, loop/ADSR overlays, 60 Hz playhead
with direction arrow (bounce mode), marker dragging with grid/transient
snapping, ruler with nice ticks or user grid.

### Per-pad audio engine — `PadAudioEngine.h` + `LoopingSampler.h`
- Custom `LoopingSamplerSound`/`LoopingSamplerVoice` on top of
  `juce::Synthesiser` (16 voices, note stealing).
- Sample-accurate start/end markers updated **live** through atomics.
- Playback modes: forward, **reverse**, **bounce (ping-pong)**, one-shot,
  freeze, and a custom ADSR — all toggleable at runtime.
- Live pitch offset (±48 st in cents) and A4 base tuning (400–480 Hz).
- 3-band parametric EQ — UI thread writes coefficients into a double buffer,
  audio thread swaps once per block.
- Normalization (scans the region on a background thread for big files),
  per-pad volume and pad-gain.
- Per-engine FFT spectrum analyzer on its own worker thread fed **only** that
  pad's post-EQ signal (isolation by private scratch buffer).
- Peak cache per pad for instant waveform display on pad switch.

### MIDI
Device selection, channel filter (0 = any), **MIDI Learn**, per-pad
note/channel routing, **MNFreeze** (incoming note toggles an independent
freeze loop per pad, tracked in `PadMnFreezeState[16]`), MIDI activity LED,
measured MIDI→audio latency (displayed as console diagnostics).

### Recording / pattern engine — `MainComponent.cpp:4378-4850`
- Beat clock advanced in the audio callback; metronome beep (1 kHz, 20 ms
  sin(πt) envelope) at beat edges.
- Quantized capture of pad triggers into a target pad; overdub mode seeds from
  the existing quantized pattern; live **WAV capture** of whatever is played
  (24-bit, `AudioFormatWriter::ThreadedWriter`); optional **audio
  quantization** (cut-and-splice of raw slices onto the beat grid → `-q.wav`).
- Per-pad saved patterns (name → beat-relative events) stored in `PadSettings`.

> ⚠️ See §5.15 — parts of this engine (quantization selector, Overdub, pattern
> save/load/playback UI) are currently **unwired shells** despite the plumbing
> existing.

### Sessions / persistence
- `PadSettings` per pad (pitch, modes, vol/norm, ADSR, EQ, markers, transients,
  grid, MIDI, zoom, tab) with pad-indexed property keys and XML.
- Single-pad kit files `.jai` (16 `<Pad index=i>` elements), with folder
  prev/next navigation over `.jai` files.
- **GJM `.gjm`** = manifest of 16 banks × 16 pads + 5 global pads; parsed in a
  background job; bank switching pre-warms engines into RAM; kit files
  auto-generated for banks with samples.

### Visuals & utility
Fullscreen FFT visualizer (`FftVisualizerView`, **on by default at boot** —
`MainComponent.cpp:376-379` hides the SampleCard), dual-channel 20-segment
level meter, sine test tone, Escape = panic reset, dialogs for audio/MIDI
devices and the legacy sample-mapping interface.

---

## 4. Architecture

```
Main.cpp → SamplerApplication → MainWindow → MainComponent
MainComponent (juce::AudioAppComponent + 11 listener interfaces, ~90 methods)
 ├─ PadManager ── 16× PadAudioEngine (+5 global)    per pad:
 │        └─ mix → master volume                     • Synthesiser + LoopingSamplerVoice[]
 │                                                   • 3-band biquad EQ (double buffer)
 ├─ PadSettings[16] + globalSettings[5]              • FFT worker thread + spectrum double buffer
 ├─ GjmManager (16 banks × 16 PadSettings)           • MappedSample[] (shared_ptr full buffer)
 ├─ SampleCard — 4 tabs; Listener → MainComponent    • peak cache, atomic param stores
 ├─ TrianglePadGrid / GlobalLoopColumn / LevelMeter / FftVisualizerView
 └─ ConfigurationManager (JUCE PropertiesFile)
```

**Audio render chain** (`MainComponent::getNextAudioBlock`):
`midiCollector` (lock-free FIFO) → `PadManager::renderNextBlock`, where each
engine renders into a **private scratch** buffer, applies norm→volume→pad-gain
→EQ, pushes only its own signal into its FFT FIFO, then accumulates into the
shared output → master volume → optional live WAV tap → metronome beep → sine
test → output-peak scan → visualizer feed.

**The "big red switch" pattern.** A `muteOutput` atomic on each engine: the
message thread sets it, the audio callback zeroes the next block and returns;
the engine is rebuilt; the flag is cleared. Used for sample loads, bank
switches, drops, panic and shutdown.

**Real-time strengths (the best code in the project):**
- `LoopingSamplerVoice::renderNextBlock` re-reads all mutable parameters once
  per block from atomics, allocates nothing, and implements careful click
  elimination: a 128-sample crossfade state machine at loop wraps / bounce
  flips / reverse start/stop, zero-crossing search at wrap points, exponential
  pitch smoothing, and a manual per-cycle ADSR that correctly retriggers on
  each loop pass (JUCE's ADSR is not restarted from sustain).
- EQ coefficients are computed on the UI thread and swapped in nanoseconds;
  filter state is drained implicitly by the pre-fade-to-zero at transitions.
- FFT is isolated per engine (private scratch before mixing) and runs on a
  worker thread, so the EQ display never shows the summed mix.

---

## 5. Findings & risks

Severity: 🔴 high · 🟠 medium · 🟡 low.

### 5.1 Real-time / concurrency

1. 🔴 **Data race on recorded pattern events.** `recEvents`/`recQuantised` are
   documented as *"Message-thread-only event storage"*
   (`MainComponent.h:719-721`) yet `push_back`'d on the **MIDI callback
   thread** in `handleIncomingMidiMessage` (`MainComponent.cpp:1529-1536`) —
   the comment there even admits the push "must not run on the MIDI callback
   thread" while doing exactly that. A `std::vector` reallocation on the MIDI
   thread risks priority inversion, and the same vector is read/sorted/cleared
   by `endRecording`/`quantiseRecordedEvents` on the message thread with no
   lock. The same vector is also written from the pad-grid lambda
   (`MainComponent.cpp:69-73`, message thread).
   *Fix:* a small lock-free SPSC queue drained on the message thread, or
   `callAsync` batching.

2. 🔴 **Global-pad MIDI injection bypasses the FIFO.** Kit pads are queued via
   the lock-free `midiCollector` and drained on the audio thread; global pads
   get `noteOn`/`noteOff` called **directly into the Synthesiser on the MIDI
   callback thread** (`MainComponent.cpp:1484-1493`), racing the audio
   thread's render. Two different real-time models for the "same" operation.

3. 🟠 **Message-thread decode freezes.** `loadSampleFileAsync`'s full-file
   decode + peak scan (`MainComponent.cpp:3069-3133`) runs on the **message
   thread** when called from the "+" button (`:1100`), `loadKitFromFile`
   (`:2217`), `switchGjmBank` (`:2663`), trim result (`:4360`), record
   finalize (`:4790`) and the rename-reload dialog. Only Prev/Next navigation
   is backgrounded (via a 1-thread pool + generation counter + 50 ms debounce,
   `:3026-3064`). Comments elsewhere admit MP3 loads used to block ~7 s.
   *Fix:* route every entry point through the same generation-guarded pool.

4. 🟠 **Background pre-warm can resurrect stale audio.** The `callAsync` tails
   of `preloadPadEngineAsync`/`preloadGlobalPadEngineAsync`
   (`MainComponent.cpp:1754`, `:1891`) have **no generation/cancellation
   check**; a slow preload from a previous bank can finish *after* a later
   bank switch and install old audio into an engine whose `padSettings`
   already belong to the new bank (the empty-pad cleanup at `:2590-2608` runs
   only at switch time).

5. 🟡 **ThreadedWriter lifetime race.** `proceedWithRecording`
   (`MainComponent.cpp:4414-4419`) deletes a replaced live-render writer on the
   pool thread while the audio thread may be inside an in-flight `write()`;
   `finalizeLiveRender`'s 80 ms settle delay covers only the normal stop path.

6. 🟡 **Allocation on the audio thread.** `PadAudioEngine::renderNextBlock`
   grows its private scratch buffer with `setSize` inside the callback
   (`PadAudioEngine.h:169-170`) — the first block after start (and any block
   size growth without `prepareToPlay`) allocates on the RT thread;
   `prepareToPlay` never pre-sizes it. A fresh `juce::MidiBuffer shiftedMidi`
   is also assembled per engine per block whenever MIDI events exist
   (`PadAudioEngine.h:181-188`).

7. 🟡 **EQ double-buffer front-index race.** `EqCoeffDoubleBuffer::front` is a
   plain `int` read by the UI thread (`writeFromUI`) while the audio thread
   flips it (`PadAudioEngine.h:54-66`); only `updated` is atomic. Benign in
   practice (0/1 toggles), formally a data race.

8. 🟡 **EQDisplay spectrum threading relies on outside wiring.** The class
   comment claims the spectrum is "pushed from the audio thread"; in fact the
   UI timer pulls a snapshot from the engine's worker-thread double buffer.
   Correct today, but the header overstates its own safety; if a producer ever
   writes `spectrumData[1024]` directly, that is a race (`SampleCard.h:36-587`).

### 5.2 State fidelity

9. 🟠 **`pitchStepCents` is destroyed on every capture.**
   `captureSampleCardToPadSettings` hard-codes `ps.pitchStepCents = 100;`
   (`MainComponent.cpp:6021`) even though `sampleCard.getPitchStepCents()`
   exists. The user's per-pad step choice (100/50/25/33) is only stored in the
   *global* config key by `pitchStepCentsChanged` (`:3790`), so it never
   round-trips through pad settings, kits, or GJM banks.

10. 🟠 **Pre-warmed pads diverge from UI-selected pads.**
    `preloadPadEngineAsync` forces `sound->baseTuningRatioAtomic.store(1.0f)`
    ("440 Hz default at startup", `MainComponent.cpp:1815`) and never restores
    **normalize gain, EQ enable/coefficients, or MNFreeze**. A pad triggered by
    MIDI *before* the user clicks it can therefore play with wrong tuning/EQ.
    (The pad-selected handler `:135-152` and `selectGlobalPad` `:5518-5529`
    push EQ only when the pad is displayed.)

11. 🟡 **Legacy multi-sample mapping is vestigial.** The Mapping dialog still
    edits `lowNote/highNote/rootNote`, but `updateSamplerSounds` only ever sets
    a **single note bit** (`noteRange.setBit(sample->rootNote)`,
    `MainComponent.cpp:2031-2033`), so key-range mapping has no effect: each
    pad plays one sound on one note.

12. 🟡 **Stale startup claim.** `MainComponent.h:162` says
    `preloadPadEngineAsync` is "used at startup to restore all saved pads";
    `loadLastSession` (`MainComponent.cpp:5426`) actually starts fresh with an
    empty untitled session and preloads run only for kit/GJM loads and bank
    switches.

### 5.3 Dead code, scaffolding, hygiene

13. 🟡 **`HeartbeatThread` does nothing.** Its `run()` body is empty
    (`MainComponent.h:589-603`) while the constructor comment claims it prints
    every 100 ms to pinpoint message-thread hangs. It is a permanently
    sleeping OS thread.

14. 🟡 **Instrumentation stubs.** The block-budget monitor in
    `getNextAudioBlock` computes `elapsedUs`/`budgetUs` then
    `juce::ignoreUnused`s them (`MainComponent.cpp:710-724`) despite the
    "Print a warning" comment. Deferred diagnostic shells in `updateDeviceInfo`
    (`:1176-1192`) and timing captures (`:3454-3458`, `:3666`, `:3706-3707`)
    are empty. `paintOverChildren`/layout diagnostics compute unused totals
    (`:1010-1018`).

15. 🟠 **Recording features partially unwired.** The Rec panel hard-codes
    `quantBeats = 0.0` — "no quantization" (`SampleCard.h:4638`) — and the
    Overdub toggle is disabled for "latency focus" (`:4625-4630`). Listener
    methods `playbackQuantisedEvents`/`savePattern`/`loadPattern`/`clearPattern`
    (items 27/30/31/32 of the 33-virtual `Listener`) are **never fired by the
    UI**, so the entire quantized-pattern save/load/playback flow — the subject
    of several commits — is currently unreachable. Record-Stop also turns off
    the metronome with a **hard-coded 120.0 BPM** (`SampleCard.h:4666`) rather
    than the panel's current BPM.

16. 🟡 **Debug output everywhere.** ~50 `printf`(+`fflush`) sites:
    `MainComponent.cpp` (29), `Main.cpp` (13, including one per construction
    step), plus per-event and per-pattern-hit `[REC]` prints
    (`MainComponent.cpp:4605`, `:4827`), `DBG` calls (`:1967`, `:2647`,
    `:2689`), and a `printf` inside `TrianglePadGrid::selectPad` on **every**
    click. `prepareToPlay` prints on every audio restart (`:527`).
    (Much of it is intentional Windows console diagnostics for a dev tool, but
    it should be gated behind a macro.)

17. 🟡 **Massive duplication.** `DummyAudioReader` (a stub to satisfy
    `SamplerSound`'s constructor) is defined identically **4 times**
    (`MainComponent.cpp:1779`, `:1912`, `:1991`, `:5838`); the
    mute→clear-flags→forceStop→clearSounds→allNotesOff→unmute teardown
    sequence is copy-pasted **8+ times** (`:1761`, `:1897`, `:2299`, `:2590`,
    `:4869`, `:5787`, `:5919`, `:5951`); the peak-bin min/max scan is written
    3× (`:1735`, `:1873`, `:3111`); sample installation logic exists in 4
    functions. `PadSettings` serialization (XML + properties + reset) repeats
    every field 3–4× by hand.

18. 🟡 **Monolithic methods.** `SampleCard`'s constructor (~960 lines), its
    `resized()` (~320 lines of pixel math), and `WaveformComponent::paint()`
    (~470 lines) are single methods full of magic numbers (fonts, hit radii,
    freq clamps, colors). Successive feature layers ("FIX 1..7",
    "CRITICAL FIX #1-4", "OPT 4", "// REMOVE THIS") read as a changelog baked
    into the code.

19. 🟡 **Doc-vs-code drift on zero-crossing snapping.** `LoopingSampler.h`
    documents zero-crossing search at loop/bounce boundaries (that part is
    real, in the voice), but the *UI* does not implement zero-crossing
    **marker** snapping — only grid and transient snapping exist
    (`SampleCard.h:5782-5811`, `:4115-4247`).

20. 🟠 **Potential use-after-free in the Tune thread path.**
    `TuneAnalyzerThread` posts results with `callAsync([this]{ … })`
    (`SampleCard.h:5948-6008`), and `~SampleCard` only joins the thread
    (`stopThread(2000)`, `:1564-1581`). A lambda already queued when the object
    is destroyed can run afterwards against a freed `this`; capturing a
    `juce::Component::SafePointer` would make it safe. (Today `sampleCard` is a
    member value destroyed with `MainComponent`, so the practical blast radius
    is small.)

### 5.4 Auxiliary UI (from header review)

21. 🟡 **`FftVisualizerView` paint is the new hot spot.** Threading is the
    cleanest in the codebase (audio `AbstractFifo` → low-priority worker FFT →
    `SpinLock` snapshot → 60 Hz timer paint), but `paint()` rebuilds full
    multi-arm paths + up to 600 particles per frame at 60 fps on the message
    thread. Also `getSensitivity()`'s doc comment says "0.1–5.0" while the
    range is 0.1–15.0.

22. 🟡 **`MappingComponent::resized()` overlap bug.** The `ListBox` is given
    the full area *before* `removeFromBottom(80)` reserves the button row, so
    Add/Remove/Clear overlap the list's bottom rows (`UIComponents.cpp`) — a
    bug inherited unchanged from the Feb 2026 snapshot.

23. 🟡 **`AudioPreviewComponent` owns a second audio device manager.** The file
    chooser's preview boots its own `AudioDeviceManager`
    (`initialiseWithDefaultDevices`), which can contend with the main one on
    exclusive-mode drivers.

24. 🟡 **Minor state machine issues.** `LevelMeter::setSources()` swaps raw
    pointers (formally racy with the timer read); channel width can go
    negative below ~40 px. `GlobalLoopColumn::selectSlot` only downgrades
    states it set itself, so an out-of-band `setSlotState(Selected…)` can wedge
    selection; and a single click fires before JUCE recognizes a double click
    (first click selects, second toggles playback — verify intended UX).

### 5.5 Persistence

25. 🟠 **Three overlapping sources of truth.** Settings live in (a)
    per-*file* `ConfigurationManager::SampleState` keyed by path hash, (b)
    per-*pad* `PadSettings` (property keys, `.jai` XML, `.gjm` XML), and (c)
    in-memory `GjmManager` bank caches. `loadSampleFileAsync` implements an
    intricate 3-way restore (`:3277-3363`), and `saveGjmToFile` needs a
    "hydration" re-load of the existing manifest to avoid erasing untouched
    banks (`:2469-2470`). It works today but is fragile: adding one field means
    editing 4+ serializer sites.

---

## 6. What's good (worth preserving)

- Genuinely real-time-safe `LoopingSamplerVoice` with meticulous click
  elimination — not the usual "atomic flag" veneer.
- Clean RT separation in the DSP core: atomics + double buffers + dedicated FFT
  workers; `PadAudioEngine` isolates each pad's signal before mixing.
- Thoughtful load UX: pre-computed peak bins (zero disk I/O in paint),
  engine pre-warm on bank switch, generation-counter navigation debounce,
  deferred transient detection, cache pre-heat of buffer heads/tails.
- The engine layer boundaries (`PadManager`/`PadAudioEngine`/`PadSettings`) are
  clean, self-contained classes — the mess is *above* them in `MainComponent`.
- `WaveformPeakBin.h` and `KnobLookAndFeel.h` are tidy, purpose-built files.

---

## 7. Recommended next steps (priority order)

1. **Fix the real race (§5.1-1):** move pattern-event capture off the MIDI
   thread (lock-free SPSC queue or `callAsync` batch) so `recEvents` is truly
   message-thread-only; reconcile the conflicting header/comment claims.
2. **Background-load uniformly (§5.1-3/4):** route kit-load, bank-switch,
   trim and record-finalize through the same generation-guarded pool path as
   navigation, and add a generation check to both preload tails.
3. **Restore full state on pre-warm (§5.2-10, 9):** apply tuning/norm/EQ/
   MNFreeze atomics in the preload functions so MIDI-first triggering matches
   the UI path; capture the real `pitchStepCents` instead of hard-coding 100.
4. **Cut the monoliths:** move `SampleCard` internals to `.cpp` files and
   split `MainComponent` orchestration into cohesive controllers (GJM session,
   recording, pad switching) following the engine layer's existing style.
5. **Reconcile features with UI (§5.3-15):** decide whether quantization,
   Overdub and pattern save/load are in scope; either wire them (re-enable the
   quantizer, fire the four dead Listener calls) or delete the shells.
6. **Hygiene sweep (§5.3):** remove/gate `printf`/DBG spam, the no-op
   HeartbeatThread and budget monitor, the duplicated `DummyAudioReader` and
   teardown sequences; update README, naming, and stale comments.

---

## 8. Persistence model detail

- **Per-file settings** (`ConfigurationManager::SampleState`): keyed by
  `"p" + hex(hashCode64(filepath))`; restored on normal file loads, *not* on
  trim/kit loads (which use snapshots).
- **Per-pad settings** (`PadSettings`): property keys `pad0_…pad15_` for the
  flat PropertiesFile, plus full XML in `.jai`/`.gjm` files. `resetToDefaults`
  intentionally preserves MIDI note/channel (used by Drop Pad).
- **Patterns** live only inside `PadSettings::savedPatterns` (XML) — a flat
  PropertiesFile cannot represent them (documented in the header).
- **Master/audio/MIDI/zoom/viz** settings are global flat keys; audio device
  state is stored as an XML blob and flushed immediately on change.

---

## 9. The root-level `.txt` files (archaeology, not current docs)

| File | Size | What it is |
|---|---|---|
| `ai_deepseek_log.txt` | 25 KB / 638 lines | Chronological developer/AI project log, 2026-02-15 → 2026-02-27: environment metadata (Windows, MSVC 2022, JUCE 8.0.12 @ `501c076`, original path `C:\Users\vlarr\...\mysampler`), feature checklists, lessons learned (JUCE 8 `String::operator bool`, NOMINMAX, message-thread discipline), known limitations. Its "git history" hashes do not match this repo. |
| `code_ base_summary_2026_02_22.txt` | 141 KB / 3,827 lines | **Raw concatenated snapshot of 8 pre-refactor source files** (2026-02-22) — ConfigurationManager.h, LookAndFeel+Main.cpp, MainComponent.cpp/.h, MidiActivityLight.h, SampleCard.h, UIComponents.cpp/.h. No prose. |
| `code_ base_ai_assesment_2026_02_22.txt` | 60 KB / 1,769 lines | Same-date **partial** snapshot of the same old architecture (Main.cpp, truncated MainComponent.cpp/.h, UIComponents) with AI-authoring placeholders. Reads as an AI-assisted reconstruction artifact. |
| `notas-juce.txt` | 33 lines | Spanish PowerShell cheat sheet (rebuild commands, log-appending recipe). |
| `test_terminal.bat` | 9 lines | Trivial `echo` test for VS Code terminal access. |

All documents describe the **pre-PadManager era**: one `MainComponent` with a
single `Synthesiser`, a `MappedSample` array, per-pixel waveform painting (the
freeze source that `WaveformPeakBin.h` later fixed), folder navigation, and no
pad/bank/GJM/recording machinery. Zero occurrences of `PadManager`,
`PadAudioEngine`, `PadSettings`, `TrianglePadGrid`, `GlobalLoopColumn`,
`LoopingSampler`, `FftVisualizerView`, `LevelMeter`, `WaveformPeakBin`,
`GjmManager`, `.gjm`, or bank switching. The 16-pad architecture arrived with
commit `feb8cb1` (2026-04-16, "refactor: extract audio engine into
PadAudioEngine/PadManager architecture").

---

## Appendix A — Key file map

| File | Lines | Role |
|---|---|---|
| `src/Main.cpp` | 94 | JUCE app bootstrap (heavy debug prints) |
| `src/MainComponent.cpp/.h` | 6,164 / 755 | God component: orchestration, MIDI, GJM, kits, recording, global pads |
| `src/SampleCard.h` | 6,181 | Sample editor: waveform, tabs (Controls/ADSR/EQ/Rec), EQ display |
| `src/PadAudioEngine.h` | 788 | Per-pad engine: synth, EQ, FFT worker, gains, peak cache |
| `src/LoopingSampler.h` | 742 | RT voice/sound: markers, loop/reverse/bounce, xfade, ADSR |
| `src/PadManager.h` | 291 | 16+5 engines, mixing, master volume, MIDI pad lookup |
| `src/PadSettings.h` | 455 | Serializable per-pad state (XML/properties/patterns) |
| `src/GjmManager.h` | 286 | 16-bank × 16-pad manifest, global pads, `.gjm` I/O |
| `src/ConfigurationManager.h` | 462 | PropertiesFile persistence (global + per-sample) |
| `src/TrianglePadGrid.h` | 767 | Trapezoid pad grid + GlobalControlsBar |
| `src/GlobalLoopColumn.h` | 245 | 5 persistent loop slots |
| `src/FftVisualizerView.h` | 391 | Fullscreen FFT visualizer (kaleidoscope/rings) |
| `src/LevelMeter.h` | 230 | Dual-channel segmented meter |
| `src/UIComponents.cpp/.h` | 347 / 137 | Legacy: AudioPreview, MidiSelector, SampleListModel, MappingComponent |
| `src/MidiActivityLight.h` | 85 | MIDI activity LED |
| `src/WaveformPeakBin.h` | 19 | Peak-bin struct + `kWaveformPeakBins = 8192` |
| `src/KnobLookAndFeel.h` | 38 | Compact rotary-slider L&F |

---

*End of analysis.*
