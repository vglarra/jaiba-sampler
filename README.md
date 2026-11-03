# Jaiba Sampler

A standalone MIDI-triggered audio sampler built with JUCE.

## Jaiba three-device setup

- The drum is three USB-MIDI devices with fixed names: `Jaiba Right`,
  `Jaiba Left`, and the Teensy knob hub `Jaiba Knobs`. The app auto-detects
  all three on every launch, retries while one is absent, and reopens after a
  replug — there is no device selection to lose.
- The two sensor streams are merged. MIDI notes are disjoint (right 60–76,
  left 78–90), so sampler pad *i* uses note `60 + 2*i`.
- The knob hub sends CC only. Its fixed map is applied to the **currently
  selected pad** (and the global master/panic):

  | Control | CC | Sampler target |
  |---|---|---|
  | Selected Pad Pitch | 20 (MSB) / 52 (LSB), 14-bit | `pitchCents`, ±4800 |
  | Master Volume | 7 | master gain |
  | Selected Pad Volume | 22 | pad `volumeLevel` |
  | Filter Cutoff | 23 | EQ band 1 frequency, 20 Hz–20 kHz |
  | Resonance | 24 | EQ band 1 Q, 0.5–10 |
  | Attack | 25 | ADSR attack, 0–2000 ms |
  | Release | 26 | ADSR release, 0–5000 ms |
  | Panic | 27 | all notes off |
- The map is only applied to the `Jaiba Knobs` input; a manual controller on
  the MIDI Input dialog does not move these controls.
- The last loaded session/kit is restored on startup. The setup autosaves on
  quit and every 30 s while dirty, so pad mappings, knob-driven parameters and
  MIDI-learned notes survive restarts.
- `~/Documents/sounds/jaiva-kits/jaiba-16-map.jai` is an empty kit with all 16
  pads mapped to the right notes — load it once and add samples.
- The MIDI Input dialog is only for an extra/manual controller; the three
  Jaiba devices never need to be selected there.
- Losing the knob hub does not panic the sensor notes; losing a sensor board
  still panics the drums so a held note cannot stick.

## Building

This project uses JUCE as a git submodule, so clone with `--recursive`
(or run `git submodule update --init` after a normal clone):

```bash
git clone --recursive https://github.com/vglarra/jaiba-sampler.git
cd jaiba-sampler
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

## Screenshots

| Sampler view | Visualizer mode |
|---|---|
| ![Sampler view](docs/screenshots/jaiba-sampler-front-sampler-view_2026-06-14.png) | ![Visualizer mode](docs/screenshots/jaiba-sampler-front-viz-mode_2026-06-14.png) |

## Features
- Audio file loading (WAV/AIFF/MP3)
- MIDI input handling
- Sample playback with looping
- ADSR envelope
- Waveform display
- 16-pad sampler with individual settings
- Three-device auto-detect: right sensor, left sensor, knob hub
- Knob-hub fixed CC map (pitch, volumes, EQ, ADSR, panic)
- EQ and filtering
- Normalization
- Trim functionality
- Session persistence

## Usage
1. Launch the application
2. Click the "+" button to load a sample
3. Use the sample card to adjust pitch, volume, start/end points
4. Use the pad grid to switch between different samples
5. Connect a MIDI controller to trigger samples

## Keyboard Shortcuts
- **Escape**: Panic reset (stops all audio)
- **Arrow Keys**: Navigate through samples in folder

## Project Structure
- `src/`: Source code
  - `MainComponent.cpp/h`: Main application component
  - `SampleCard.cpp/h`: Sample editing interface
  - `PadManager.cpp/h`: Manages 16 audio pads
  - `PadAudioEngine.cpp/h`: Audio processing for each pad
- `lib/JUCE/`: JUCE framework
- `assets/`: Sample assets and resources