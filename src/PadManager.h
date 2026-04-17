#pragma once

#include <array>
#include <memory>
#include <atomic>

#include "PadAudioEngine.h"

//==============================================================================
// PadManager — owns up to 16 PadAudioEngine instances and mixes them into one
// stereo output buffer.
//
// Responsibilities:
//   • Lifetime management of PadAudioEngines (index 0–15).
//   • prepareToPlay / releaseResources delegation to each active engine.
//   • renderNextBlock: delegates to each engine in [0, numSamples), then
//     applies master volume to the final mixed result.
//   • Master volume atomic (applied AFTER all pads are mixed).
//
// For the current single-pad architecture, only engine[0] is populated.
// The 16-pad expansion only requires wiring more engines and MIDI routing
// without changing any existing engine or MainComponent code paths.
//
// MIDI routing:
//   In the current architecture MainComponent passes a pre-assembled MidiBuffer
//   (from midiCollector) directly into PadManager::renderNextBlock.  The
//   manager forwards that buffer to engine[0] only.  Future MIDI routing
//   (per-pad channel/note filter) can be added here without touching engines.
//==============================================================================

class PadManager
{
public:
    //==========================================================================
    explicit PadManager(juce::AudioFormatManager& fmt)
    {
        // Create engine 0 (the only active pad in the current single-pad build).
        engines[0] = std::make_unique<PadAudioEngine>(fmt);

        // Engines 1–15 are intentionally left null.  They will be created on
        // demand when multi-pad support is added in a later session.
    }

    ~PadManager() = default;

    //==========================================================================
    // AudioAppComponent delegation

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate)
    {
        for (auto& e : engines)
            if (e != nullptr)
                e->prepareToPlay(samplesPerBlockExpected, sampleRate);
    }

    void releaseResources()
    {
        for (auto& e : engines)
            if (e != nullptr)
                e->releaseResources();
    }

    // Render all active pads into bufferToFill, then apply master volume.
    // Called from MainComponent::getNextAudioBlock after the mute guard and
    // before the sine wave and heartbeat code.
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         const juce::MidiBuffer& midiMessages,
                         int startSample, int numSamples)
    {
        // Route MIDI and audio for each active engine.
        // Currently only engine[0] is active; this loop is ready for 16 pads.
        for (int padIdx = 0; padIdx < kMaxPads; ++padIdx)
        {
            auto& e = engines[padIdx];
            if (e == nullptr)            continue;
            if (e->muteOutput.load())    continue;  // engine silenced during load

            // Future: filter midiMessages by pad MIDI note/channel here.
            // For now, pad 0 receives the full MIDI stream (identical to old behavior).
            e->renderNextBlock(outputBuffer, midiMessages, startSample, numSamples);
        }

        // Apply master volume last — scales the fully mixed output.
        {
            float mg = masterVolumeGain.load();
            if (mg != 1.0f)
                outputBuffer.applyGain(startSample, numSamples, mg);
        }
    }

    //==========================================================================
    // Engine access

    static constexpr int kMaxPads = 16;

    // Returns a reference to engine[index].  Creates it on first access if null.
    // index must be in [0, kMaxPads).
    PadAudioEngine& getEngine(int index)
    {
        jassert(index >= 0 && index < kMaxPads);
        // Engine 0 is always created in the constructor.
        // Others are created lazily when needed.
        if (engines[index] == nullptr)
        {
            // Note: formatManager must stay alive for the PadAudioEngine lifetime.
            // PadManager::PadManager stores it by reference in engine[0]; for future
            // engines callers must ensure this.  For safety we jassert here.
            jassertfalse;  // Caller must add lazy-create support before using pad > 0
        }
        return *engines[index];
    }

    const PadAudioEngine& getEngine(int index) const
    {
        jassert(index >= 0 && index < kMaxPads);
        jassert(engines[index] != nullptr);
        return *engines[index];
    }

    bool hasEngine(int index) const
    {
        return index >= 0 && index < kMaxPads && engines[index] != nullptr;
    }

    //==========================================================================
    // Master volume — applied post-mix

    void setMasterVolume(float gain)
    {
        masterVolumeGain.store(juce::jlimit(0.0f, 1.0f, gain));
    }

    float getMasterVolume() const { return masterVolumeGain.load(); }

    //==========================================================================
    // Panic — hard-stop all engines simultaneously

    void stopAllPads()
    {
        for (auto& e : engines)
        {
            if (e == nullptr) continue;
            e->clearActiveSoundFlags();
            e->forceStopAllVoices();
            e->allNotesOff(0, false);
        }
    }

private:
    std::array<std::unique_ptr<PadAudioEngine>, kMaxPads> engines;
    std::atomic<float> masterVolumeGain { 0.7f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PadManager)
};
