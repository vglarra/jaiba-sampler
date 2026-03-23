#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>
#include <memory>

//==============================================================================
// SamplerSound subclass.
//   fullAudioData  — shared_ptr to the FULL original buffer (owned jointly with MappedSample).
//                    Safe even after a sample reload because the refcount keeps it alive.
//   startSampleAtomic / endSampleAtomic — absolute positions in fullAudioData, updated
//                    at any time by the message thread while the audio thread is running.
//   loopEnabled    — toggled at runtime without rebuilding the sound.
class LoopingSamplerSound : public juce::SamplerSound
{
public:
    LoopingSamplerSound(const juce::String& name,
                        juce::AudioFormatReader& source,
                        const juce::BigInteger& notes,
                        int midiRootNote,
                        double attackSecs,
                        double releaseSecs,
                        double maxLenSecs)
        : juce::SamplerSound(name, source, notes, midiRootNote,
                             attackSecs, releaseSecs, maxLenSecs),
          midiRootNote(midiRootNote),
          sourceSampleRate(source.sampleRate),
          adsrAttack(static_cast<float>(attackSecs)),
          adsrRelease(static_cast<float>(releaseSecs))
    {}

    const int    midiRootNote;
    const double sourceSampleRate;
    const float  adsrAttack;
    const float  adsrRelease;

    // Set once at construction, then only read — no lock needed.
    std::shared_ptr<juce::AudioBuffer<float>> fullAudioData;

    // Written by message thread, read by audio thread — must be atomic.
    std::atomic<juce::int64> startSampleAtomic  { 0 };
    std::atomic<juce::int64> endSampleAtomic    { 0 };
    std::atomic<bool>        loopEnabled         { false };
    // Pitch offset in semitones (±48). Voice multiplies basePitchRatio by
    // pow(2, offset/12) once per block — zero extra cost per sample.
    std::atomic<int>         pitchOffsetAtomic   { 0 };
    // Freeze: when true the voice ignores note-offs and always loops.
    // Must be cleared BEFORE calling allNotesOff so stopNote actually fires.
    std::atomic<bool>        freezeActive        { false };
    // OneShot: when true the voice ignores note-offs and plays to the end point without looping.
    // Loop is suppressed even if loopEnabled is true.
    std::atomic<bool>        oneShotEnabled      { false };
    // Base tuning ratio: baseTuningHz / 440.0. Updated by message thread when user changes tuning.
    std::atomic<float>       baseTuningRatioAtomic { 1.0f };
};

//==============================================================================
// Custom SynthesiserVoice with real-time start/end point support and looping.
// Reads start/end from the sound's atomics once per block so the message thread
// can update them at any time without blocking or memory allocation.
class LoopingSamplerVoice : public juce::SynthesiserVoice
{
public:
    bool canPlaySound(juce::SynthesiserSound* s) override
    {
        return dynamic_cast<LoopingSamplerSound*>(s) != nullptr;
    }

    void startNote(int midiNoteNumber, float velocity,
                   juce::SynthesiserSound* s, int /*pitchWheel*/) override
    {
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(s))
        {
            // basePitchRatio: MIDI transpose only (no pitch offset baked in).
            // pitchOffsetAtomic is applied per block so changes take effect instantly.
            basePitchRatio = std::pow(2.0, (midiNoteNumber - sound->midiRootNote) / 12.0)
                             * sound->sourceSampleRate / getSampleRate();
            const int offset = sound->pitchOffsetAtomic.load();
            const float tuningRatio = sound->baseTuningRatioAtomic.load();
            pitchRatio = basePitchRatio * (double)tuningRatio * std::pow(2.0, offset / 1200.0);

            // Start playback at the current start point (atomic read).
            sourceSamplePosition = (double)sound->startSampleAtomic.load();
            lgain = velocity;
            rgain = velocity;

            juce::ADSR::Parameters params;
            params.attack  = sound->adsrAttack;
            params.release = sound->adsrRelease;
            adsr.setSampleRate(sound->sourceSampleRate);
            adsr.setParameters(params);
            adsr.noteOn();
        }
    }

    void stopNote(float /*velocity*/, bool allowTailOff) override
    {
        // While freeze is active ignore note-offs so the loop keeps running.
        // freezeActive is cleared on the message thread BEFORE allNotesOff is called,
        // so the voice will stop correctly when freeze is turned off.
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(getCurrentlyPlayingSound().get()))
            if (sound->freezeActive.load() || sound->oneShotEnabled.load())
                return;

        if (allowTailOff)
            adsr.noteOff();
        else
        {
            clearCurrentNote();
            adsr.reset();
        }
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    // Called from message thread during stop-before-load to unconditionally release
    // the currentlyPlayingSound ref-counted pointer.  Must be called with muteOutput=true
    // so the audio thread is not inside renderNextBlock concurrently.
    void forceStop()
    {
        clearCurrentNote();   // sets currentlyPlayingSound = nullptr (releases ref-count)
        adsr.reset();
        sourceSamplePosition = 0.0;
    }

    void renderNextBlock(juce::AudioBuffer<float>& output,
                         int startSample, int numSamples) override
    {
        if (auto* sound = static_cast<LoopingSamplerSound*>(getCurrentlyPlayingSound().get()))
        {
            // fullAudioData is set once at construction and never changed — safe without lock.
            auto* data = sound->fullAudioData.get();
            if (data == nullptr) return;

            // Read start/end/pitch once per block — atomic, no lock, negligible cost.
            const juce::int64 sStart = sound->startSampleAtomic.load();
            const juce::int64 sEnd   = sound->endSampleAtomic.load();

            // Re-apply pitch offset so changes from the message thread are heard
            // within one block (~6 ms) — smooth, no pop (just rate change, not position jump).
            const int offset = sound->pitchOffsetAtomic.load();
            const float tuningRatio = sound->baseTuningRatioAtomic.load();
            pitchRatio = basePitchRatio * (double)tuningRatio * std::pow(2.0, offset / 1200.0);
            if (sStart >= sEnd) return;

            const double regionLen = (double)(sEnd - sStart);

            // maxSafePos: highest pos where inL[pos+1] is still within the buffer.
            const int maxSafePos = data->getNumSamples() - 2;
            if (maxSafePos < 0) return;

            const float* inL = data->getReadPointer(0);
            const float* inR = data->getNumChannels() > 1 ? data->getReadPointer(1) : nullptr;

            float* outL = output.getWritePointer(0, startSample);
            float* outR = output.getNumChannels() > 1
                              ? output.getWritePointer(1, startSample) : nullptr;

            // OneShot overrides loop: even if loopEnabled is true, we play through once only.
            const bool shouldLoop = (sound->loopEnabled.load() || sound->freezeActive.load())
                                    && !sound->oneShotEnabled.load();

            // Clamp position into [sStart, sEnd) in case the atomics just changed.
            if (sourceSamplePosition < (double)sStart)
                sourceSamplePosition = (double)sStart;

            if (sourceSamplePosition >= (double)sEnd)
            {
                if (shouldLoop)
                    sourceSamplePosition = (double)sStart
                        + std::fmod(sourceSamplePosition - (double)sStart, regionLen);
                else
                {
                    // Use direct stop to bypass the oneshot/freeze early-return guard in stopNote.
                    clearCurrentNote();
                    adsr.reset();
                    return;
                }
            }

            while (--numSamples >= 0)
            {
                // Safety clamp for interpolation — never read past end of buffer.
                int pos = juce::jmin((int)sourceSamplePosition, maxSafePos);
                auto alpha    = (float)(sourceSamplePosition - (double)pos);
                auto invAlpha = 1.0f - alpha;

                float l = inL[pos] * invAlpha + inL[pos + 1] * alpha;
                float r = inR ? (inR[pos] * invAlpha + inR[pos + 1] * alpha) : l;

                auto env = adsr.getNextSample();

                // If the ADSR envelope has finished (release tail complete), stop the voice.
                if (!adsr.isActive())
                {
                    clearCurrentNote();
                    break;
                }

                l *= lgain * env;
                r *= rgain * env;

                if (outR != nullptr) { *outL++ += l; *outR++ += r; }
                else                 { *outL++ += (l + r) * 0.5f; }

                sourceSamplePosition += pitchRatio;

                if (sourceSamplePosition >= (double)sEnd)
                {
                    if (shouldLoop)
                        sourceSamplePosition = (double)sStart
                            + std::fmod(sourceSamplePosition - (double)sStart, regionLen);
                    else
                    {
                        // Use direct stop to bypass the oneshot/freeze early-return guard in stopNote.
                        clearCurrentNote();
                        adsr.reset();
                        break;
                    }
                }
            }
        }
    }

private:
    double basePitchRatio = 0.0;       // MIDI transpose only — set in startNote, never changes
    double pitchRatio = 0.0;           // basePitchRatio * pow(2, offset/12) — updated per block
    double sourceSamplePosition = 0.0;
    float  lgain = 0.0f, rgain = 0.0f;
    juce::ADSR adsr;
};
