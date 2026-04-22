#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <atomic>

#include "LoopingSampler.h"
#include "WaveformPeakBin.h"

//==============================================================================
// PadAudioEngine — self-contained audio engine for one sample pad.
//
// Encapsulates everything that was previously scattered through MainComponent:
//   • juce::Synthesiser  (16 LoopingSamplerVoice instances)
//   • 3-band parametric EQ  (lock-free double-buffered coefficients)
//   • FFT spectrum analyzer  (background worker thread, 30fps)
//   • Sample audio data (MappedSample array)
//   • All atomic param stores (pitch, volume, loop, ADSR …)
//
// Design rules:
//   • No UI references (no Component, no Label, no Slider).
//   • No persistence references (no ConfigurationManager).
//   • updateSamplerSounds() stays in MainComponent because it needs SampleCard
//     state; PadAudioEngine provides primitives (clearSoundsAndVoices,
//     getSynthesiser, etc.) for MainComponent to orchestrate.
//   • renderNextBlock() accepts startSample + numSamples so PadManager can mix
//     multiple pads into one output buffer.
//   • Master volume is NOT applied here — PadManager applies it after mixing.
//   • Most members are public to allow MainComponent to orchestrate complex
//     sequences (freeze, sample-change, panic) without awkward callbacks.
//==============================================================================

class PadAudioEngine
{
public:
    //==========================================================================
    // Inner structs declared first so they can be used in member declarations.

    //--------------------------------------------------------------------------
    // Lock-free double buffer for EQ coefficients.
    // UI thread writes to back buffer; audio thread swaps in nanoseconds.
    struct EqCoeffDoubleBuffer
    {
        struct Coeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

        Coeffs buf[2][3] {};
        std::atomic<bool> updated { false };
        int front = 0;   // audio-thread owned

        void writeFromUI(const Coeffs newCoeffs[3]) noexcept
        {
            const int back = 1 - front;
            for (int i = 0; i < 3; ++i) buf[back][i] = newCoeffs[i];
            updated.store(true, std::memory_order_release);
        }

        const Coeffs* swapIfUpdated() noexcept
        {
            if (updated.load(std::memory_order_acquire))
            {
                front = 1 - front;
                updated.store(false, std::memory_order_relaxed);
            }
            return buf[front];
        }

        EqCoeffDoubleBuffer() = default;
        EqCoeffDoubleBuffer(const EqCoeffDoubleBuffer&) = delete;
        EqCoeffDoubleBuffer& operator=(const EqCoeffDoubleBuffer&) = delete;
    };

    //--------------------------------------------------------------------------
    // Per-pad audio data (was MainComponent::MappedSample).
    struct MappedSample
    {
        juce::File   file;
        juce::String name;
        int rootNote = 60;
        int lowNote  = 48;
        int highNote = 60;
        double attack  = 0.1;
        double release = 0.1;
        bool isSelected = false;
        int  pitchOffset = 0;
        double startPointSeconds = 0.0;
        double endPointSeconds   = -1.0;

        std::shared_ptr<juce::AudioBuffer<float>> audioData;
        double       sampleRate      = 0;
        int          numChannels     = 0;
        juce::int64  lengthInSamples = 0;

        bool isValid() const { return audioData != nullptr && audioData->getNumSamples() > 0; }
        ~MappedSample() = default;
    };

    //==========================================================================
    // Construction / destruction

    explicit PadAudioEngine(juce::AudioFormatManager& fmt)
        : formatManager(fmt)
    {
        for (int i = 0; i < 16; ++i)
            sampler.addVoice(new LoopingSamplerVoice());
        sampler.setNoteStealingEnabled(true);
    }

    ~PadAudioEngine()
    {
        if (fftThread != nullptr)
        {
            fftThread->stopThread(500);
            fftThread.reset();
        }
    }

    //==========================================================================
    // AudioAppComponent delegation

    void prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate)
    {
        sampler.setCurrentPlaybackSampleRate(sampleRate);

        if (fftThread != nullptr)
        {
            fftThread->stopThread(200);
            fftThread.reset();
        }

        fft = std::make_unique<juce::dsp::FFT>(kFFTOrder);

        for (int i = 0; i < kFFTSize; ++i)
            fftWindow[i] = 0.5f * (1.0f - std::cos(
                juce::MathConstants<float>::twoPi * i / (kFFTSize - 1)));

        resetEqState();
        fftAbstractFifo.reset();
        std::fill(std::begin(fftCircularBuffer), std::end(fftCircularBuffer), 0.0f);

        // Pre-compute flat EQ coefficients (used by Reset button — no math at click time)
        defaultFlatCoeffs[0] = computeEqCoeffs(100.0f,  0.0f, 1.0f, 2, sampleRate);
        defaultFlatCoeffs[1] = computeEqCoeffs(500.0f,  0.0f, 1.0f, 2, sampleRate);
        defaultFlatCoeffs[2] = computeEqCoeffs(8000.0f, 0.0f, 1.0f, 2, sampleRate);

        fftThread = std::make_unique<FftWorkerThread>(*this);
        fftThread->startThread();
    }

    void releaseResources() {}

    // Render this pad's audio into [startSample, startSample+numSamples) of outputBuffer.
    // Applies: sampler → normGain → volumeGain → EQ → push to FFT FIFO → accumulate into outputBuffer.
    //
    // KEY ISOLATION: the sampler renders into a PRIVATE scratch buffer (ownBuffer), not directly
    // into outputBuffer. EQ and FFT operate on ownBuffer — so the FFT spectrum shows ONLY this
    // pad's audio, not the accumulated mix of all engines rendered before it. Only after EQ and
    // FFT capture is ownBuffer added into outputBuffer.
    //
    // Master volume is intentionally NOT applied — PadManager does that after mixing.
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         const juce::MidiBuffer& midiMessages,
                         int startSample, int numSamples)
    {
        const int nCh = outputBuffer.getNumChannels();

        // ── Grow private scratch buffer if needed (no allocation in the common case) ──
        if (ownBuffer.getNumChannels() < nCh || ownBuffer.getNumSamples() < numSamples)
            ownBuffer.setSize(nCh, numSamples, false, true, true);

        // Render sampler into our private buffer (cleared to silence first by JUCE Synthesiser)
        {
            // Synthesiser expects a buffer large enough from sample 0.
            // We use a sub-buffer view trick: set the ownBuffer to zeros then render.
            ownBuffer.clear(0, numSamples);
            juce::AudioBuffer<float> subBuf(ownBuffer.getArrayOfWritePointers(),
                                            nCh, 0, numSamples);
            // Use startSample=0 in subBuf — MIDI events reference absolute sample within block,
            // so we need to pass the same midiMessages but shifted. Use an offset MidiBuffer view.
            juce::MidiBuffer shiftedMidi;
            for (const auto& meta : midiMessages)
            {
                const int pos = meta.samplePosition - startSample;
                if (pos >= 0 && pos < numSamples)
                    shiftedMidi.addEvent(meta.getMessage(), pos);
            }
            sampler.renderNextBlock(subBuf, shiftedMidi, 0, numSamples);
        }

        // Normalization gain (before per-pad volume)
        {
            float ng = normGain.load();
            if (ng != 1.0f)
                ownBuffer.applyGain(0, numSamples, ng);
        }

        // Per-pad volume gain
        {
            float g = volumeGain.load();
            if (g != 1.0f)
                ownBuffer.applyGain(0, numSamples, g);
        }

        // ── Parametric EQ — single-pass cascade, lock-free double-buffer coeffs ──
        if (eqActive.load())
        {
            // Measure EQ Reset round-trip (no printf in audio thread)
            const juce::int64 resetReqMs = eqResetRequestedMs.load(std::memory_order_relaxed);
            if (resetReqMs > 0)
            {
                const juce::int64 elapsed = juce::Time::getMillisecondCounter() - resetReqMs;
                eqResetElapsedMs.store(elapsed, std::memory_order_relaxed);
                eqResetRequestedMs.store(0, std::memory_order_relaxed);
            }

            const auto* c = eqCoeffDB.swapIfUpdated();

            for (int ch = 0; ch < juce::jmin(nCh, 2); ++ch)
            {
                float* data = ownBuffer.getWritePointer(ch, 0);
                double z1_0 = eqZ1[0][ch], z2_0 = eqZ2[0][ch];
                double z1_1 = eqZ1[1][ch], z2_1 = eqZ2[1][ch];
                double z1_2 = eqZ1[2][ch], z2_2 = eqZ2[2][ch];

                for (int i = 0; i < numSamples; ++i)
                {
                    double x  = static_cast<double>(data[i]);
                    double y0 = c[0].b0 * x  + z1_0;
                    z1_0 = c[0].b1 * x  - c[0].a1 * y0 + z2_0;
                    z2_0 = c[0].b2 * x  - c[0].a2 * y0;
                    double y1 = c[1].b0 * y0 + z1_1;
                    z1_1 = c[1].b1 * y0 - c[1].a1 * y1 + z2_1;
                    z2_1 = c[1].b2 * y0 - c[1].a2 * y1;
                    double y2 = c[2].b0 * y1 + z1_2;
                    z1_2 = c[2].b1 * y1 - c[2].a1 * y2 + z2_2;
                    z2_2 = c[2].b2 * y1 - c[2].a2 * y2;
                    data[i] = static_cast<float>(y2);
                }

                eqZ1[0][ch] = z1_0; eqZ2[0][ch] = z2_0;
                eqZ1[1][ch] = z1_1; eqZ2[1][ch] = z2_1;
                eqZ1[2][ch] = z1_2; eqZ2[2][ch] = z2_2;
            }
        }

        // ── Push THIS PAD'S audio to FFT (from ownBuffer, not the accumulated mix) ──
        {
            const int toWrite = juce::jmin(numSamples, fftAbstractFifo.getFreeSpace());

            if (toWrite > 0)
            {
                int start1, size1, start2, size2;
                fftAbstractFifo.prepareToWrite(toWrite, start1, size1, start2, size2);

                for (int i = 0; i < size1; ++i)
                {
                    float mono = 0.0f;
                    for (int ch = 0; ch < juce::jmin(nCh, 2); ++ch)
                        mono += ownBuffer.getReadPointer(ch, 0)[i];
                    if (nCh > 0) mono /= static_cast<float>(nCh);
                    fftCircularBuffer[start1 + i] = mono;
                }
                for (int i = 0; i < size2; ++i)
                {
                    float mono = 0.0f;
                    for (int ch = 0; ch < juce::jmin(nCh, 2); ++ch)
                        mono += ownBuffer.getReadPointer(ch, 0)[size1 + i];
                    if (nCh > 0) mono /= static_cast<float>(nCh);
                    fftCircularBuffer[start2 + i] = mono;
                }
                fftAbstractFifo.finishedWrite(size1 + size2);
            }
        }

        // ── Accumulate this pad's processed audio into the shared output buffer ──
        for (int ch = 0; ch < juce::jmin(nCh, ownBuffer.getNumChannels()); ++ch)
            outputBuffer.addFrom(ch, startSample, ownBuffer, ch, 0, numSamples);
    }

    //==========================================================================
    // Voice / sound management

    void forceStopAllVoices()
    {
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
                v->forceStop();
    }

    void clearSoundsAndVoices() { sampler.clearSounds(); }

    void allNotesOff(int channel = 0, bool allowTailOff = false)
    {
        sampler.allNotesOff(channel, allowTailOff);
    }

    // Clear freeze/loop/oneshot on all live sounds (called before forceStopAllVoices).
    void clearActiveSoundFlags()
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                snd->freezeActive.store(false);
                snd->loopEnabled.store(false);
                snd->oneShotEnabled.store(false);
            }
    }

    void injectNoteOn(int channel, int noteNumber, float velocity)
    {
        sampler.noteOn(channel, noteNumber, velocity);
    }

    void stopAllVoiceNotes(bool allowTailOff)
    {
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
                v->stopNote(0.0f, allowTailOff);
    }

    bool isAnyVoiceActive() const
    {
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
                if (v->isVoiceActive()) return true;
        return false;
    }

    //==========================================================================
    // Convenience setters — atomic store + iterate live sounds.
    // These are called from MainComponent's SampleCard::Listener callbacks.

    void setLoopEnabledOnSounds(bool v)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                s->loopEnabled.store(v);
    }

    void setOneShotEnabledOnSounds(bool v)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                s->oneShotEnabled.store(v);
    }

    void setReverseEnabledOnSounds(bool v)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                s->reverseEnabled.store(v);
    }

    void setBounceEnabledOnSounds(bool v)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                s->bounceEnabled.store(v);
    }

    void setFreezeActiveOnSounds(bool v)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                s->freezeActive.store(v);
    }

    void setPitchOffsetOnSounds(int totalCents)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                s->pitchOffsetAtomic.store(totalCents);
    }

    void setBaseTuningRatioOnSounds(float ratio)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                s->baseTuningRatioAtomic.store(ratio);
    }

    void setStartPointOnSounds(double secs)
    {
        juce::ScopedLock lock(sampleLock);
        if (selectedSampleIndex < 0 || selectedSampleIndex >= samples.size()) return;
        auto* sample = samples[selectedSampleIndex];
        sample->startPointSeconds = secs;
        const double sr = sample->sampleRate > 0 ? sample->sampleRate : 44100.0;
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                const juce::int64 endSmp = snd->endSampleAtomic.load();
                juce::int64 startSmp = juce::jlimit<juce::int64>(0, endSmp - 1, (juce::int64)(secs * sr));
                snd->startSampleAtomic.store(startSmp);
            }
    }

    void setEndPointOnSounds(double secs)
    {
        juce::ScopedLock lock(sampleLock);
        if (selectedSampleIndex < 0 || selectedSampleIndex >= samples.size()) return;
        auto* sample = samples[selectedSampleIndex];
        sample->endPointSeconds = secs;
        const double sr = sample->sampleRate > 0 ? sample->sampleRate : 44100.0;
        const juce::int64 bufLen = sample->audioData ? sample->audioData->getNumSamples() : 0;
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                const juce::int64 startSmp = snd->startSampleAtomic.load();
                juce::int64 endSmp = (secs > 0.0) ? (juce::int64)(secs * sr) : bufLen;
                endSmp = juce::jlimit<juce::int64>(startSmp + 1, bufLen, endSmp);
                snd->endSampleAtomic.store(endSmp);
            }
    }

    void setAdsrParamsOnSounds(bool enabled, float atk, float dcy, float sus, float rel)
    {
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                s->customAdsrEnabled.store(enabled);
                s->customAdsrAttackMs.store(atk);
                s->customAdsrDecayMs.store(dcy);
                s->customAdsrSustain.store(sus);
                s->customAdsrReleaseMs.store(rel);
            }
    }

    // Apply a mirrored seek to all active voices (used for real-time reverse toggle).
    void applyMirroredSeekOnVoices()
    {
        juce::ScopedLock lock(sampleLock);
        if (selectedSampleIndex < 0 || selectedSampleIndex >= samples.size()) return;
        auto* sample = samples[selectedSampleIndex];
        if (!sample->isValid()) return;
        const double sr = sample->sampleRate > 0 ? sample->sampleRate : 44100.0;

        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                const juce::int64 sStart = snd->startSampleAtomic.load();
                const juce::int64 sEnd   = snd->endSampleAtomic.load();
                for (int v = 0; v < sampler.getNumVoices(); ++v)
                    if (auto* voice = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(v)))
                        if (voice->isVoiceActive())
                        {
                            const juce::int64 pos = voice->playheadPositionAtomic.load();
                            if (pos >= 0)
                            {
                                const juce::int64 mirrored = (sEnd - 1) - (pos - sStart);
                                voice->pendingSeekAtomic.store(mirrored);
                            }
                        }
                (void)sr;
            }
    }

    //==========================================================================
    // Playhead (60fps UI timer — reads atomics only, no locks)

    double getPlayheadPositionNormalized() const
    {
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
            {
                const juce::int64 pos   = v->playheadPositionAtomic.load();
                const juce::int64 total = v->totalSamplesAtomic.load();
                if (pos >= 0 && total > 0)
                    return (double)pos / (double)total;
            }
        return -1.0;
    }

    int getPlayheadDirection() const
    {
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
                if (v->isVoiceActive())
                    return v->playDirectionAtomic.load();
        return 1;
    }

    //==========================================================================
    // Normalize — scans audio data within start/end range.
    float computeNormGainFromAudio(float targetDb) const
    {
        juce::ScopedLock lock(sampleLock);
        if (selectedSampleIndex < 0 || selectedSampleIndex >= samples.size()) return 1.0f;
        const auto* sample = samples[selectedSampleIndex];
        if (!sample->isValid()) return 1.0f;

        const auto& buf  = *sample->audioData;
        const double sr  = sample->sampleRate > 0 ? sample->sampleRate : 44100.0;
        const int bufLen = buf.getNumSamples();

        int startSmp = (int)(sample->startPointSeconds * sr);
        int endSmp   = (sample->endPointSeconds > 0.0)
                     ? (int)(sample->endPointSeconds * sr) : bufLen;
        startSmp = juce::jlimit(0, bufLen - 1, startSmp);
        endSmp   = juce::jlimit(startSmp + 1, bufLen, endSmp);

        float peak = 0.0f;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            const float* src = buf.getReadPointer(ch);
            for (int i = startSmp; i < endSmp; ++i)
                peak = juce::jmax(peak, std::abs(src[i]));
        }

        if (peak <= 1e-7f) return 1.0f;
        return std::pow(10.0f, targetDb / 20.0f) / peak;
    }

    //==========================================================================
    // EQ helpers

    // Compute biquad coefficients for one band on the UI thread.
    // mode: 0=LowCut 1=LowShelf 2=Bell 3=Notch 4=HighShelf 5=HighCut
    EqCoeffDoubleBuffer::Coeffs computeEqCoeffs(
        float freqHz, float gainDb, float q, int filterMode, double sampleRate) const
    {
        const double w0    = juce::MathConstants<double>::twoPi * (double)freqHz / sampleRate;
        const double sinW0 = std::sin(w0);
        const double cosW0 = std::cos(w0);
        const double safeQ = (double)juce::jmax(q, 0.01f);
        const double alpha = sinW0 / (2.0 * safeQ);
        double b0=1.0, b1=0.0, b2=0.0, a0=1.0, a1=0.0, a2=0.0;

        switch (filterMode)
        {
            case 0: b0=(1.0+cosW0)/2.0; b1=-(1.0+cosW0); b2=(1.0+cosW0)/2.0;
                    a0=1.0+alpha;        a1=-2.0*cosW0;    a2=1.0-alpha; break;
            case 1: {
                const double A=std::pow(10.0,(double)gainDb/40.0), sqA=std::sqrt(juce::jmax(A,0.0001));
                const double al=sinW0/2.0*std::sqrt(juce::jmax((A+1.0/A)*(1.0/safeQ-1.0)+2.0,0.0));
                b0=A*((A+1.0)-(A-1.0)*cosW0+2.0*sqA*al); b1=2.0*A*((A-1.0)-(A+1.0)*cosW0);
                b2=A*((A+1.0)-(A-1.0)*cosW0-2.0*sqA*al);
                a0=(A+1.0)+(A-1.0)*cosW0+2.0*sqA*al; a1=-2.0*((A-1.0)+(A+1.0)*cosW0);
                a2=(A+1.0)+(A-1.0)*cosW0-2.0*sqA*al; break;
            }
            case 2: {
                const double A=std::pow(10.0,(double)gainDb/40.0);
                b0=1.0+alpha*A; b1=-2.0*cosW0; b2=1.0-alpha*A;
                a0=1.0+alpha/A; a1=-2.0*cosW0; a2=1.0-alpha/A; break;
            }
            case 3: b0=1.0; b1=-2.0*cosW0; b2=1.0;
                    a0=1.0+alpha; a1=-2.0*cosW0; a2=1.0-alpha; break;
            case 4: {
                const double A=std::pow(10.0,(double)gainDb/40.0), sqA=std::sqrt(juce::jmax(A,0.0001));
                const double al=sinW0/2.0*std::sqrt(juce::jmax((A+1.0/A)*(1.0/safeQ-1.0)+2.0,0.0));
                b0=A*((A+1.0)+(A-1.0)*cosW0+2.0*sqA*al); b1=-2.0*A*((A-1.0)+(A+1.0)*cosW0);
                b2=A*((A+1.0)+(A-1.0)*cosW0-2.0*sqA*al);
                a0=(A+1.0)-(A-1.0)*cosW0+2.0*sqA*al; a1=2.0*((A-1.0)-(A+1.0)*cosW0);
                a2=(A+1.0)-(A-1.0)*cosW0-2.0*sqA*al; break;
            }
            case 5: b0=(1.0-cosW0)/2.0; b1=1.0-cosW0; b2=(1.0-cosW0)/2.0;
                    a0=1.0+alpha;        a1=-2.0*cosW0;  a2=1.0-alpha; break;
            default: {
                const double A=std::pow(10.0,(double)gainDb/40.0);
                b0=1.0+alpha*A; b1=-2.0*cosW0; b2=1.0-alpha*A;
                a0=1.0+alpha/A; a1=-2.0*cosW0; a2=1.0-alpha/A; break;
            }
        }

        if (std::abs(a0) < 1e-30) return EqCoeffDoubleBuffer::Coeffs {};
        return EqCoeffDoubleBuffer::Coeffs { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
    }

    // Reset filter state — only when muteOutput=true or from prepareToPlay().
    void resetEqState()
    {
        for (int b = 0; b < 3; ++b)
            for (int ch = 0; ch < 2; ++ch)
                eqZ1[b][ch] = eqZ2[b][ch] = 0.0;
    }

    // Returns true only when a new FFT frame is ready — called by EQDisplay timer.
    bool getSpectrumSnapshot(float* dest, int numBins)
    {
        if (!hasNewFFTData.load(std::memory_order_acquire)) return false;
        hasNewFFTData.store(false, std::memory_order_relaxed);
        const int front = specFront.load(std::memory_order_acquire);
        const int count = juce::jmin(numBins, kSpecBins);
        std::memcpy(dest, specBuffers[front], sizeof(float) * (size_t)count);
        if (numBins > kSpecBins)
            std::memset(dest + kSpecBins, 0, sizeof(float) * (size_t)(numBins - kSpecBins));
        return true;
    }

    double getSampleRate() const { return sampler.getSampleRate(); }

    // Direct access to synthesiser for MainComponent orchestration.
    juce::Synthesiser& getSynthesiser() { return sampler; }

    //==========================================================================
    // Peak cache — allows instant pad switching without re-reading disk.
    // Stored when a sample is loaded; served to SampleCard::setAudioPeaksFromBuffer()
    // on pad selection so WaveformComponent can repaint immediately.

    bool hasSampleLoaded() const
    {
        return cachedPeakBins != nullptr && cachedPeakNumSamples > 0;
    }

    // Store a deep copy of the peak bins computed during sample load.
    // Called from MainComponent::loadSampleFileAsync callAsync lambda.
    void storePeakCache(const WaveformPeakBin* src, int numBins,
                        int numCh, juce::int64 numSamples, double sr)
    {
        if (src == nullptr || numBins <= 0) return;
        cachedPeakBins = std::make_unique<WaveformPeakBin[]>((size_t)numBins);
        std::memcpy(cachedPeakBins.get(), src, sizeof(WaveformPeakBin) * (size_t)numBins);
        cachedPeakNumBins    = numBins;
        cachedPeakNumCh      = numCh;
        cachedPeakNumSamples = numSamples;
        cachedPeakSampleRate = sr;
    }

    const WaveformPeakBin* getPeakBins()     const { return cachedPeakBins.get(); }
    int                    getPeakNumBins()   const { return cachedPeakNumBins; }
    int                    getPeakNumCh()     const { return cachedPeakNumCh; }
    juce::int64            getPeakNumSamples()const { return cachedPeakNumSamples; }
    double                 getPeakSampleRate()const { return cachedPeakSampleRate; }

    // Clear peak cache (called on sample unload / pad clear).
    void clearPeakCache()
    {
        cachedPeakBins.reset();
        cachedPeakNumBins    = 0;
        cachedPeakNumCh      = 0;
        cachedPeakNumSamples = 0;
        cachedPeakSampleRate = 0.0;
    }

    //==========================================================================
    // Public data — all members that MainComponent.cpp needs direct access to.
    // Keeping these public avoids verbose getter/setter boilerplate for the
    // dozens of atomic state variables and allows MainComponent to implement
    // complex sequences (freeze, sample-change, panic) cleanly.

    // Sample audio data (was MainComponent::samples etc.)
    juce::OwnedArray<MappedSample> samples;
    int selectedSampleIndex = -1;
    mutable juce::CriticalSection sampleLock;

    // Silence the audio thread immediately during sample changes.
    std::atomic<bool> muteOutput { false };

    // Per-pad volume / normalize (applied in renderNextBlock before master vol)
    std::atomic<float> volumeGain { 1.0f };
    std::atomic<float> normGain   { 1.0f };
    std::atomic<bool>  loopEnabled { false };

    // Playback mode params — stored so updateSamplerSounds() can read them.
    std::atomic<bool>  oneShotEnabled { false };
    std::atomic<bool>  reverseEnabled { false };
    std::atomic<bool>  bounceEnabled  { false };
    std::atomic<float> baseTuningRatio { 1.0f };

    // ADSR params — stored for updateSamplerSounds().
    std::atomic<bool>  adsrEnabled   { false };
    std::atomic<float> adsrAttackMs  { 0.0f };
    std::atomic<float> adsrDecayMs   { 0.0f };
    std::atomic<float> adsrSustain   { 1.0f };
    std::atomic<float> adsrReleaseMs { 0.0f };

    // EQ double buffer — UI thread writes, audio thread reads in nanoseconds.
    EqCoeffDoubleBuffer  eqCoeffDB;
    std::atomic<bool>    eqActive { false };
    int                  eqFilterModes[3] = { 2, 2, 2 };  // message-thread only
    EqCoeffDoubleBuffer::Coeffs defaultFlatCoeffs[3];

    // Timing diagnostics: written by audio/MIDI thread, read by UI CPU timer.
    std::atomic<juce::int64> midiNoteOnTicks    { 0 };
    std::atomic<juce::int64> lastMidiLatencyUs  { -1 };
    std::atomic<juce::int64> eqResetRequestedMs { 0 };
    std::atomic<juce::int64> eqResetElapsedMs   { -1 };

private:
    //==========================================================================
    // Internal state — not accessed from outside

    juce::AudioFormatManager& formatManager;
    juce::Synthesiser sampler;

    // Peak cache — deep copy of bins computed at load time.
    // Allows instant pad switching (no disk access).
    std::unique_ptr<WaveformPeakBin[]> cachedPeakBins;
    int         cachedPeakNumBins    = 0;
    int         cachedPeakNumCh      = 0;
    juce::int64 cachedPeakNumSamples = 0;
    double      cachedPeakSampleRate = 0.0;

    // EQ filter state — audio thread only.
    double eqZ1[3][2] {};
    double eqZ2[3][2] {};

    //==========================================================================
    // FFT spectrum analyzer

    static constexpr int kFFTOrder = 11;
    static constexpr int kFFTSize  = 1 << kFFTOrder;  // 2048
    static constexpr int kSpecBins = kFFTSize / 2;     // 1024

    static constexpr float kSpecSmoothUp   = 0.7f;
    static constexpr float kSpecSmoothDown = 0.3f;

    std::unique_ptr<juce::dsp::FFT> fft;

    // Private scratch buffer — renderNextBlock() renders into this, not into the shared
    // outputBuffer, so EQ and FFT see only this pad's audio before mixing.
    juce::AudioBuffer<float> ownBuffer;

    juce::AbstractFifo fftAbstractFifo { kFFTSize * 2 };
    float              fftCircularBuffer[kFFTSize * 2] {};
    float              fftWindow[kFFTSize]   {};
    float              fftScratch[kFFTSize * 2] {};

    float             specBuffers[2][kSpecBins] {};
    std::atomic<int>  specFront     { 0 };
    std::atomic<bool> hasNewFFTData { false };

    class FftWorkerThread : public juce::Thread
    {
    public:
        FftWorkerThread(PadAudioEngine& o) : juce::Thread("FFT Worker"), owner(o) {}
        void run() override
        {
            while (!threadShouldExit())
            {
                wait(33);
                if (!threadShouldExit())
                    owner.processFftOnWorkerThread();
            }
        }
    private:
        PadAudioEngine& owner;
    };
    std::unique_ptr<FftWorkerThread> fftThread;

    void processFftOnWorkerThread()
    {
        if (fft == nullptr) return;
        if (fftAbstractFifo.getNumReady() < kFFTSize) return;

        int start1, size1, start2, size2;
        fftAbstractFifo.prepareToRead(kFFTSize, start1, size1, start2, size2);

        int dst = 0;
        for (int i = 0; i < size1; ++i, ++dst)
        {
            fftScratch[dst * 2]     = fftCircularBuffer[start1 + i] * fftWindow[dst];
            fftScratch[dst * 2 + 1] = 0.0f;
        }
        for (int i = 0; i < size2; ++i, ++dst)
        {
            fftScratch[dst * 2]     = fftCircularBuffer[start2 + i] * fftWindow[dst];
            fftScratch[dst * 2 + 1] = 0.0f;
        }
        fftAbstractFifo.finishedRead(size1 + size2);

        fft->performFrequencyOnlyForwardTransform(fftScratch);

        const int back = 1 - specFront.load(std::memory_order_relaxed);
        for (int k = 0; k < kSpecBins; ++k)
        {
            const float mag  = fftScratch[k] / static_cast<float>(kFFTSize);
            const float db   = juce::Decibels::gainToDecibels(mag, -100.0f);
            const float prev = specBuffers[back][k];
            specBuffers[back][k] = (db > prev)
                ? kSpecSmoothUp   * db + (1.0f - kSpecSmoothUp)   * prev
                : kSpecSmoothDown * db + (1.0f - kSpecSmoothDown) * prev;
        }
        specFront.store(back, std::memory_order_release);
        hasNewFFTData.store(true, std::memory_order_release);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PadAudioEngine)
};
