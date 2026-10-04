#!/usr/bin/env bash
# Builds and runs the sequencer test harnesses against the objects already produced
# by a normal build.  No audio device, no GUI, no fixture files: the audio tests
# synthesise their own buffers.
#
#   ninja -C build && tests/run_tests.sh
set -euo pipefail

cd "$(dirname "$0")/.."

B=build/CMakeFiles/JaibaSampler.dir/lib/JUCE/modules
if [ ! -d "$B" ]; then
    echo "Build first:  ninja -C build" >&2
    exit 1
fi

CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}
OUT=${OUT%/}/jaiba_seq_tests

FLAGS=(-g -std=gnu++17
       -DDEBUG=1 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
       -DJUCE_STANDALONE_APPLICATION=1 -DLINUX=1 -D_DEBUG=1
       -Ibuild/JaibaSampler_artefacts/JuceLibraryCode -Ilib/JUCE/modules -Isrc)

LINK=(-lpthread -ldl -lrt)

status=0

echo "== sequencer model tests =="
"$CXX" "${FLAGS[@]}" \
    -DJUCE_MODULE_AVAILABLE_juce_core=1 \
    -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 \
    -DJUCE_MODULE_AVAILABLE_juce_data_structures=1 \
    -DJUCE_MODULE_AVAILABLE_juce_events=1 \
    tests/sequencer_tests.cpp \
    "$B/juce_core/juce_core.cpp.o" \
    "$B/juce_core/juce_core_CompilationTime.cpp.o" \
    "$B/juce_audio_basics/juce_audio_basics.cpp.o" \
    "$B/juce_data_structures/juce_data_structures.cpp.o" \
    "$B/juce_events/juce_events.cpp.o" \
    -o "${OUT}_model" "${LINK[@]}"
"${OUT}_model" || status=1

echo
echo "== sequencer audio tests =="
# Renders the pool and asserts sample offsets.  Needs juce_dsp because
# PadAudioEngine owns an FFT (disabled for pool engines, but still linked).
"$CXX" "${FLAGS[@]}" \
    -DJUCE_MODULE_AVAILABLE_juce_core=1 \
    -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 \
    -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 \
    -DJUCE_MODULE_AVAILABLE_juce_data_structures=1 \
    -DJUCE_MODULE_AVAILABLE_juce_events=1 \
    -DJUCE_MODULE_AVAILABLE_juce_dsp=1 \
    tests/sequencer_audio_tests.cpp \
    "$B/juce_core/juce_core.cpp.o" \
    "$B/juce_core/juce_core_CompilationTime.cpp.o" \
    "$B/juce_audio_basics/juce_audio_basics.cpp.o" \
    "$B/juce_audio_formats/juce_audio_formats.cpp.o" \
    "$B/juce_data_structures/juce_data_structures.cpp.o" \
    "$B/juce_events/juce_events.cpp.o" \
    "$B/juce_dsp/juce_dsp.cpp.o" \
    -o "${OUT}_audio" "${LINK[@]}"
"${OUT}_audio" || status=1

exit $status
