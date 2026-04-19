#pragma once

#include <array>
#include <memory>
#include <atomic>

#include "PadAudioEngine.h"
#include "PadSettings.h"

//==============================================================================
// PadManager — owns up to 16 PadAudioEngine instances and mixes them into one
// stereo output buffer.
//
// Responsibilities:
//   • Lifetime management of PadAudioEngines (index 0–15), lazily created.
//   • prepareToPlay / releaseResources delegation to each active engine.
//   • renderNextBlock: delegates to each engine, then applies master volume.
//   • Per-pad PadSettings array (serializable state for all 16 pads).
//   • Selected pad tracking (which pad the SampleCard UI is connected to).
//   • MIDI routing helper: findPadForMidiNote.
//   • Persistence: saveAllPads / loadAllPads.
//==============================================================================

class PadManager
{
public:
    static constexpr int kMaxPads = 16;

    //==========================================================================
    explicit PadManager(juce::AudioFormatManager& fmt)
        : formatManager(fmt)
    {
        // Initialise each PadSettings with its pad index.
        for (int i = 0; i < kMaxPads; ++i)
            padSettings[i].padIndex = i;

        // Create engine 0 immediately — it is always active.
        engines[0] = std::make_unique<PadAudioEngine>(fmt);
        // Engines 1–15 are created lazily on first getEngine() call.
    }

    ~PadManager() = default;

    //==========================================================================
    // AudioAppComponent delegation

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate)
    {
        // Cache for lazily-created engines that arrive after prepareToPlay.
        lastBlockSize  = samplesPerBlockExpected;
        lastSampleRate = sampleRate;

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

    // Render all active pads into outputBuffer, then apply master volume.
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         const juce::MidiBuffer& midiMessages,
                         int startSample, int numSamples)
    {
        for (int padIdx = 0; padIdx < kMaxPads; ++padIdx)
        {
            auto& e = engines[padIdx];
            if (e == nullptr)         continue;
            if (e->muteOutput.load()) continue;

            // Future: per-pad MIDI note/channel filter here.
            e->renderNextBlock(outputBuffer, midiMessages, startSample, numSamples);
        }

        // Apply master volume last — scales the fully mixed output.
        float mg = masterVolumeGain.load();
        if (mg != 1.0f)
            outputBuffer.applyGain(startSample, numSamples, mg);
    }

    //==========================================================================
    // Engine access

    // Returns engine[index], creating it lazily if needed.
    PadAudioEngine& getEngine(int index)
    {
        jassert(index >= 0 && index < kMaxPads);
        if (engines[index] == nullptr)
        {
            engines[index] = std::make_unique<PadAudioEngine>(formatManager);
            // Prepare the new engine if the audio device is already running.
            if (lastSampleRate > 0.0)
                engines[index]->prepareToPlay(lastBlockSize, lastSampleRate);
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
    // Selected pad — which pad the SampleCard UI is currently connected to.

    int selectedPadIndex = 0;

    void selectPad(int index)
    {
        jassert(index >= 0 && index < kMaxPads);
        selectedPadIndex = index;
    }

    //==========================================================================
    // Settings access

    PadSettings& getSettings(int index)
    {
        jassert(index >= 0 && index < kMaxPads);
        return padSettings[index];
    }

    const PadSettings& getSettings(int index) const
    {
        jassert(index >= 0 && index < kMaxPads);
        return padSettings[index];
    }

    //==========================================================================
    // MIDI routing — find which pad is mapped to a given note+channel.
    // Returns pad index [0,15] or -1 if none match.
    int findPadForMidiNote(int note, int channel) const
    {
        for (int i = 0; i < kMaxPads; ++i)
            if (padSettings[i].midiNote    == note &&
                padSettings[i].midiChannel == channel)
                return i;
        return -1;
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

    //==========================================================================
    // Persistence — read/write all 16 pad settings to a PropertiesFile.

    void saveAllPads(juce::PropertiesFile* props) const
    {
        if (props == nullptr) return;
        for (int i = 0; i < kMaxPads; ++i)
            padSettings[i].saveToProperties(props);
    }

    void loadAllPads(const juce::PropertiesFile* props)
    {
        if (props == nullptr) return;
        for (int i = 0; i < kMaxPads; ++i)
            padSettings[i].loadFromProperties(props);
    }

    //==========================================================================
    // Per-pad settings (public — MainComponent orchestrates complex sequences)
    PadSettings padSettings[kMaxPads];

private:
    juce::AudioFormatManager& formatManager;
    std::array<std::unique_ptr<PadAudioEngine>, kMaxPads> engines;
    std::atomic<float> masterVolumeGain { 0.7f };

    // Cached audio device params for lazily-created engines.
    int    lastBlockSize  = 0;
    double lastSampleRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PadManager)
};
