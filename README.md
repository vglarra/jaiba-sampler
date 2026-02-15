# Add project documentation
echo "# My Sampler

A standalone audio sampler built with JUCE.

## Building
\`\`\`bash
git clone --recursive https://github.com/yourusername/mysampler.git
cd mysampler
cmake -B build -G \"Visual Studio 17 2022\"
cmake --build build --config Debug
\`\`\`

## Features (Coming Soon)
- [ ] Audio file loading (WAV/AIFF)
- [ ] MIDI input handling
- [ ] Sample playback
- [ ] ADSR envelope
- [ ] Waveform display
" > README.md

git add README.md
git commit -m "Add README with build instructions"