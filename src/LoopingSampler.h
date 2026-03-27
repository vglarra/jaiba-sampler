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
    // Custom ADSR envelope. When customAdsrEnabled=true the voice uses these parameters
    // instead of the default short attack/release. All values applied on the next noteOn.
    std::atomic<bool>        customAdsrEnabled   { false };
    std::atomic<float>       customAdsrAttackMs  { 0.0f };
    std::atomic<float>       customAdsrDecayMs   { 0.0f };
    std::atomic<float>       customAdsrSustain   { 1.0f };
    std::atomic<float>       customAdsrReleaseMs { 0.0f };
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
            if (sound->customAdsrEnabled.load())
            {
                // User-configured ADSR envelope
                params.attack  = juce::jmax(0.001f, sound->customAdsrAttackMs.load()  / 1000.0f);
                params.decay   = juce::jmax(0.001f, sound->customAdsrDecayMs.load()   / 1000.0f);
                params.sustain = juce::jlimit(0.0f, 1.0f, sound->customAdsrSustain.load());
                params.release = juce::jmax(0.001f, sound->customAdsrReleaseMs.load() / 1000.0f);
            }
            else
            {
                // Default: short fade-in/fade-out for click elimination
                params.attack  = sound->adsrAttack;
                params.release = sound->adsrRelease;
            }
            // ADSR must use the DEVICE sample rate, not the source file's sample rate.
            // The ADSR advances one step per device output sample; using sourceSampleRate
            // here would cause wrong timing whenever file SR != device SR.
            adsr.setSampleRate(getSampleRate());
            adsr.setParameters(params);
            adsr.noteOn();
            noteIsHeld = true;

            // Precompute manual envelope sample counts for the custom ADSR path.
            // These are recalculated each block in renderNextBlock for real-time knob updates,
            // but initialising here ensures correct values on the very first sample.
            envSamplePosition = 0;
            inRelease         = false;
            releasePosition   = -1;
            debugEnvCounter   = 0;
            if (sound->customAdsrEnabled.load())
            {
                const double sr = getSampleRate();
                attackSamples  = juce::jmax(0, (int)(sound->customAdsrAttackMs.load()  / 1000.0f * sr));
                decaySamples   = juce::jmax(0, (int)(sound->customAdsrDecayMs.load()   / 1000.0f * sr));
                releaseSamples = juce::jmax(1, (int)(sound->customAdsrReleaseMs.load() / 1000.0f * sr));
                sustainLevel   = juce::jlimit(0.0f, 1.0f, sound->customAdsrSustain.load());
            }
        }
    }

    void stopNote(float /*velocity*/, bool allowTailOff) override
    {
        // While freeze is active ignore note-offs so the loop keeps running.
        // freezeActive is cleared on the message thread BEFORE allNotesOff is called,
        // so the voice will stop correctly when freeze is turned off.
        auto* sound = dynamic_cast<LoopingSamplerSound*>(getCurrentlyPlayingSound().get());
        if (sound != nullptr && (sound->freezeActive.load() || sound->oneShotEnabled.load()))
            return;

        noteIsHeld = false;   // note is genuinely being released — loop wraps must not restart envelope

        if (!allowTailOff)
        {
            inRelease       = false;
            releasePosition = -1;
            clearCurrentNote();
            adsr.reset();
            return;
        }

        // Custom ADSR path: hand off to manual release counter — do not touch JUCE ADSR.
        if (sound != nullptr && sound->customAdsrEnabled.load())
        {
            inRelease       = true;
            releasePosition = 0;
            return;   // renderNextBlock will call clearCurrentNote() when counter expires
        }

        adsr.noteOff();   // default click-elimination path
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    // Called from message thread during stop-before-load to unconditionally release
    // the currentlyPlayingSound ref-counted pointer.  Must be called with muteOutput=true
    // so the audio thread is not inside renderNextBlock concurrently.
    void forceStop()
    {
        noteIsHeld        = false;
        inRelease         = false;
        releasePosition   = -1;
        envSamplePosition = 0;
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

            // Cache once per block — avoids repeated atomic loads inside the sample loop.
            const bool customAdsr = sound->customAdsrEnabled.load();

            // Re-read custom ADSR knob values once per block so changes take effect within ~6 ms.
            if (customAdsr)
            {
                const double sr = getSampleRate();
                attackSamples  = juce::jmax(0, (int)(sound->customAdsrAttackMs.load()  / 1000.0f * sr));
                decaySamples   = juce::jmax(0, (int)(sound->customAdsrDecayMs.load()   / 1000.0f * sr));
                releaseSamples = juce::jmax(1, (int)(sound->customAdsrReleaseMs.load() / 1000.0f * sr));
                sustainLevel   = juce::jlimit(0.0f, 1.0f, sound->customAdsrSustain.load());
            }

            // Clamp position into [sStart, sEnd) in case the atomics just changed.
            if (sourceSamplePosition < (double)sStart)
                sourceSamplePosition = (double)sStart;

            if (sourceSamplePosition >= (double)sEnd)
            {
                if (shouldLoop)
                {
                    sourceSamplePosition = (double)sStart
                        + std::fmod(sourceSamplePosition - (double)sStart, regionLen);
                    // Reset manual envelope counter so the new loop cycle starts from attack.
                    // Only when note is still held — if released, inRelease is already true
                    // and we must not disturb the release counter.
                    if (customAdsr && noteIsHeld)
                        envSamplePosition = 0;
                }
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

                // Envelope gain — manual calculation for custom ADSR, JUCE ADSR for click-elimination.
                float env;
                if (customAdsr)
                {
                    if (inRelease)
                    {
                        // Linear release from sustainLevel → 0 over releaseSamples.
                        env = (releaseSamples > 0)
                            ? sustainLevel * juce::jmax(0.0f, 1.0f - (float)releasePosition / (float)releaseSamples)
                            : 0.0f;
                        ++releasePosition;
                        if (releasePosition >= releaseSamples)
                        {
                            clearCurrentNote();
                            break;
                        }
                    }
                    else
                    {
                        // Attack: linear ramp 0 → 1 over attackSamples.
                        if (attackSamples > 0 && envSamplePosition < attackSamples)
                        {
                            env = (float)envSamplePosition / (float)attackSamples;
                        }
                        // Decay: linear ramp 1 → sustainLevel over decaySamples.
                        else if (decaySamples > 0 && envSamplePosition < attackSamples + decaySamples)
                        {
                            const float dp = (float)(envSamplePosition - attackSamples) / (float)decaySamples;
                            env = 1.0f - dp * (1.0f - sustainLevel);
                        }
                        // Sustain: hold at sustainLevel.
                        else
                        {
                            env = sustainLevel;
                        }
                        ++envSamplePosition;
                    }

                    // Debug: print envelope state ~once per second.
                    if (++debugEnvCounter >= 44100)
                    {
                        debugEnvCounter = 0;
                        DBG("[ADSR-MANUAL] envPos=" + juce::String(envSamplePosition)
                            + " gain=" + juce::String(env, 4)
                            + " inRelease=" + juce::String((int)inRelease)
                            + " relPos=" + juce::String(releasePosition));
                    }
                }
                else
                {
                    // Default click-elimination ADSR — runs once, never restarted at loop wraps.
                    env = adsr.getNextSample();
                    if (!adsr.isActive())
                    {
                        clearCurrentNote();
                        break;
                    }
                }

                l *= lgain * env;
                r *= rgain * env;

                if (outR != nullptr) { *outL++ += l; *outR++ += r; }
                else                 { *outL++ += (l + r) * 0.5f; }

                sourceSamplePosition += pitchRatio;

                if (sourceSamplePosition >= (double)sEnd)
                {
                    if (shouldLoop)
                    {
                        sourceSamplePosition = (double)sStart
                            + std::fmod(sourceSamplePosition - (double)sStart, regionLen);
                        // Reset manual envelope counter so the new cycle starts from attack.
                        // Guard: if released (noteIsHeld=false), inRelease is already running —
                        // do not touch envSamplePosition.
                        if (customAdsr && noteIsHeld)
                            envSamplePosition = 0;
                    }
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
    // True from startNote until a genuine note-off (not frozen/oneshot early-return).
    // Guards the loop-wrap envSamplePosition reset so a released note's release tail is never canceled.
    bool noteIsHeld = false;

    // Manual per-cycle ADSR envelope (used when customAdsrEnabled=true).
    // Replaces adsr.noteOn() re-triggers at loop wraps — JUCE ADSR does not reliably restart
    // the attack phase from sustain state; it blends from current level instead of resetting to 0.
    int   envSamplePosition = 0;   // samples elapsed since start of current loop cycle
    int   attackSamples     = 0;   // precomputed: attackMs * sampleRate / 1000
    int   decaySamples      = 0;
    int   releaseSamples    = 1;
    float sustainLevel      = 1.0f;
    bool  inRelease         = false;
    int   releasePosition   = -1;  // -1 = not in release; >=0 = samples elapsed in release phase
    int   debugEnvCounter   = 0;   // throttles debug prints to ~1 per second
};
