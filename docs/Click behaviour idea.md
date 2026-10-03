# Metronome click behaviour — design idea

Status: **not decided, not implemented.** This note parks the open questions so we
can pick a direction later. Everything below is a proposal, not current behaviour.

## Goal

Let the metronome play a **loaded sample** (a click) instead of the built-in synth
beep, and let each pad's Rec tab remember its own click settings so a kit/session
restores exactly what you were working with.

## Where it stands today (verified in code)

- **The "click" is not a sample.** It's a 1000 Hz sine, 20 ms burst, windowed by a
  sine envelope, synthesised in the audio callback
  (`MainComponent::getNextAudioBlock`, the "Beat clock + metronome beep" block).
  No buffer, no file, no slot to point at a sample.
- **Trigger** happens on each whole-beat crossing, and the beep is mixed from
  **sample 0 of that audio block** — so the click is quantised to the buffer size
  (roughly 0–11 ms of jitter at 512 samples / 44.1 kHz). True today with the sine;
  loading a sample neither helps nor hurts it.
- **Volume is already per-pad.** `PadSettings::metronomeVolume` (serialised as
  `metVol` in both `.jai` XML and Properties). It is restored on pad switch, kit
  load, bank switch and session load, and marks the kit dirty (`*`) when moved.
- **Tempo is *not* per-pad.** BPM lives in `RecControlPanel::currentBpm`
  (default 120) plus the global `recBpmAtomic`. It is set **only** by tap tempo
  (there is no BPM field), and switching pads restores nothing.
- **One click at a time is inherent** — there is a single beep generator, and all
  16 pads play over it because it is mixed into the master output independently of
  pad selection.
- **Where settings persist.** `PadSettings` is written to the bank's `.jai` kit
  file; global-pad settings are written inline in the `.gjm` session manifest. Kit
  pads are referenced from the `.gjm` via per-bank `.jai` files. So anything added
  to `PadSettings` rides along in both the kit and the session for free.

## Agreed direction

- **One** metronome click at a time; all pads play over that single click.
- Each pad's Rec tab **remembers its own tempo and its own click sample**
  (and volume, already done).
- Enabling the metronome on a pad while another pad's click is playing **turns the
  previous one off**.

## Open questions — needs a decision

### Q1. What happens when you switch pads while the click is running?

Two mutually exclusive models. Both keep everything agreed above; they differ only
in this one case.

**A. Owner model** — the click belongs to the pad that switched it on.

- Switching to another pad leaves the click running on the **owner's** tempo,
  sample and volume.
- The newly displayed pad's Metro button reads **off** (it is not the owner).
- Pressing it on the new pad **stops the previous** click and starts the new pad's
  click with the new pad's tempo/sample/volume.
- Matches the original description ("the new one toggles off the previous").
- Implies a `metronomeOwnerPad` index (−1 = off) and that the Rec tab's Metro
  toggle reflects ownership rather than a plain global flag.

**B. Follows-the-display** — one global Metro on/off.

- Switching pads **immediately** swaps the running click to the displayed pad's
  tempo, sample and volume.
- Simpler: no ownership state; the Metro toggle is a single global flag.
- Downside: merely browsing pads retunes a running click, and it would retune a
  take mid-recording if switching is allowed while recording.

*(Recommendation offered: A, because it matches the stated behaviour and stops
pad browsing from disturbing a running click. B is simpler but more surprising.)*

### Q2. Should the metronome be locked during an active recording?

- **Locked (suggested):** the Metro button and tempo are frozen while a take is
  armed/recording, so the beat clock cannot shift mid-take.
- **Unlocked:** you can retune or switch the click mid-take.

This matters more under model **B**, where switching pads alone would change tempo.

### Q3. On handover, restart the beat or keep the phase? (only relevant to model A)

- **Restart at beat 1 (suggested):** the downbeat lines up with the new tempo.
- **Keep the phase:** the click continues, but the first beat under the new tempo
  may land off-grid.

## What it would take (proposed implementation, for later)

1. **Per-pad tempo** — add `PadSettings::bpm` (double, default 120), a
   `setBpm`/`getBpm` on the Rec panel, restore it in `updateUIFromSettings`, and
   persist it when tap tempo changes it. Mirrors the volume work already done.
2. **Per-pad click path** — add `PadSettings::clickSamplePath` (String, empty =
   use the built-in sine), serialised like the other fields.
3. **Audio player** — decode the click once, on the message/background thread, into
   a `juce::AudioBuffer<float>`; resample to the device sample rate at load time;
   publish via an `std::atomic` pointer swap. Never allocate, lock or touch disk
   inside `getNextAudioBlock`.
4. **Voice state** — on each beat, start a playhead into the active click buffer
   instead of `recMetroBeepLeft`; fall back to the existing sine when no click is
   assigned. Only the **active** click ever needs decoding, so RAM stays at roughly
   one click (~10 KB for a 50 ms mono click), not 16.
5. **UI** — a small "Click" load/clear button in the Rec tab next to the existing
   volume slider (the slider stays a horizontal slider).
6. **Owner handling (if model A)** — track `metronomeOwnerPad`; the Metro button
   shows/hides ownership; handover stops the previous click.

### Notes / caveats

- **Samples are not embedded** in `.jai`/`.gjm` — only the path is. Moving or
  renaming the WAV makes the click go silent, same as pad samples today.
- **Latency:** selecting a per-pad click adds **no** latency. The audio callback
  still reads an in-RAM buffer; a buffer read is cheaper than the per-sample
  `std::sin()` it replaces. The only requirement is the lock-free pointer swap so a
  pad switch can never block the audio thread (or you get a dropout, not latency).
- **Timing:** independent of samples, the beat trigger is block-quantised. If a
  tighter click is wanted, compute the beat's fractional position inside the block
  and start the click at that exact sample offset.

## Recommendation

Model **A** (owner), metro **locked during recording**, beat **restarted at beat 1**
on handover. This keeps a running click undisturbed by pad browsing, which is the
least surprising behaviour when playing a kit over the click.
