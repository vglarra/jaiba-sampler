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
    // Reverse playback: when true the voice reads samples from End→Start instead of Start→End.
    std::atomic<bool>        reverseEnabled      { false };
    // Bounce (ping-pong) playback: forward then backward, alternating at start/end boundaries.
    // Mutually exclusive with reverseEnabled at the UI level.
    std::atomic<bool>        bounceEnabled       { false };
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
            smoothedPitchRatio = pitchRatio;   // align immediately at note start — no ramp at attack

            // Start playback at the correct boundary for current direction.
            // Reverse: start at End−1 and count down toward Start.
            // Forward: start at Start and count up toward End.
            if (sound->reverseEnabled.load())
                sourceSamplePosition = (double)(sound->endSampleAtomic.load() - 1);
            else
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
            playDirectionInternal = 1;   // always start forward in bounce mode
            playDirectionAtomic.store(1);

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

            // Fix 2: Apply fade-in for reverse and bounce modes to prevent click when
            // the start position (End marker for reverse, Start marker for bounce) has non-zero amplitude.
            // Forward mode is handled by the JUCE/custom ADSR attack ramp.
            if (sound->reverseEnabled.load() || sound->bounceEnabled.load())
                triggerFadeInOnly();
            else
                resetXfade();
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

        // Cancel any in-progress pre-wrap fade-out — release envelope handles the fade from here.
        inPreWrapFadeOut = false;

        noteIsHeld = false;   // note is genuinely being released — loop wraps must not restart envelope

        if (!allowTailOff)
        {
            inRelease       = false;
            releasePosition = -1;
            playheadPositionAtomic.store(-1);
            clearCurrentNote();
            adsr.reset();
            resetXfade();
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
        noteIsHeld            = false;
        inRelease             = false;
        releasePosition       = -1;
        envSamplePosition     = 0;
        playDirectionInternal = 1;
        playheadPositionAtomic.store(-1);  // hide playhead immediately
        playDirectionAtomic.store(1);      // reset direction indicator
        pendingSeekAtomic.store(-1);       // cancel any pending direction-switch seek
        resetXfade();
        clearCurrentNote();   // sets currentlyPlayingSound = nullptr (releases ref-count)
        adsr.reset();
        sourceSamplePosition = 0.0;
        smoothedPitchRatio   = 0.0;
    }

    // Written by audio thread once per block; read by UI timer (60 fps) for playhead display.
    // Both are public so MainComponent can read them directly — no lock, no sound pointer access.
    std::atomic<juce::int64> playheadPositionAtomic { -1 };  // -1 = not playing
    std::atomic<juce::int64> totalSamplesAtomic     {  0 };  // total sample count for normalization
    // Current bounce direction: +1 = forward, -1 = backward.
    // Written by audio thread when direction flips; read by UI timer for direction arrow.
    std::atomic<int>         playDirectionAtomic    {  1 };
    // Written by message thread when reverse is toggled during active playback.
    // Audio thread applies the seek at start of next block and resets to -1.
    // Enables the "mirror current position" behavior required for seamless direction switches.
    std::atomic<juce::int64> pendingSeekAtomic      { -1 };  // -1 = no pending seek

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
            // 10ms exponential pitch-ramp coefficient — precomputed once per block (not per sample).
            // At 44100 Hz: coeff ≈ 0.00226; smoothedPitchRatio reaches 63% of target in ~10ms.
            // Eliminates audible click when pitch changes during active playback.
            const double pitchSmoothCoeff = 1.0 - std::exp(-1.0 / (getSampleRate() * 0.010));
            if (sStart >= sEnd) return;

            const double regionLen = (double)(sEnd - sStart);

            // Cache mode flags once per block.
            const bool reversePlayback = sound->reverseEnabled.load();
            const bool bounceMode      = sound->bounceEnabled.load();
            const bool freezeActive    = sound->freezeActive.load();

            // maxSafePos: highest pos where inL[pos+1] is still within the buffer.
            const int maxSafePos = data->getNumSamples() - 2;
            if (maxSafePos < 0) return;

            const float* inL = data->getReadPointer(0);
            const float* inR = data->getNumChannels() > 1 ? data->getReadPointer(1) : nullptr;

            float* outL = output.getWritePointer(0, startSample);
            float* outR = output.getNumChannels() > 1
                              ? output.getWritePointer(1, startSample) : nullptr;

            // OneShot overrides loop: even if loopEnabled is true, we play through once only.
            const bool shouldLoop = (sound->loopEnabled.load() || freezeActive)
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

            // Fix 4 — Zero crossing search helper (inline lambda, uses inL buffer).
            // Finds the nearest sample index where |amplitude| < 0.001, within [sStart, sEnd-1]
            // and at most 'radius' samples from 'center'. Returns 'center' if none found.
            auto findZeroCrossing = [&](juce::int64 center, juce::int64 radius) -> juce::int64
            {
                const juce::int64 bufLen = (juce::int64)data->getNumSamples();
                const juce::int64 lo     = juce::jmax(sStart, center - radius);
                const juce::int64 hi     = juce::jmin(sEnd - 1, center + radius);
                for (juce::int64 i = 0; i <= radius; ++i)
                {
                    if (center + i <= hi && std::abs(inL[center + i]) < 0.001f) return center + i;
                    if (center - i >= lo && std::abs(inL[center - i]) < 0.001f) return center - i;
                }
                return juce::jlimit(sStart, sEnd - 1, center);  // fallback: clamped original
            };

            // Apply any pending position seek (written by message thread on direction toggle).
            // Mirrors the current position around the midpoint so playback continues smoothly.
            {
                const juce::int64 seek = pendingSeekAtomic.load();
                if (seek >= 0)
                {
                    sourceSamplePosition = (double)seek;
                    pendingSeekAtomic.store(-1);
                }
            }

            // Clamp position to valid range for current direction.
            // Direction flips here trigger a crossfade without zero-crossing (recovery path).
            if (bounceMode)
            {
                // Bounce: position must stay within [sStart, sEnd-1]; flip direction at boundaries.
                if (playDirectionInternal == 1)
                {
                    if (sourceSamplePosition < (double)sStart)
                        sourceSamplePosition = (double)sStart;
                    if (sourceSamplePosition >= (double)sEnd)
                    {
                        if (noteIsHeld || freezeActive)
                        {
                            sourceSamplePosition = (double)(sEnd - 1);
                            playDirectionInternal = -1;
                            playDirectionAtomic.store(-1);
                            triggerBounceXfade();  // Fix 1: crossfade at direction flip
                            if (customAdsr && noteIsHeld) envSamplePosition = 0;
                        }
                        else { playheadPositionAtomic.store(-1); clearCurrentNote(); adsr.reset(); return; }
                    }
                }
                else
                {
                    if (sourceSamplePosition >= (double)sEnd)
                        sourceSamplePosition = (double)(sEnd - 1);
                    if (sourceSamplePosition < (double)sStart)
                    {
                        if (noteIsHeld || freezeActive)
                        {
                            sourceSamplePosition = (double)sStart;
                            playDirectionInternal = 1;
                            playDirectionAtomic.store(1);
                            triggerBounceXfade();  // Fix 1: crossfade at direction flip
                            if (customAdsr && noteIsHeld) envSamplePosition = 0;
                        }
                        else { playheadPositionAtomic.store(-1); clearCurrentNote(); adsr.reset(); return; }
                    }
                }
            }
            else if (reversePlayback)
            {
                // Reverse: position counts down from (sEnd-1) toward sStart.
                if (sourceSamplePosition >= (double)sEnd)
                    sourceSamplePosition = (double)(sEnd - 1);

                if (sourceSamplePosition < (double)sStart)
                {
                    if (shouldLoop)
                    {
                        const double overshoot = (double)sStart - sourceSamplePosition;
                        sourceSamplePosition = (double)(sEnd - 1) - std::fmod(overshoot, regionLen);
                        if (customAdsr && noteIsHeld)
                            envSamplePosition = 0;
                    }
                    else
                    {
                        playheadPositionAtomic.store(-1);
                        clearCurrentNote();
                        adsr.reset();
                        return;
                    }
                }
            }
            else
            {
                // Forward: position counts up from sStart toward sEnd.
                if (sourceSamplePosition < (double)sStart)
                    sourceSamplePosition = (double)sStart;

                if (sourceSamplePosition >= (double)sEnd)
                {
                    if (shouldLoop)
                    {
                        sourceSamplePosition = (double)sStart
                            + std::fmod(sourceSamplePosition - (double)sStart, regionLen);
                        if (customAdsr && noteIsHeld)
                            envSamplePosition = 0;
                    }
                    else
                    {
                        playheadPositionAtomic.store(-1);
                        clearCurrentNote();
                        adsr.reset();
                        return;
                    }
                }
            }

            // Update playhead display once per block (not per sample — negligible cost).
            totalSamplesAtomic.store(data->getNumSamples());
            playheadPositionAtomic.store((juce::int64)sourceSamplePosition);

            while (--numSamples >= 0)
            {
                // ── Fix 3: Pre-end fade-out detection for loop wrap ─────────────────────────
                // Detect when we're approaching the loop boundary (within kXfadeSamples/2 source
                // samples). Start the fade-out so we reach silence exactly at the boundary,
                // then fade back in after the wrap. Guard: only when not already fading.
                if (shouldLoop && !bounceMode && xfadeCounter < 0 && !inRelease)
                {
                    double distToBoundary;
                    if (reversePlayback)
                        distToBoundary = sourceSamplePosition - (double)sStart;
                    else
                        distToBoundary = (double)(sEnd - 1) - sourceSamplePosition;

                    if (distToBoundary >= 0.0 && distToBoundary < (double)(kXfadeSamples / 2))
                    {
                        // Start fade-out partway through so we reach 0 at the boundary.
                        const int elapsed = (kXfadeSamples / 2) - (int)distToBoundary;
                        xfadeCounter     = juce::jlimit(0, kXfadeSamples / 2 - 1, elapsed);
                        const float half = (float)(kXfadeSamples / 2);
                        xfadeGain        = 1.0f - (float)xfadeCounter / half;
                        inPreWrapFadeOut = true;
                    }
                }

                // ── Fix 2: Reverse stop fade-out ────────────────────────────────────────────
                // When reverse (non-loop) approaches sStart, start a fade-out so the voice
                // stops cleanly at near-zero amplitude rather than an abrupt cut.
                if (reversePlayback && !shouldLoop && xfadeCounter < 0 && !inRelease)
                {
                    const double distToStart = sourceSamplePosition - (double)sStart;
                    if (distToStart >= 0.0 && distToStart < (double)(kXfadeSamples / 2))
                    {
                        const int elapsed = (kXfadeSamples / 2) - (int)distToStart;
                        xfadeCounter     = juce::jlimit(0, kXfadeSamples / 2 - 1, elapsed);
                        const float half = (float)(kXfadeSamples / 2);
                        xfadeGain        = 1.0f - (float)xfadeCounter / half;
                        // Don't set inPreWrapFadeOut — let the counter advance normally to silence.
                    }
                }

                // ── Crossfade gain computation ───────────────────────────────────────────────
                // Phase 0..63  (fade-out): gain  1.0 → 0.0
                // Phase 64..127 (fade-in): gain  0.0 → 1.0
                // inPreWrapFadeOut: hold at phase 63 (gain ≈ 0) until the wrap fires, then
                // the wrap handler jumps us to phase 64 to begin the fade-in.
                if (xfadeCounter >= 0)
                {
                    const float half = (float)(kXfadeSamples / 2);
                    if (inPreWrapFadeOut && xfadeCounter >= kXfadeSamples / 2)
                    {
                        // Hold at silence — waiting for the wrap boundary to be crossed.
                        xfadeGain = 0.0f;
                        // Counter deliberately NOT incremented here.
                    }
                    else
                    {
                        xfadeGain = (xfadeCounter < kXfadeSamples / 2)
                            ? (1.0f - (float)xfadeCounter / half)                   // fade-out 1→0
                            : ((float)(xfadeCounter - kXfadeSamples / 2) / half);   // fade-in  0→1
                        if (++xfadeCounter >= kXfadeSamples)
                        {
                            xfadeCounter     = -1;
                            xfadeGain        = 1.0f;
                            inPreWrapFadeOut = false;
                        }
                    }
                }

                // Safety clamp for interpolation — valid for both forward and reverse.
                // For reverse, position decreases so we need the lower bound (0) as well.
                int pos = juce::jlimit(0, maxSafePos, (int)sourceSamplePosition);
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
                            playheadPositionAtomic.store(-1);
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
                        playheadPositionAtomic.store(-1);
                        clearCurrentNote();
                        break;
                    }
                }

                // Fix 5: Signal chain order — rawSample → ADSR (env) → bounceXfadeGain → output.
                // EQ filters (in MainComponent) apply after this block, so they see the faded signal.
                // The fade-to-zero during the crossfade naturally drains filter memory (Fix 6 implicit).
                l *= lgain * env * xfadeGain;
                r *= rgain * env * xfadeGain;

                if (outR != nullptr) { *outL++ += l; *outR++ += r; }
                else                 { *outL++ += (l + r) * 0.5f; }

                // Smooth pitch toward target — eliminates audible click on mid-playback pitch change.
                smoothedPitchRatio += (pitchRatio - smoothedPitchRatio) * pitchSmoothCoeff;

                // Advance position and handle boundary (mode-specific).
                if (bounceMode)
                {
                    // Bounce: advance in current internal direction, then flip at each boundary.
                    if (playDirectionInternal == 1)
                        sourceSamplePosition += smoothedPitchRatio;
                    else
                        sourceSamplePosition -= smoothedPitchRatio;

                    // Continue bouncing while the note is held OR freeze is active.
                    if (playDirectionInternal == 1 && sourceSamplePosition >= (double)sEnd)
                    {
                        if (noteIsHeld || freezeActive)
                        {
                            // Fix 4: Find zero crossing near End marker for cleanest transition.
                            const juce::int64 zcEnd = findZeroCrossing(sEnd - 1, 512);
                            if (zcEnd != sEnd - 1)
                                printf("[ZERO-CROSS] Found zero crossing at offset %lld samples from end marker\n",
                                       (long long)(zcEnd - (sEnd - 1)));

                            sourceSamplePosition = (double)zcEnd;
                            playDirectionInternal = -1;
                            playDirectionAtomic.store(-1);
                            // Fix 1: Trigger crossfade centered on the zero crossing position.
                            triggerBounceXfade();
                            printf("[BOUNCE-XFADE] Direction fwd->rev at sample %lld — applying %d sample crossfade\n",
                                   (long long)zcEnd, kXfadeSamples);
                            if (customAdsr && noteIsHeld) envSamplePosition = 0;
                        }
                        else
                        {
                            playheadPositionAtomic.store(-1);
                            clearCurrentNote();
                            adsr.reset();
                            break;
                        }
                    }
                    else if (playDirectionInternal == -1 && sourceSamplePosition < (double)sStart)
                    {
                        if (noteIsHeld || freezeActive)
                        {
                            // Fix 4: Find zero crossing near Start marker for cleanest transition.
                            const juce::int64 zcStart = findZeroCrossing(sStart, 512);
                            if (zcStart != sStart)
                                printf("[ZERO-CROSS] Found zero crossing at offset %lld samples from start marker\n",
                                       (long long)(zcStart - sStart));

                            sourceSamplePosition = (double)zcStart;
                            playDirectionInternal = 1;
                            playDirectionAtomic.store(1);
                            // Fix 1: Trigger crossfade centered on the zero crossing position.
                            triggerBounceXfade();
                            printf("[BOUNCE-XFADE] Direction rev->fwd at sample %lld — applying %d sample crossfade\n",
                                   (long long)zcStart, kXfadeSamples);
                            if (customAdsr && noteIsHeld) envSamplePosition = 0;
                        }
                        else
                        {
                            playheadPositionAtomic.store(-1);
                            clearCurrentNote();
                            adsr.reset();
                            break;
                        }
                    }
                }
                else if (reversePlayback)
                {
                    sourceSamplePosition -= smoothedPitchRatio;

                    if (sourceSamplePosition < (double)sStart)
                    {
                        if (shouldLoop)
                        {
                            // Fix 4: Find zero crossing near End marker (start of new reverse cycle).
                            const juce::int64 zcEnd = findZeroCrossing(sEnd - 1, 512);
                            const double overshoot = (double)sStart - sourceSamplePosition;
                            sourceSamplePosition = (double)zcEnd - std::fmod(overshoot, regionLen);
                            if (sourceSamplePosition < (double)sStart) sourceSamplePosition = (double)zcEnd;
                            // Fix 3: Fade-in at start of new reverse loop cycle.
                            // Switch from pre-wrap fade-out (if active) to fade-in.
                            xfadeCounter     = kXfadeSamples / 2;
                            xfadeGain        = 0.0f;
                            inPreWrapFadeOut = false;
                            if (customAdsr && noteIsHeld)
                                envSamplePosition = 0;
                        }
                        else
                        {
                            playheadPositionAtomic.store(-1);
                            clearCurrentNote();
                            adsr.reset();
                            resetXfade();
                            break;
                        }
                    }
                }
                else
                {
                    sourceSamplePosition += smoothedPitchRatio;

                    if (sourceSamplePosition >= (double)sEnd)
                    {
                        if (shouldLoop)
                        {
                            // Fix 4: Find zero crossing near Start marker (start of new loop cycle).
                            const juce::int64 zcStart = findZeroCrossing(sStart, 512);
                            sourceSamplePosition = (double)zcStart
                                + std::fmod(sourceSamplePosition - (double)sStart, regionLen);
                            // Fix 3: Switch from pre-wrap fade-out to fade-in at the wrap boundary.
                            // If inPreWrapFadeOut is true, the counter was held at kXfadeSamples/2-1.
                            // Setting it to kXfadeSamples/2 here begins the fade-in phase.
                            xfadeCounter     = kXfadeSamples / 2;
                            xfadeGain        = 0.0f;
                            inPreWrapFadeOut = false;
                            // Reset manual envelope counter so the new cycle starts from attack.
                            // Guard: if released (noteIsHeld=false), inRelease is already running —
                            // do not touch envSamplePosition.
                            if (customAdsr && noteIsHeld)
                                envSamplePosition = 0;
                        }
                        else
                        {
                            // Use direct stop to bypass the oneshot/freeze early-return guard in stopNote.
                            playheadPositionAtomic.store(-1);
                            clearCurrentNote();
                            adsr.reset();
                            resetXfade();
                            break;
                        }
                    }
                }
            }
        }
    }

private:
    double basePitchRatio = 0.0;       // MIDI transpose only — set in startNote, never changes
    double pitchRatio = 0.0;           // basePitchRatio * pow(2, offset/12) — updated per block (target)
    double smoothedPitchRatio = 0.0;   // exponential IIR toward pitchRatio — 10ms ramp, no click on pitch change
    double sourceSamplePosition = 0.0;
    float  lgain = 0.0f, rgain = 0.0f;
    juce::ADSR adsr;
    // True from startNote until a genuine note-off (not frozen/oneshot early-return).
    // Guards the loop-wrap envSamplePosition reset so a released note's release tail is never canceled.
    bool noteIsHeld = false;
    // Current bounce direction: +1 = forward, -1 = backward.
    // Changes at each boundary during bounce playback.
    int playDirectionInternal = 1;

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

    // ── Micro crossfade for click-free bounce / loop-wrap / reverse transitions ──
    // Fix 1 (bounce), Fix 2 (reverse start/stop), Fix 3 (loop wrap).
    //
    // Counter semantics:
    //   -1              = inactive; xfadeGain stays at 1.0 (no effect on output)
    //    0 .. half-1    = fade-OUT phase: gain decreases linearly from 1.0 → 0.0
    //    half .. total-1 = fade-IN phase: gain increases linearly from 0.0 → 1.0
    //
    // The xfadeGain multiplies the ADSR gain BEFORE the signal reaches the EQ filters,
    // which means the filters see a smooth ramp-to-zero at every transition — naturally
    // draining their memory and preventing filter-induced clicks (Fix 6, implicit).
    static constexpr int kXfadeSamples = 128;  // ~2.9ms @ 44.1kHz — inaudible as fade, eliminates all clicks
    int   xfadeCounter     = -1;      // see above
    float xfadeGain        = 1.0f;    // computed each sample from xfadeCounter
    // When true, the fade-out phase is "held" at counter = kXfadeSamples/2 - 1 (gain ≈ 0)
    // until the loop-wrap boundary is actually crossed, then the wrap handler kicks it to
    // the fade-in phase.  Prevents the fade-in from starting prematurely before the wrap.
    bool  inPreWrapFadeOut = false;

    // Trigger a full crossfade (fade-out THEN fade-in) centred on the transition point.
    // Used at bounce direction flips — xfadeGain dips to 0 at the mid-point then recovers.
    void triggerBounceXfade()
    {
        xfadeCounter     = 0;
        xfadeGain        = 1.0f;
        inPreWrapFadeOut = false;
    }

    // Trigger a fade-in only (start from counter = half, gain = 0 → 1).
    // Used at note start for reverse/bounce modes and at loop wrap after the fade-out completes.
    void triggerFadeInOnly()
    {
        xfadeCounter     = kXfadeSamples / 2;
        xfadeGain        = 0.0f;
        inPreWrapFadeOut = false;
    }

    // Reset crossfade state to inactive.  Called from forceStop() and when a stop path fires
    // outside the normal xfade completion (e.g. non-loop voice stopping cleanly).
    void resetXfade()
    {
        xfadeCounter     = -1;
        xfadeGain        = 1.0f;
        inPreWrapFadeOut = false;
    }
};
