# Seq tab — step sequencer plan

Status: **proposal only — nothing implemented.**
Revision 4 — self-contained (detached) tracks + per-track mixer volume.
Related: `docs/Click behaviour idea.md` (metronome click — parked)

---

## 1. Request and locked decisions

A new **Seq** tab on the SampleCard that hosts a per-kit step sequencer.

| # | Decision | Agreed |
|---|---|---|
| **D1** | Track model | **16 tracks, nominally one per pad** (track *i* starts out as pad *i*). |
| **D2** | Pattern ownership | **Per bank/kit.** The Seq view is shared by all 16 pads. One sequence plays at a time, owned by the bank that started it. |
| **D3** | MIDI rows | **Removed.** No MIDI tracks, no MIDI output subsystem. |
| **D4** | Transport | **Shared with the metronome** → tempo **and click volume** are bank-level (§8). |
| **D5** | Free / no-snap | Live-record unquantised + shift-drag arbitrary placement. |
| **D6** | Legacy pattern recorder | Keep the data code, **disable the UI**, repurpose the Rec tab later for sound-card input recording. |
| **D7** | Sound ownership | **Self-contained tracks.** Each track stores its own sample path + settings and plays through its own resident engine — independent of the live pads and of the active bank. |
| **D8** | Track mixing | **Per-track volume fader**, always live and adjustable after recording. |

Snap choices: **1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, 1/64, Free**.

### Why D7 (this is the big one)

Revision 3 had `SeqTrack.padIndex` pointing at `padManager.getEngine(padIndex)`, but those 16 engines are **bank-swapped** by `switchGjmBank`. So a Bank 1 pattern could never sound while Bank 2 was active, and editing a pad changed the pattern. D7 fixes both: the pattern **owns its sounds** and plays through **resident engines** that live outside the bank swap. Record in Bank 1, switch to Bank 2, keep playing and keep the recorded sounds.

The app already has this pattern: the **global loop pads** are engines that deliberately survive bank switching.

### Why D8

With self-contained tracks, the captured `PadSettings` fixes the sound — but a pattern still needs a mix. The per-track fader is a **post-capture mixer level**, always visible and always live, so levels can be balanced after recording without touching the captured sound.

---

## 2. What already exists (and why it shapes the plan)

| Thing | Where | Relevance |
|---|---|---|
| Pattern recorder | `recEvents` / `recQuantised`, `PatternEvent{padIndex, beatTime}` | Closest existing feature, wrong foundation (no velocity, 10 ms timer) — §9. |
| Velocity | `LoopingSamplerVoice::startNote` → `lgain = rgain = velocity` | Already works in the audio path. |
| **Resident engines precedent** | `PadManager::globalEngines` (5 "global loop pads" that survive bank switches) | Exactly the mechanism D7 needs, already proven. |
| **Shared audio buffers** | `MappedSample::audioData` is `std::shared_ptr<juce::AudioBuffer<float>>` | A track can **share the pad's decoded buffer** → detaching costs no duplicate sample RAM. |
| **FFT thread warning** | `PadAudioEngine::prepareToPlay` unconditionally creates + starts an `FftWorkerThread` | A naive 16-engine sequencer pool would add 16 threads for nothing. Must be gated. |
| Master volume convention | `PadManager::renderNextBlock` applies master at the end; the metronome beep multiplies by `padManager.getMasterVolume()` itself | The sequencer must apply master the same way, or be mixed before that step. |
| Audio capture | `liveRenderWriter` taps the master output into WAV | Reusable for D6's input recording; source must change (§9). |
| UI space | SampleCard ≈ 720×375; tab content ≈ 80 px today | 16 rows fit once the waveform is reclaimed (§5). |

---

## 3. Recommended architecture

### 3.1 Timing — PPQ ticks

**960 PPQ** so every requested snap divides evenly (1/64 = 60 ticks, 1/32T = 80, 1/16T = 160, 1/8T = 320).

| Snap | Steps/beat | Ticks/step |
|---|---|---|
| 1/4 | 1 | 960 |
| 1/8 | 2 | 480 |
| 1/8T | 3 | 320 |
| 1/16 | 4 | 240 |
| 1/16T | 6 | 160 |
| 1/32 | 8 | 120 |
| 1/32T | 12 | 80 |
| 1/64 | 16 | 60 |
| Free | — | 0 (no quantise) |

Pattern length = `bars * 4 * PPQ`. Snap: `round(tick / ticksPerStep) * ticksPerStep`.

### 3.2 Data model

```cpp
enum class SeqSnap { Quarter, Eighth, EighthT, Sixteenth, SixteenthT,
                     ThirtySecond, ThirtySecondT, SixtyFourth, Free };

struct SeqHit { int tick = 0; float velocity = 0.8f; };

// Sound-relevant subset of PadSettings, captured with the track.
struct SeqSound {
    bool         hasSound = false;
    int          sourcePad = 0;        // pad it was captured from (label/default only)
    juce::String sampleFilePath;
    PadSettings  settings;             // pitch/loop/one-shot/reverse/bounce/vol/norm/ADSR/EQ/padGain
    std::shared_ptr<juce::AudioBuffer<float>> audio;   // shared with the source pad engine
};

struct SeqTrack {
    float volume = 1.0f;               // D8: mixer fader, always live
    bool  mute   = false;
    SeqSound    sound;                 // D7: self-contained
    std::vector<SeqHit> hits;          // absolute ticks
};

struct SeqPattern {
    static constexpr int kTracks = 16;
    SeqSnap snap = SeqSnap::Sixteenth;
    int     bars = 1;
    int     ppq  = 960;
    double  bpm  = 120.0;              // bank tempo (D4)
    float   clickVolume = 0.5f;        // bank click level (D4)
    std::array<SeqTrack, kTracks> tracks;
};
```

Absolute ticks per hit (not a fixed step array) is what makes triplets **and** Free mode work with one model. The grid is a view: column = `tick / ticksPerStep`.

Note `SeqSound::audio` is **runtime only** — the XML stores the path and settings, and the buffer is decoded on load.

### 3.3 Resident playback pool (D7)

One dedicated engine per track, created **lazily** for tracks that actually have a sound, living outside `switchGjmBank`.

```cpp
class SequencerEngine {
public:
    void prepareToPlay (double sampleRate, int blockSize);
    void releaseResources();

    void setPattern (std::shared_ptr<const SeqPattern>);   // lock-free publish
    void start(); void stop();

    // Message thread. Installs/refreshes a track's sound, sharing the pad's buffer.
    void setTrackSound (int trackIdx, const SeqSound& sound);
    void setTrackVolume (int trackIdx, float volume);      // D8, live

    // Audio thread. Renders the whole pool into the master mix.
    void processBlock (int numSamples, juce::AudioBuffer<float>& mixInto);
private:
    std::array<std::unique_ptr<PadAudioEngine>, SeqPattern::kTracks> trackEngines;
    std::shared_ptr<const SeqPattern> pattern;
    double playheadTicks = 0.0;
    std::atomic<bool> playing { false };
};
```

Per block: advance the playhead; for each track find hits in `[startTick, endTick)`; add note-ons **at the exact sample offset** into that track engine's `MidiBuffer`; render each track engine into the mix; wrap at `bars * 4 * PPQ`.

**Two required engine tweaks:**

1. **Gate the FFT thread.** Add something like `PadAudioEngine(bool enableFft = true)` (or a `prepareToPlay` flag). Sequencer engines pass `false` so they skip the `FftWorkerThread` and the spectrum push. Without this, detaching would spawn up to 16 extra threads for a spectrum nobody looks at.
2. **Share the buffer.** Build the track's `LoopingSamplerSound` from the pad's existing `audioData` shared_ptr using the same dummy-reader trick `preloadPadEngineAsync` already uses — no re-decode, no duplicate RAM.

**Master volume:** the sequencer pool applies `padManager.getMasterVolume()` to its output the same way the metronome beep does, so it stays consistent with the existing convention.

**This adds no percussion latency.** The pool renders in the same `getNextAudioBlock`, and its events carry exact sample offsets. Sequenced hits are tighter than live MIDI, which has to come in through the OS and the `midiCollector`.

### 3.4 Capture and re-link (D7)

- **Capture (automatic):** the first time a track is used (first step placed, or first hit recorded), it snapshots the corresponding pad: `sampleFilePath`, a copy of the sound settings, and a **shared reference** to the decoded buffer.
- **"Update from pad"** (explicit action): re-captures a track from its pad, for when you *want* it to follow a changed pad sound.
- Because the snapshot holds a shared_ptr, a later pad change replaces *the pad's* reference — the track keeps the old buffer. Detaching is therefore free of duplication and immune to pad edits.

### 3.5 Modes

| Mode | Sequencer | Incoming MIDI | Grid |
|---|---|---|---|
| **Live** | plays | plays pads live, does **not** touch the pattern | read-only |
| **Record** | can play or stop | captured into the matching track (note → pad → track) | writes hits |
| **Arrange** | stopped (or plays, your call) | ignored for capture | full mouse editing |

Optional **Lock** ("pattern approved") keeps Live read-only so a performance can't disturb an approved take. Re-arranging is explicit: hit **Record**, play, done.

Recording maps notes to tracks via the pad's `midiNote` (the same `findPadForMidiNote` routing the app already uses), so pads sharing a note are ambiguous — a small "unique pad notes" warning is a nice-to-have.

### 3.6 Thread safety

UI edits build a **new immutable `SeqPattern`**, published with an atomic `shared_ptr` store; the audio thread loads it once per block. Retired snapshots are freed on the message thread. Track sounds (buffer + settings) are installed on the message thread behind the existing `sampleLock`/mute pattern used by `preloadPadEngineAsync`. Never allocate, lock or free inline in `processBlock`.

### 3.7 What this removes

The Revision 3 plan extended `PadManager::renderNextBlock` with per-pad event buffers for deterministic routing. **No longer needed** — the sequencer's own pool has one sound per track with its own note, so it is decoupled from the pad note map by construction. `PadManager` is untouched except for mixing the pool's output.

---

## 4. UI

```
┌ Seq ──────────────────────────────────────────────────────────────────────────┐
│ [▶][■][● Rec]  Snap:[1/16▾]  Bars:[1▾]  BPM:120  [Live/Arrange]  [Lock]       │
│ playing: Bank 1   ← indicator when another bank's sequence is running         │
├────────────────┬──────────────────────────────────────────────────────────────┤
│ 1  kick.wav ▬▬ │ ■  ·  ·  ■  ·  ·  ■  ·  ■  ·  ·  ■  ·  ·  ■  ·               │
│ 2  snare.wav ▬ │ ·  ·  ■  ·  ·  ■  ·  ·  ·  ·  ■  ·  ·  ■  ·  ·               │
│ 3  hat.wav  ▬▬ │ ■  ·  ■  ·  ■  ·  ■  ·  ■  ·  ■  ·  ■  ·  ■  ·               │
│ …              │                                    16 rows, one per pad     │
│ 16 …        ▬  │ …                                                            │
└────────────────┴──────────────────────────────────────────────────────────────┘
   fixed row header          horizontally scrollable grid · playhead column
```

- **Row header (fixed):** track no., the **captured** sample name, mute, and a **compact per-track volume fader** (D8) — always visible, always live, double-click to reset to unity. It shows the *captured* sample, not the live pad, so it stays truthful after the pad changes.
- **Grid:** inside a `juce::Viewport`; horizontal scroll for long patterns. 16 rows fit at the default size (§5).
- **Cell interaction:** left-click toggles a hit at the last-used velocity; **vertical drag sets velocity**; right-click clears.
- **Playhead:** one highlighted column, repainted from an `std::atomic<double>` published by the engine, ~30 fps, repainting only that column.
- **Audition:** clicking a cell fires the track's own engine once — you hear exactly the captured sound.
- **Snap combo:** all 9 options. Changing resolution never moves existing hits; add an explicit "Quantise now" later.
- **BPM / click volume:** the bank values (§8).
- **"playing: Bank N"** indicator, because the pattern playing may not be the one the tab is showing (D2 + D7).

---

## 5. Layout

Tab content is ≈80 px today because the waveform takes 175 px. Reclaiming the waveform gives ≈340 px: 16 rows × 18 px = 288 px, plus ~24 px toolbar and ~16 px ruler ≈ 328 px. **It fits**, with vertical scrolling as a safety net for smaller windows. Horizontal scrolling handles long patterns (one 4/4 bar at 16ths = 16 steps, which fits ~720 px).

---

## 6. Live recording into the grid

In **Record** mode, from the existing `handleIncomingMidiMessage` hook:

- Map note → track via `findPadForMidiNote`.
- Capture `message.getFloatVelocity()` and the tick from `recSongBeatPos * PPQ`.
- Snap per the current resolution; raw tick in Free mode.
- If the track has no sound yet, capture it from that pad at the same moment.
- Hand off to the message thread (lock-free FIFO / `callAsync`) — never write the live pattern from the MIDI callback thread.

---

## 7. Persistence

Per bank, inside the bank's `.jai`, so a kit carries its groove (D2) and its sounds (D7).

```xml
<JaibaKit version="1" ...>
  <Pad index="0"> ... </Pad>
  ...
  <Sequencer snap="3" bars="1" ppq="960" bpm="120" clickVol="0.5" mode="live">
    <Track volume="0.80" mute="0">
      <!-- sound-relevant fields only; no patterns / MIDI routing / zoom -->
      <Sound pad="0" samplePath="C:/samples/kick.wav" vol="1.0" eqEn="1" ... />
      <Hit tick="0"   vel="0.80"/>
      <Hit tick="240" vel="0.55"/>
    </Track>
    ...
  </Sequencer>
</JaibaKit>
```

- The `<Sound>` element reuses the same attribute names as `<Pad>` (so the writer/reader already understand them), but **only the sound-relevant subset** — deliberately excluding `savedPatterns`, MIDI routing, zoom and tab, so a track snapshot never duplicates a pad's patterns.
- Touch points: `GjmBank` gains `SeqPattern sequence;`; `GjmManager::parseKitFile` must return it too (new `KitData` struct or out-parameter) and `saveBankKit` writes it. The `.gjm` references and rewrites each bank's `.jai`, so "Save Session" covers it.

---

## 8. Metronome and tempo ownership (D4)

- **BPM → bank level** (in `SeqPattern`). The metronome reads it; this replaces the global `recBpmAtomic` as the source of truth for the bank's tempo.
- **Click volume → bank level** (confirmed). Replaces the per-pad `metVol` as the live value; the per-pad field stays in `PadSettings` for file compatibility and is superseded.
- **Metronome on/off → transport state.**
- Because the sequencer pool is resident, the click and the sequence keep running across bank switches.

---

## 9. Legacy pattern recorder (D6)

- The **pattern/MIDI-event** half is not reusable (different timing model, no velocity, per-pad storage) — but it **is serialised into existing `.jai` kits**, so deleting it would silently drop saved patterns. **Disable/hide the Rec-tab pattern UI, keep the parse/serialise code**, mark it legacy, remove later in a deliberate commit.
- The **audio-capture** half is reusable for future input recording (`ThreadedWriter`, writer thread, file naming, trim). Today it taps the **master output**; input recording means switching the source to `AudioDeviceManager` input channels.
- **Don't remove — disable now, repurpose later.**

---

## 10. Phased delivery

| Phase | Deliverable |
|---|---|
| **0** | `SeqPattern` model (self-contained tracks + per-track volume) + `.jai` round-trip. No UI. |
| **1** | Seq tab shell: 5th tab, panel, waveform reclaimed, 16-row grid with row header (sample name + volume fader), snap combo, click-to-toggle, ruler. **Silent.** |
| **2** | Resident sequencer pool: lazily-created FFT-less engines, shared buffers, capture-from-pad, play/stop, playhead, audition, mixing + master volume, bank tempo. |
| **3** | Velocity editing (drag) + **Record mode** + Free mode + mode switching. |
| **4** | Polish: bars/length, mute, copy/paste, swing, undo, quantise-now, "Update from pad", "playing: Bank N" indicator, unique-pad-note warning, optional global launch strip. |

Phases 0–2 give a working self-contained 16-track sequencer that survives bank switches; 3 completes the "hit + velocity + snap" requirement.

---

## 11. Risks

- **Thread budget.** `PadAudioEngine` spawns an FFT thread per instance. The sequencer pool **must** gate it (§3.3) and create engines **lazily** (only tracks that have a sound), or bank-switching plus detaching could balloon the thread count.
- **Buffer sharing.** The track must share the pad's `shared_ptr<AudioBuffer>` rather than re-decoding; verify the sound builds correctly from a shared buffer (the `preloadPadEngineAsync` dummy-reader path).
- **Master volume.** The pool must apply master the same way the metronome does, or be mixed before `PadManager`'s master step — easy to get wrong and it silently skews the mix.
- **RT safety.** Pre-reserve all `MidiBuffer`s in `prepareToPlay`; no allocation, locks or frees in `processBlock`.
- **Snapshot lifetime.** Freeing a retired pattern (or a track's shared buffer) while the audio thread still holds it is a use-after-free. Retire on the message thread only.
- **Note gating.** A step needs a note-off: fixed short gate (e.g. 30 ms), off-at-next-hit, or honour the pad's one-shot/loop setting. Decide in Phase 2.
- **Broken sample paths.** Self-contained tracks store a path; move the file and the track goes silent (same as pads today). Surface it in the row header (e.g. "missing").
- **Duplicated settings in the `.jai`.** Each track repeats a subset of pad settings. Small, but the reader/writer must stay in sync with `PadSettings`'s attribute names.
- **Two behaviours to document.** "Sound captured at record time" vs "live pad" is exactly the kind of thing that confuses later — the row header showing the *captured* name is the mitigation.
- **Tempo migration.** Moving BPM to per-bank touches the existing Rec/pattern paths — do it in Phase 2, keeping `recBpmAtomic` mirrored while both features coexist.

---

## 12. Files likely to change

| File | Change |
|---|---|
| `src/Sequencer.h` *(new)* | `SeqSnap`, `SeqHit`, `SeqSound`, `SeqTrack`, `SeqPattern`, `SequencerEngine` |
| `src/SequencerGrid.h` *(new)* | 16-row grid, row header (name + volume), playhead, mode/snap toolbar |
| `src/PadAudioEngine.h` | Optional FFT disable (constructor/prepare flag); expose the decoded buffer for sharing |
| `src/PadManager.h` | Mix the sequencer pool's output (or MainComponent does) |
| `src/SampleCard.h` | 5th tab button, content routing, waveform reclaim, `setSeqTabVisible` |
| `src/MainComponent.h/.cpp` | Own the engine + transport + modes; call `processBlock`; capture; bank tempo + click volume |
| `src/GjmManager.h` | `GjmBank::sequence`, parse + save `<Sequencer>` |
| `CMakeLists.txt` | List the new headers |

`ConfigurationManager` is unchanged — no MIDI output device to persist.

---

## 13. Suggested first move

**Phase 0 + Phase 1, then stop and look.** The Seq tab that reclaims the waveform, draws the 16-row grid with its row header (captured sample name + volume fader), the 9 snap choices and the mode toggle, lets you click steps in and out, and saves/reloads with the kit — **silent**. That validates the layout fit, the row-header density and the `.jai` shape before any audio-thread work, and it is cheap to discard if the feel is wrong.

All decisions are closed (D1–D8). Ready to start on your word.
