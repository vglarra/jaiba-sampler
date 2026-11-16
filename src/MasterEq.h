#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

//==============================================================================
// Master-bus EQ: three peaking (bell) biquads, applied to the fully mixed output
// *before* the master volume so the volume knob stays a clean output trim.
//
// The coefficient helper is shared with the UI, which draws the same response.
//==============================================================================

struct MasterEqCoeffs
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

// Filter shape per band, mirroring the per-pad EQ order.
//   0 = LowCut, 1 = LowShelf, 2 = Bell, 3 = Notch, 4 = HighShelf, 5 = HighCut
inline const char* masterEqModeName (int mode)
{
    switch (mode)
    {
        case 0: return "Low Cut";
        case 1: return "Low Shelf";
        case 2: return "Bell";
        case 3: return "Notch";
        case 4: return "High Shelf";
        case 5: return "High Cut";
        default: return "Bell";
    }
}

// RBJ audio-EQ-cookbook coefficients for the requested shape.
inline MasterEqCoeffs makeEqCoeffs (float freq, float gainDb, float q, int mode, double sampleRate)
{
    MasterEqCoeffs c;
    if (sampleRate <= 0.0)
        return c;

    const float clampedHz = juce::jlimit (10.0f, (float) sampleRate * 0.49f, freq);
    const float w0        = 2.0f * juce::MathConstants<float>::pi * clampedHz / (float) sampleRate;
    const float cosw0     = std::cos (w0);
    const float sinw0     = std::sin (w0);
    const float A         = std::pow (10.0f, gainDb / 40.0f);
    const float qq        = juce::jmax (0.1f, q);
    const float alpha     = sinw0 / (2.0f * qq);

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

    switch (mode)
    {
        case 0:  // LowCut (high-pass)
        {
            b0 = (1.0f + cosw0) * 0.5f;
            b1 = -(1.0f + cosw0);
            b2 = (1.0f + cosw0) * 0.5f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosw0;
            a2 = 1.0f - alpha;
            break;
        }
        case 1:  // LowShelf
        {
            const float sqA = std::sqrt (A);
            const float beta = 2.0f * sqA * alpha;
            b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + beta);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
            b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - beta);
            a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + beta;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
            a2 = (A + 1.0f) + (A - 1.0f) * cosw0 - beta;
            break;
        }
        case 3:  // Notch
        {
            b0 = 1.0f;
            b1 = -2.0f * cosw0;
            b2 = 1.0f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosw0;
            a2 = 1.0f - alpha;
            break;
        }
        case 4:  // HighShelf
        {
            const float sqA = std::sqrt (A);
            const float beta = 2.0f * sqA * alpha;
            b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + beta);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
            b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - beta);
            a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + beta;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
            a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - beta;
            break;
        }
        case 5:  // HighCut (low-pass)
        {
            b0 = (1.0f - cosw0) * 0.5f;
            b1 = 1.0f - cosw0;
            b2 = (1.0f - cosw0) * 0.5f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosw0;
            a2 = 1.0f - alpha;
            break;
        }
        case 2:  // Bell (peaking)
        default:
        {
            b0 = 1.0f + alpha * A;
            b1 = -2.0f * cosw0;
            b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1 = -2.0f * cosw0;
            a2 = 1.0f - alpha / A;
            break;
        }
    }

    if (std::abs (a0) < 1.0e-12f)
        return c;

    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

// Magnitude response of a biquad at a given frequency (linear gain).
inline float biquadMagnitude (const MasterEqCoeffs& c, float freq, double sampleRate)
{
    if (sampleRate <= 0.0)
        return 1.0f;

    const float w = 2.0f * juce::MathConstants<float>::pi * freq / (float) sampleRate;
    const float cz1r = std::cos (w),  cz1i = -std::sin (w);   // z^-1
    const float cz2r = std::cos (2.0f * w), cz2i = -std::sin (2.0f * w);

    // Numerator: b0 + b1 z^-1 + b2 z^-2
    const float nr = c.b0 + c.b1 * cz1r + c.b2 * cz2r;
    const float ni = c.b1 * cz1i + c.b2 * cz2i;

    // Denominator: 1 + a1 z^-1 + a2 z^-2
    const float dr = 1.0f + c.a1 * cz1r + c.a2 * cz2r;
    const float di = c.a1 * cz1i + c.a2 * cz2i;

    const float num = std::sqrt (nr * nr + ni * ni);
    const float den = std::sqrt (dr * dr + di * di);
    return den > 1.0e-9f ? num / den : 0.0f;
}

//==============================================================================
class MasterEqProcessor
{
public:
    static constexpr int kNumBands = 3;

    void prepare (double newSampleRate)
    {
        sampleRate.store (newSampleRate > 0.0 ? newSampleRate : 44100.0);
        resetRequested.store (true);
    }

    void reset()
    {
        for (auto& band : states)
            for (auto& ch : band)
                ch = {};
    }

    void setEnabled (bool shouldBeEnabled)
    {
        enabled.store (shouldBeEnabled);
        resetRequested.store (true);
    }

    bool isEnabled() const { return enabled.load(); }

    void setBandMode (int index, int mode)
    {
        if (index < 0 || index >= kNumBands)
            return;
        bandMode[(size_t) index].store (juce::jlimit (0, 5, mode));
    }

    int getBandMode (int index) const
    {
        return bandMode[(size_t) juce::jlimit (0, kNumBands - 1, index)].load();
    }

    void setBand (int index, float freq, float gainDb, float q = 1.0f)
    {
        if (index < 0 || index >= kNumBands)
            return;

        bandFreq[(size_t) index].store (juce::jlimit (20.0f, 20000.0f, freq));
        bandGain[(size_t) index].store (juce::jlimit (-24.0f, 24.0f, gainDb));
        bandQ[(size_t) index].store (juce::jlimit (0.2f, 8.0f, q));
    }

    float getBandFreq (int index) const { return bandFreq[(size_t) juce::jlimit (0, kNumBands - 1, index)].load(); }
    float getBandGain (int index) const { return bandGain[(size_t) juce::jlimit (0, kNumBands - 1, index)].load(); }
    float getBandQ    (int index) const { return bandQ[(size_t) juce::jlimit (0, kNumBands - 1, index)].load(); }

    // Audio thread. Processes in place; a no-op when the EQ is disabled.
    void process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
    {
        if (! enabled.load (std::memory_order_relaxed))
            return;

        if (resetRequested.exchange (false))
        {
            reset();

            for (int b = 0; b < kNumBands; ++b)
            {
                curFreq[b] = lastFreq[b] = (float) bandFreq[(size_t) b].load();
                curGain[b] = lastGain[b] = (float) bandGain[(size_t) b].load();
                lastMode[b] = bandMode[(size_t) b].load();
                coeffs[(size_t) b] = makeEqCoeffs (curFreq[b], curGain[b],
                                                   (float) bandQ[(size_t) b].load(),
                                                   lastMode[b], sampleRate.load());
            }
        }

        // One-pole smoothing (time constant ~ a few blocks) so dragging a band
        // changes the coefficients gradually instead of stepping per block.
        const auto sr = sampleRate.load();
        for (int b = 0; b < kNumBands; ++b)
        {
            const float tf = bandFreq[(size_t) b].load();
            const float tg = bandGain[(size_t) b].load();
            const int   tm = bandMode[(size_t) b].load();

            curFreq[b] += (tf - curFreq[b]) * 0.25f;
            curGain[b] += (tg - curGain[b]) * 0.25f;

            if (std::abs (curFreq[b] - lastFreq[b]) > 1.0e-3f
                || std::abs (curGain[b] - lastGain[b]) > 1.0e-3f
                || tm != lastMode[b])
            {
                lastFreq[b] = curFreq[b];
                lastGain[b] = curGain[b];
                lastMode[b] = tm;
                coeffs[(size_t) b] = makeEqCoeffs (curFreq[b], curGain[b],
                                                   bandQ[(size_t) b].load(), tm, sr);
            }
        }

        const int numCh = juce::jmin (2, buffer.getNumChannels());
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch, startSample);

            for (int i = 0; i < numSamples; ++i)
            {
                float x = d[i];
                for (int b = 0; b < kNumBands; ++b)
                {
                    const auto& c = coeffs[(size_t) b];
                    auto& s = states[(size_t) b][(size_t) ch];

                    const float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2
                                    - c.a1 * s.y1 - c.a2 * s.y2;
                    s.x2 = s.x1; s.x1 = x;
                    s.y2 = s.y1; s.y1 = y;
                    x = y;
                }
                d[i] = x;
            }
        }
    }

private:
    struct BiquadState { float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f; };

    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<bool>   enabled { false };
    std::atomic<bool>   resetRequested { true };

    std::atomic<float> bandFreq[kNumBands] { { 100.0f }, { 1000.0f }, { 8000.0f } };
    std::atomic<float> bandGain[kNumBands] { { 0.0f }, { 0.0f }, { 0.0f } };
    std::atomic<float> bandQ[kNumBands]    { { 1.0f }, { 1.0f }, { 1.0f } };
    std::atomic<int>   bandMode[kNumBands] { { 2 }, { 2 }, { 2 } };

    MasterEqCoeffs coeffs[kNumBands];

    // Audio-thread parameter smoothing (per block): avoids zipper noise when a
    // band is dragged while audio is playing.
    float curFreq[kNumBands]  { 100.0f, 1000.0f, 8000.0f };
    float curGain[kNumBands]  { 0.0f, 0.0f, 0.0f };
    float lastFreq[kNumBands] { 100.0f, 1000.0f, 8000.0f };
    float lastGain[kNumBands] { 0.0f, 0.0f, 0.0f };
    int   lastMode[kNumBands] { 2, 2, 2 };
    BiquadState    states[kNumBands][2];
};
