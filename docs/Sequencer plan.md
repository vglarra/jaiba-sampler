# Seq tab — step sequencer plan

Status: **proposal only — nothing implemented.**
Revision 3 — final track model (16 pad tracks), all decisions locked.
Related: `docs/Click behaviour idea.md` (metronome click — parked)

---

## 1. Request and locked decisions

A new **Seq** tab on the SampleCard that hosts a per-kit step sequencer.

| # | Decision | Agreed |
|---|---|---|
| **D1** | Track model | **16 tracks, one per pad.** Track *i* IS pad *i*. No MIDI tracks. |
| **D2** | Pattern ownership | **Per bank/kit.** The Seq view is shared by all 16 pads — opening Seq on pad 1 shows the same thing as on pad 2. |
| **D3** | MIDI rows | **Removed.** The 4-MIDI-track idea is dropped; the app needs no MIDI output. |
| **D4** | Transport | **Shared with the metronome** → tempo **and click volume** are bank-level (§8). |
| **D5** | Free / no-snap | Live-record unquantised + shift-drag arbitrary placement. |
| **D6** | Legacy pattern recorder | Keep the data code, **disable the UI**, repurpose the Rec tab later for **sound-card input recording**. Nothing deleted yet. |

Snap choices: **1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, 1/64, Free**.

### What dropping the MIDI rows buys us

The biggest new subsystem in Revision 2 was MIDI output (`MidiOutput`, a device selector, timestamped sending, OS/driver latency risks, plus the reserved-note block). **All of it is gone.** The sequencer is now purely an internal pad trigger — no new device plumbing, no reserved notes, and the track count drops from 20 rows to 16, which also fixes the layout squeeze (§5).

### What D1 buys us

Because track *i* is pad *i*, the early ambiguity ("the sample should be executed by the current pad selected") disappears: **each track owns its pad**. Selecting a pad no longer routes anything — it just highlights that pad's row. Clicking pad 5 in the pad grid can scroll/highlight track 5.

---

## 2. What already exists (and why it shapes the plan)

| Thing | Where | Relevance |
|---|---|---|
| Pattern recorder | `recEvents` / `recQuantised`, `PatternEvent{padIndex, beatTime}` | Closest existing feature, but **wrong foundation** (§9). |
| Pattern playback | 10 ms `juce::Timer` → `tickPatternPlayback()` | Message-thread timing, fixed 0.8 velocity. Adds up to ~10 ms jitter. |
| Velocity | `LoopingSamplerVoice::startNote` → `lgain = rgain = velocity` | Already works in the audio path. |
| Pad triggering | `synthesizer.noteOn(ch, note, vel)` | How a step fires a pad. |
| Pad MIDI routing | `PadManager::renderNextBlock` passes **the same** MidiBuffer to **every** pad engine; routing relies on each pad's note range | Not deterministic enough for a sequencer — fixed by §3.4. |
| MIDI output | **None** — input only | No longer needed (D3). |
| Audio capture | `liveRenderWriter` taps the **master output** into WAV | Reusable for D6's input recording, but the source must change (§9). |
| UI space | SampleCard ≈ **720×375**; tab content ≈ **80 px** today | 16 rows now fit once the waveform is reclaimed (§5). |

---

## 3. Recommended architecture

### 3.1 Timing — PPQ ticks, not floats

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

struct SeqTrack {
    int  padIndex = 0;          // 0..15 — track i == pad i
    bool mute     = false;
    std::vector<SeqHit> hits;   // absolute ticks, arbitrary count
};

struct SeqPattern {
    static constexpr int kTracks = 16;
    SeqSnap snap = SeqSnap::Sixteenth;
    int     bars = 1;
    int     ppq  = 960;
    std::array<SeqTrack, kTracks> tracks;
};
```

Absolute ticks per hit (rather than a fixed step array) is what makes triplets **and** Free mode work with one model. The grid is a *view*: column = `tick / ticksPerStep`; in Free mode the columns are only a visual reference.

### 3.3 Playback engine (audio thread, sample-accurate)

The existing 10 ms timer is not good enough. Positions are computed **inside the audio callback**.

```cpp
class SequencerEngine {
public:
    void prepareToPlay (double sampleRate);
    void setPattern (std::shared_ptr<const SeqPattern>);   // lock-free publish
    void start(); void stop();
    void processBlock (int numSamples, double bpm,
                       std::array<juce::MidiBuffer, PadManager::kMaxPads>& padEvents);
private:
    std::shared_ptr<const SeqPattern> pattern;
    double playheadTicks = 0.0;
    std::atomic<bool> playing { false };
};
```

Per block: advance the playhead; for each of the 16 tracks find hits in `[startTick, endTick)`; add a note-on **at the exact sample offset** (plus a note-off after a short gate) into `padEvents[track.padIndex]`; wrap at `bars * 4 * PPQ`.

**Real-time rules:** no allocation, locks or logging in `processBlock`. `MidiBuffer::addEvent` can allocate, so **pre-reserve** every buffer in `prepareToPlay` and `clear()` each block.

**This adds no percussion latency.** It runs before `padManager.renderNextBlock` in the same callback, so events land in the synthesisers for the block being output. Sequenced hits are actually tighter than live MIDI, which has to come in through the OS and the `midiCollector` and can wait up to a block; a step carries an exact sample offset and never waits. CPU cost is a handful of comparisons plus a few note-ons per block.

### 3.4 Index-based routing — this is what makes D3 work

Extend `PadManager::renderNextBlock` with optional per-pad buffers:

```cpp
void renderNextBlock (juce::AudioBuffer<float>& out,
                      const juce::MidiBuffer& incoming,   // external MIDI, unchanged
                      int startSample, int numSamples,
                      const std::array<juce::MidiBuffer, kMaxPads>* seqEvents = nullptr);
```

Each engine renders `incoming` merged with `seqEvents[i]`. A step therefore fires **its own pad by index**, never "whatever pad happens to own note 60".

This is the mechanism behind the stand-alone request:

- **Sequencer → pads is index-based and independent of the MIDI note map.** No collisions, no dependence on a pad's assigned note.
- **Live MIDI → pads is unchanged** and goes through the existing collector.
- The two paths meet inside each synthesiser and simply mix. That is what lets the sequencer "stand alone from the pad": once a pattern is approved you can play the 16 pads live — or over MIDI — and the running pattern is unaffected, and vice versa.

### 3.5 Modes: Live / Record / Arrange

Three states for the Seq tab, one at a time:

| Mode | Sequencer | Incoming MIDI | Grid |
|---|---|---|---|
| **Live** | plays the approved pattern | plays pads live, does **not** touch the pattern | read-only |
| **Record** | can play or stop | **captured into the matching track** | writes hits |
| **Arrange** | stopped (or plays, your call) | ignored for capture | full mouse editing |

- **Record** maps an incoming note to a track via the pad's `midiNote` (the same `findPadForMidiNote` routing the app already uses). Velocity and timing are captured, snapped per the current resolution (or raw in Free).
- An optional **Lock** (a "pattern approved" padlock) makes Live read-only so a performance cannot disturb an approved take.
- Re-arranging is therefore explicit: hit **Record**, play the pads, done. No accidental edits during performance.
- **Note for capture quality:** live recording maps notes → pads, so pads that share a `midiNote` are ambiguous. A small "unique pad notes" validation/warning would help (nice-to-have, not required by index-based *playback*, which is always exact).

### 3.6 Thread safety

UI edits build a **new immutable `SeqPattern`**, published with an atomic `shared_ptr` store; the audio thread loads the pointer once per block. Retired snapshots are freed on the message thread via a small retire list / timer. Never free inline — that would be a use-after-free.

---

## 4. UI

A `SeqControlPanel` mirroring how `RecControlPanel` is structured and routed.

```
┌ Seq ──────────────────────────────────────────────────────────────────────┐
│ [▶][■][● Rec]  Snap:[1/16▾]  Bars:[1▾]  BPM:120  [Live/Arrange]  [Lock]   │
├──────┬────────────────────────────────────────────────────────────────────┤
│ 1 ◉  │ ■  ·  ·  ■  ·  ·  ■  ·  ■  ·  ·  ■  ·  ·  ■  ·   →  pad 1          │
│ 2 ◉  │ ·  ·  ■  ·  ·  ■  ·  ·  ·  ·  ■  ·  ·  ■  ·  ·   →  pad 2          │
│ 3 ◉  │ …                                                                  │
│ …    │                                            16 rows, one per pad    │
│ 16 ◉ │ …                                                                  │
└──────┴────────────────────────────────────────────────────────────────────┘
         ▲ fixed row header · horizontally scrollable grid · playhead column
```

- **Row header (fixed):** track no., pad marker, the pad's sample name, mute. Selecting a pad anywhere in the app highlights its row.
- **Grid:** inside a `juce::Viewport`; horizontal scroll for long patterns. 16 rows fit at the default size (§5), so vertical scroll is only a safety net for smaller windows.
- **Cell interaction:** left-click toggles a hit at the last-used velocity; **vertical drag sets velocity**; right-click clears. Velocity as a brightness ramp and/or a small in-cell bar.
- **Playhead:** one highlighted column, repainted from an `std::atomic<double>` published by the engine, ~30 fps, **repainting only that column**.
- **Audition:** clicking a cell fires its pad once so you hear exactly the sound you're sequencing.
- **Snap combo** holds all 9 options. Changing resolution **never moves existing hits** — add an explicit "Quantise now" later.
- **BPM** reads/writes the bank tempo (§8).

---

## 5. Layout

At the default window the tab content area is **≈80 px** because the waveform takes 175 px. Reclaiming the waveform (as the EQ tab already partly does) gives roughly **340 px** in the card.

- 16 rows × 18 px = **288 px**, plus a ~24 px toolbar and a ~16 px ruler ≈ **328 px** — it fits, just.
- So: **collapse/hide the waveform while Seq is active**, use ~18 px rows, and keep vertical scrolling available as a safety net for smaller windows.
- Horizontal scrolling handles patterns longer than the card width (at 16th-note resolution, one 4/4 bar = 16 steps, which fits ~720 px comfortably).

This is a much smaller problem than the 20-row version.

---

## 6. Live recording into the grid

The capture hook already exists in `handleIncomingMidiMessage` (it currently pushes `{flashPad, beatNow}` when armed). For the sequencer, in **Record** mode:

- Map note → track via `findPadForMidiNote` (pad note → its sample track).
- Capture `message.getFloatVelocity()` (already available) and the tick from `recSongBeatPos * PPQ`.
- Snap per the current resolution; raw tick in Free mode.
- Hand off to the message thread (lock-free FIFO or `callAsync`) — never write the live pattern from the MIDI callback thread.

---

## 7. Persistence

Per bank, inside the bank's `.jai`, so a kit carries its groove (D2).

```xml
<JaibaKit version="1" ...>
  <Pad index="0"> ... </Pad>
  ...
  <Sequencer snap="3" bars="1" ppq="960" bpm="120" mode="live">
    <Track pad="0" mute="0">
      <Hit tick="0"   vel="0.80"/>
      <Hit tick="240" vel="0.55"/>
    </Track>
    <Track pad="1" mute="0">
      <Hit tick="480" vel="0.70"/>
    </Track>
    ...
  </Sequencer>
</JaibaKit>
```

Touch points:
- `GjmBank` gains `SeqPattern sequence;`.
- `GjmManager::parseKitFile` currently returns `std::array<PadSettings,16>`; it must also return the sequence (new `KitData` struct or an out-parameter). `ensureBankReady` / `parseAllBanks` follow.
- `saveBankKit` writes the `<Sequencer>` block.
- The session (`.gjm`) references and rewrites each bank's `.jai`, so "Save Session" covers it automatically.

---

## 8. Metronome and tempo ownership (D4)

Sharing the metronome with the sequencer means **tempo must belong to the bank**, not to a pad — otherwise the click and the pattern can disagree. So:

- **BPM → stored with the bank** (in `SeqPattern`). The metronome's beat clock reads it. This replaces the global `recBpmAtomic` as the source of truth for the bank's tempo.
- **Metronome on/off → transport state** (bank-level UI, not necessarily persisted).
- **Metronome volume → bank level (confirmed).** With a bank-wide transport, "whose volume is the click?" has no good per-pad answer. A single bank-level click level removes the ambiguity parked in `docs/Click behaviour idea.md`. The per-pad `metVol` field stays in `PadSettings` for file compatibility and is superseded by the bank value.

---

## 9. Legacy pattern recorder (D6)

- The **pattern/MIDI-event** half is **not reusable** by the Seq engine — different timing model, no velocity, per-pad storage.
- But it **is serialised into existing `.jai` kits** (`savedPatterns` XML). Deleting the code now would silently drop any patterns saved in kits. So: **disable/hide the Rec-tab pattern UI, keep the parse/serialise code**, and mark it legacy. Remove only in a later, deliberate commit (optionally with a one-time migration).
- The **audio-capture** half is genuinely reusable for your future input recording: `AudioFormatWriter::ThreadedWriter`, the writer thread, file naming, and the trim pipeline. Note the current recorder taps the **master output** (§2); input recording means switching the source to `AudioDeviceManager` input channels and enabling inputs in the device setup. Keep that infrastructure.
- So: **don't remove — disable now, repurpose later.**

---

## 10. Phased delivery

| Phase | Deliverable |
|---|---|
| **0** | `SeqPattern` model + `.jai` round-trip (16 tracks). No UI. |
| **1** | Seq tab shell: 5th tab, panel, waveform reclaimed, 16-row grid, snap combo, click-to-toggle, ruler. **Silent.** |
| **2** | Audio-thread playback: play/stop, index-based per-pad routing, playhead, audition, bank tempo. |
| **3** | Velocity editing (drag) + **Record mode** + Free mode + mode switching (§3.5). |
| **4** | Polish: bars/length, mute, copy/paste, swing, undo, quantise-now, "follow pad selection" highlight, unique-pad-note warning. |

Phases 0–2 give a working 16-track drum sequencer; 3 completes the "hit + velocity + snap" requirement.

---

## 11. Risks

- **RT safety.** Pre-reserve all `MidiBuffer`s in `prepareToPlay`; assert no allocation in `processBlock`.
- **Note gating.** A step needs a note-off: fixed short gate (e.g. 30 ms) or off-at-next-hit; respect a pad's own one-shot behaviour. Decide in Phase 2.
- **Snapshot lifetime.** Freeing a retired pattern while the audio thread still holds it is a use-after-free. Retire on the message thread only.
- **Layout.** 16 rows fit at the default window *only* if the waveform is reclaimed on the Seq tab. Validate in Phase 1.
- **Scope.** Much reduced now that MIDI output is gone, but still: 16 tracks + velocity + 8 snap resolutions + recording modes.
- **Tempo migration.** Moving BPM from the global `recBpmAtomic` to per-bank touches the existing Rec/pattern paths — do it in Phase 2 and keep the old atomic as a mirror while both features coexist.
- **External MIDI clock/sync** is out of scope (internal clock only).

---

## 12. Files likely to change

| File | Change |
|---|---|
| `src/Sequencer.h` *(new)* | Model (`SeqSnap`, `SeqHit`, `SeqTrack`, `SeqPattern`) + `SequencerEngine` |
| `src/SequencerGrid.h` *(new)* | 16-row grid, row header, playhead, mode/snap toolbar |
| `src/SampleCard.h` | 5th tab button, content routing, waveform reclaim, `setSeqTabVisible` |
| `src/MainComponent.h/.cpp` | Own engine + transport + modes; call `processBlock`; live capture; bank tempo |
| `src/PadManager.h` | Optional per-pad event buffers in `renderNextBlock` |
| `src/GjmManager.h` | `GjmBank::sequence`, parse + save `<Sequencer>`, bank tempo |
| `CMakeLists.txt` | List the new headers |

`ConfigurationManager` is **unchanged** — no MIDI output device to persist any more.

---

## 13. Suggested first move

**Phase 0 + Phase 1, then stop and look.** A Seq tab that reclaims the waveform, draws the 16-row grid with the 9 snap choices and the mode toggle, and lets you click steps in and out — saving and reloading with the kit, but silent. That validates the two real risks (the layout fit and the `.jai` shape) before any audio-thread work, and it is cheap to discard if the interaction doesn't feel right.

All decisions are now closed. Ready to start Phase 0 + 1 on your word.
