# Jaiva Sampler

A standalone MIDI-triggered audio sampler built with JUCE.

## Building

This project uses JUCE as a git submodule, so clone with `--recursive`
(or run `git submodule update --init` after a normal clone):

```bash
git clone --recursive https://github.com/<your-github-username>/jaiva-sampler.git
cd jaiva-sampler
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

## Features
- Audio file loading (WAV/AIFF/MP3)
- MIDI input handling
- Sample playback with looping
- ADSR envelope
- Waveform display
- 16-pad sampler with individual settings
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