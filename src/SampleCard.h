#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <vector>
#include "KnobLookAndFeel.h"
#include "PadSettings.h"
#include "WaveformPeakBin.h"

// Forward declaration — EQDisplay is defined after WaveformViewport, before SampleCard.
class EQDisplay;

// Custom Viewport subclass that exposes visibleAreaChanged so SampleCard can
// track the horizontal scroll position for the zoom indicator.
class WaveformViewport : public juce::Viewport
{
public:
    std::function<void(int scrollX)> onScrollChanged;

    void visibleAreaChanged(const juce::Rectangle<int>& newVisibleArea) override
    {
        if (onScrollChanged)
            onScrollChanged(newVisibleArea.getX());
    }
};

// ===========================================================================
// EQDisplay — parametric EQ display with real-time spectrum overlay.
// Drawn inside the EQ tab of SampleCard.
// Spectrum data is pushed from the audio thread via getSpectrumCallback.
// Band dragging fires onBandChanged; scroll-wheel adjusts Q factor.
// ===========================================================================
class EQDisplay : public juce::Component, public juce::Timer
{
public:
    struct Band
    {
        float freq   = 500.0f;  // Hz
        float gainDb = 0.0f;    // -12..+12
        float q      = 1.0f;    // 0.5..10
    };

    // Wire from MainComponent — fills dest[0..numBins-1] with dB magnitudes.
    // OPT 4: Returns true only when new FFT data is available; false = skip repaint.
    std::function<bool(float* dest, int numBins)> getSpectrumCallback;

    // Fired on every drag / scroll so MainComponent can update filter coefficients.
    std::function<void(int bandIdx, float freq, float gainDb, float q)> onBandChanged;

    // Fired when the user clicks a control point, making it the active band.
    std::function<void(int bandIdx)> onActiveBandChanged;

    // Filter mode per band: 0=LowCut, 1=LowShelf, 2=Bell, 3=Notch, 4=HighShelf, 5=HighCut
    void setFilterMode(int bandIdx, int mode)
    {
        if (bandIdx >= 0 && bandIdx < 3) { filterModes[bandIdx] = mode; repaint(); }
    }
    int getFilterMode(int bandIdx) const
    {
        return (bandIdx >= 0 && bandIdx < 3) ? filterModes[bandIdx] : 2;
    }

    // Active band — the last band the user clicked (shows which band the filter mode button controls)
    void setActiveBand(int band) { activeBand = band; repaint(); }
    int  getActiveBand() const   { return activeBand; }

    // Returns the human-readable name for a filter mode index (0-5).
    static const char* filterModeName(int mode)
    {
        static const char* names[] = { "Low Cut", "Low Shelf", "Bell", "Notch", "High Shelf", "High Cut" };
        return (mode >= 0 && mode < 6) ? names[mode] : "Bell";
    }

    EQDisplay()
    {
        bands[0] = { 100.0f,  0.0f, 1.0f };
        bands[1] = { 500.0f,  0.0f, 1.0f };
        bands[2] = { 8000.0f, 0.0f, 1.0f };
        std::fill(spectrumData, spectrumData + kSpecBins, -120.0f);
    }

    ~EQDisplay() override { stopTimer(); }

    void setEqEnabled(bool e)  { eqEnabled = e;  repaint(); }
    void setSampleRate(double sr) { sampleRate = juce::jmax(sr, 1.0); }

    void setBands(const Band newBands[3])
    {
        for (int i = 0; i < 3; ++i) bands[i] = newBands[i];
        repaint();
    }
    void setBand(int i, const Band& b)
    {
        if (i >= 0 && i < 3) { bands[i] = b; repaint(); }
    }
    const Band* getBands() const { return bands; }

    void startAnimation() { startTimerHz(30); }
    void stopAnimation()  { stopTimer(); }

    // OPT 4: Timer callback — only repaint when new FFT data is actually available (30fps max).
    // During an active drag the spectrum is paused — only the EQ curve needs redrawing,
    // and that happens immediately from mouseDrag via repaint().
    void timerCallback() override
    {
        if (eqIsDragging) return;   // spectrum paused during drag — curve updated via mouseDrag

        if (getSpectrumCallback)
        {
            // getSpectrumCallback returns true only when the FFT worker has produced new data.
            // This prevents unnecessary repaints (saving UI thread CPU) at a steady 30fps cap.
            if (getSpectrumCallback(spectrumData, kSpecBins))
                repaint();
        }
    }

    bool isEqDragging() const { return eqIsDragging; }

    // -----------------------------------------------------------------------
    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll(juce::Colour(0xFF0D0D0D));
        drawGrid(g, bounds);
        if (getSpectrumCallback) drawSpectrum(g, bounds);
        drawEQCurve(g, bounds);
        drawControlPoints(g, bounds);
        if (draggingBand >= 0) drawTooltip(g, bounds);
        g.setColour(juce::Colour(0xFF333333));
        g.drawRect(bounds, 1);
    }

    // -----------------------------------------------------------------------
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.getNumberOfClicks() > 1) return;
        draggingBand = findNearestBand(e.x, e.y);
        if (draggingBand >= 0)
        {
            eqIsDragging = true;
            lastDragPixelX = e.x;
            lastDragPixelY = e.y;

            // Set active band and notify SampleCard so the filter mode button can update.
            activeBand = draggingBand;
            if (onActiveBandChanged) onActiveBandChanged(activeBand);

            dragStartX      = (float)e.x;
            dragStartY      = (float)e.y;
            dragStartFreq   = bands[draggingBand].freq;
            dragStartGainDb = bands[draggingBand].gainDb;
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (draggingBand < 0) return;

        // Throttle: skip if the pointer hasn't moved at least 1 pixel since last update.
        // JUCE can fire mouseDrag at 200+ Hz — most events carry identical coordinates.
        if (std::abs(e.x - lastDragPixelX) < 1 && std::abs(e.y - lastDragPixelY) < 1)
            return;
        lastDragPixelX = e.x;
        lastDragPixelY = e.y;

        const float w = (float)getWidth();
        const float h = (float)getHeight();
        float newFreq   = xToFreq((float)e.x, w);
        float newGainDb = yToGain((float)e.y, h);

        // Per-band frequency range
        float minF, maxF;
        switch (draggingBand)
        {
            case 0: minF =  20.0f; maxF =   500.0f; break;
            case 1: minF = 200.0f; maxF =  5000.0f; break;
            case 2: minF =1000.0f; maxF = 20000.0f; break;
            default:minF =  20.0f; maxF = 20000.0f;
        }
        bands[draggingBand].freq   = juce::jlimit(minF, maxF, newFreq);
        bands[draggingBand].gainDb = juce::jlimit(-12.0f, 12.0f, newGainDb);

        if (onBandChanged)
            onBandChanged(draggingBand, bands[draggingBand].freq,
                          bands[draggingBand].gainDb, bands[draggingBand].q);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        draggingBand   = -1;
        eqIsDragging   = false;
        lastDragPixelX = -9999;
        lastDragPixelY = -9999;
        repaint();  // final repaint including spectrum
    }

    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& d) override
    {
        int band = findNearestBand(e.x, e.y);
        if (band < 0) return;
        float factor = d.deltaY > 0 ? 1.15f : (1.0f / 1.15f);
        bands[band].q = juce::jlimit(0.5f, 10.0f, bands[band].q * factor);
        if (onBandChanged)
            onBandChanged(band, bands[band].freq, bands[band].gainDb, bands[band].q);
        repaint();
    }

private:
    // -----------------------------------------------------------------------
    static constexpr int   kSpecBins = 1024;
    static constexpr float kMinHz    = 20.0f;
    static constexpr float kMaxHz    = 20000.0f;
    static constexpr float kEqMinDb  = -12.0f;
    static constexpr float kEqMaxDb  = +12.0f;

    Band   bands[3];
    float  spectrumData[kSpecBins];
    double sampleRate    = 44100.0;
    bool   eqEnabled     = false;
    int    draggingBand  = -1;
    int    activeBand    = -1;  // last band clicked — filter mode button applies to this
    int    filterModes[3] = { 2, 2, 2 };  // per-band: 0=LowCut 1=LowShelf 2=Bell 3=Notch 4=HighShelf 5=HighCut
    float  dragStartX    = 0.0f;
    float  dragStartY    = 0.0f;
    float  dragStartFreq   = 500.0f;
    float  dragStartGainDb = 0.0f;

    // Drag optimisation state.
    // eqIsDragging: true while mouse button is held on a control point.
    //   → timerCallback skips spectrum fetch so FFT updates don't compete with drag repaints.
    // lastDragPixelX/Y: previous mouse position; mouseDrag skips processing if < 1px moved.
    bool eqIsDragging  = false;
    int  lastDragPixelX = -9999;
    int  lastDragPixelY = -9999;

    // -----------------------------------------------------------------------
    // Log-frequency ↔ pixel x  (20 Hz … 20 kHz, full width)
    float freqToX(float freq, float width) const
    {
        return width * std::log10(freq / kMinHz) / std::log10(kMaxHz / kMinHz);
    }
    float xToFreq(float x, float width) const
    {
        float t = juce::jlimit(0.0f, 1.0f, x / juce::jmax(width, 1.0f));
        return kMinHz * std::pow(kMaxHz / kMinHz, t);
    }
    // dB gain ↔ pixel y  (+12 dB at top, -12 dB at bottom)
    float gainToY(float db, float height) const
    {
        float t = 1.0f - (db - kEqMinDb) / (kEqMaxDb - kEqMinDb);
        return height * t;
    }
    float yToGain(float y, float height) const
    {
        float t = 1.0f - y / juce::jmax(height, 1.0f);
        return kEqMinDb + t * (kEqMaxDb - kEqMinDb);
    }

    // Nearest control point within hit radius
    int findNearestBand(int mx, int my) const
    {
        const float w = (float)getWidth(), h = (float)getHeight();
        const float kHitRadius = 18.0f;
        int   best = -1;
        float bestDist = kHitRadius;
        for (int i = 0; i < 3; ++i)
        {
            float cx = freqToX(bands[i].freq, w);
            float cy = gainToY(bands[i].gainDb, h);
            float d  = std::sqrt((mx - cx) * (mx - cx) + (my - cy) * (my - cy));
            if (d < bestDist) { bestDist = d; best = i; }
        }
        return best;
    }

    // -----------------------------------------------------------------------
    // Biquad peaking EQ (Audio EQ Cookbook)
    struct BiqCoeffs { double b0, b1, b2, a1, a2; };

    BiqCoeffs computePeaking(float freq, float gainDb, float q) const
    {
        if (std::abs(gainDb) < 0.001f) return { 1, 0, 0, 0, 0 };
        double A    = std::pow(10.0, gainDb / 40.0);
        double w0   = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
        double al   = std::sin(w0) / (2.0 * q);
        double cw   = std::cos(w0);
        double a0   = 1.0 + al / A;
        return { (1.0 + al * A) / a0,
                 (-2.0 * cw)    / a0,
                 (1.0 - al * A) / a0,
                 (-2.0 * cw)    / a0,
                 (1.0 - al / A) / a0 };
    }

    // Compute biquad coefficients for any of the 6 filter types using Audio EQ Cookbook formulas.
    BiqCoeffs computeCoeffsForMode(float freq, float gainDb, float q, int mode) const
    {
        const double w0    = juce::MathConstants<double>::twoPi * (double)freq / sampleRate;
        const double sw    = std::sin(w0);
        const double cw    = std::cos(w0);
        const double alpha = sw / (2.0 * (double)juce::jmax(q, 0.01f));

        switch (mode)
        {
            case 0: // Low Cut — 2nd order highpass
            {
                const double a0 = 1.0 + alpha;
                return { ((1.0 + cw) / 2.0) / a0,
                         (-(1.0 + cw))      / a0,
                         ((1.0 + cw) / 2.0) / a0,
                         (-2.0 * cw)        / a0,
                         (1.0 - alpha)      / a0 };
            }
            case 1: // Low Shelf
            {
                const double A   = std::pow(10.0, gainDb / 40.0);
                const double sqA = std::sqrt(juce::jmax(A, 0.0001));
                const double arg = (A + 1.0 / A) * (1.0 / (double)juce::jmax(q, 0.01f) - 1.0) + 2.0;
                const double al  = sw / 2.0 * std::sqrt(juce::jmax(arg, 0.0));
                const double a0  = (A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * al;
                if (std::abs(a0) < 1e-30) return { 1, 0, 0, 0, 0 };
                return { A * ((A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * al) / a0,
                         2.0 * A * ((A - 1.0) - (A + 1.0) * cw)            / a0,
                         A * ((A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * al) / a0,
                         -2.0 * ((A - 1.0) + (A + 1.0) * cw)               / a0,
                         ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * al)    / a0 };
            }
            case 2: // Bell (peaking EQ) — existing formula
                return computePeaking(freq, gainDb, q);
            case 3: // Notch
            {
                const double a0 = 1.0 + alpha;
                return { 1.0          / a0,
                         (-2.0 * cw) / a0,
                         1.0          / a0,
                         (-2.0 * cw) / a0,
                         (1.0 - alpha)/ a0 };
            }
            case 4: // High Shelf
            {
                const double A   = std::pow(10.0, gainDb / 40.0);
                const double sqA = std::sqrt(juce::jmax(A, 0.0001));
                const double arg = (A + 1.0 / A) * (1.0 / (double)juce::jmax(q, 0.01f) - 1.0) + 2.0;
                const double al  = sw / 2.0 * std::sqrt(juce::jmax(arg, 0.0));
                const double a0  = (A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * al;
                if (std::abs(a0) < 1e-30) return { 1, 0, 0, 0, 0 };
                return { A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * al)  / a0,
                         -2.0 * A * ((A - 1.0) + (A + 1.0) * cw)            / a0,
                         A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * al) / a0,
                         2.0 * ((A - 1.0) - (A + 1.0) * cw)                 / a0,
                         ((A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * al)     / a0 };
            }
            case 5: // High Cut — 2nd order lowpass
            {
                const double a0 = 1.0 + alpha;
                return { ((1.0 - cw) / 2.0) / a0,
                         (1.0 - cw)          / a0,
                         ((1.0 - cw) / 2.0) / a0,
                         (-2.0 * cw)         / a0,
                         (1.0 - alpha)       / a0 };
            }
            default:
                return computePeaking(freq, gainDb, q);
        }
    }

    // Magnitude response of one biquad in dB at testFreq
    double filterResponseDb(const BiqCoeffs& c, double testFreq) const
    {
        double w   = 2.0 * juce::MathConstants<double>::pi * testFreq / sampleRate;
        double cw  = std::cos(w),  sw  = std::sin(w);
        double c2w = std::cos(2.0 * w), s2w = std::sin(2.0 * w);
        double nr = c.b0 + c.b1 * cw  + c.b2 * c2w;
        double ni =        c.b1 * (-sw) + c.b2 * (-s2w);
        double dr = 1.0  + c.a1 * cw  + c.a2 * c2w;
        double di =        c.a1 * (-sw) + c.a2 * (-s2w);
        double mag2 = (nr * nr + ni * ni) / juce::jmax(dr * dr + di * di, 1e-30);
        return 20.0 * std::log10(std::sqrt(juce::jmax(mag2, 1e-30)));
    }

    // -----------------------------------------------------------------------
    void drawGrid(juce::Graphics& g, juce::Rectangle<int> b) const
    {
        const float w = (float)b.getWidth();
        const float h = (float)b.getHeight();

        // Vertical frequency lines
        g.setColour(juce::Colour(0xFF555555));
        const float vFreqs[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        for (float f : vFreqs)
            g.drawVerticalLine((int)(b.getX() + freqToX(f, w)), (float)b.getY(), (float)b.getBottom());

        // Horizontal dB lines — 0dB is brighter and thicker as a reference
        const float dbLines[] = { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f };
        for (float db : dbLines)
        {
            float y = (float)(b.getY()) + gainToY(db, h);
            if (db == 0.0f)
            {
                g.setColour(juce::Colour(0xFF777777));
                g.drawLine((float)b.getX(), y, (float)b.getRight(), y, 1.5f);
            }
            else
            {
                g.setColour(juce::Colour(0xFF444444));
                g.drawHorizontalLine((int)y, (float)b.getX(), (float)b.getRight());
            }
        }

        // Frequency labels (bottom)
        g.setColour(juce::Colour(0xFFBBBBBB));
        g.setFont(9.0f);
        const float labelFs[]    = { 100.0f, 1000.0f, 10000.0f };
        const char* labelTexts[] = { "100",  "1k",    "10k" };
        for (int i = 0; i < 3; ++i)
        {
            int x = b.getX() + (int)freqToX(labelFs[i], w);
            g.drawText(labelTexts[i], x - 12, b.getBottom() - 12, 24, 11,
                       juce::Justification::centred);
        }

        // dB labels (left)
        g.setColour(juce::Colour(0xFFBBBBBB));
        const char* dbTexts[] = { "+12", "+6", "0", "-6", "-12" };
        for (int i = 0; i < 5; ++i)
        {
            int y = b.getY() + (int)gainToY(dbLines[i], h);
            g.drawText(dbTexts[i], b.getX() + 1, y - 5, 22, 11, juce::Justification::left);
        }
    }

    void drawSpectrum(juce::Graphics& g, juce::Rectangle<int> b) const
    {
        const float w = (float)b.getWidth();
        const float h = (float)b.getHeight();
        juce::Path fill, stroke;
        bool started = false;

        for (int px = 0; px < (int)w; ++px)
        {
            float freq = xToFreq((float)px, w);
            float nyq  = (float)(sampleRate * 0.5);
            float binf = freq / nyq * (float)(kSpecBins - 1);
            binf = juce::jlimit(0.0f, (float)(kSpecBins - 1), binf);
            int   b0 = (int)binf, b1 = juce::jmin(b0 + 1, kSpecBins - 1);
            float fr  = binf - (float)b0;
            float db  = spectrumData[b0] * (1.0f - fr) + spectrumData[b1] * fr;
            db = juce::jlimit(-80.0f, 6.0f, db);
            // Map dB to y: 0 dB → top, -80 dB → bottom
            float norm = (db + 80.0f) / 86.0f;
            float y    = (float)(b.getBottom()) - norm * h;

            float fx = (float)(b.getX() + px);
            if (!started)
            {
                fill.startNewSubPath(fx, (float)b.getBottom());
                fill.lineTo(fx, y);
                stroke.startNewSubPath(fx, y);
                started = true;
            }
            else
            {
                fill.lineTo(fx, y);
                stroke.lineTo(fx, y);
            }
        }

        if (started)
        {
            fill.lineTo((float)b.getRight(), (float)b.getBottom());
            fill.closeSubPath();
            g.setColour(juce::Colour(0x99B4142A));
            g.fillPath(fill);
            g.setColour(juce::Colour(0xFFFF2244));
            g.strokePath(stroke, juce::PathStrokeType(1.5f));
        }
    }

    void drawEQCurve(juce::Graphics& g, juce::Rectangle<int> b) const
    {
        const float w = (float)b.getWidth();
        const float h = (float)b.getHeight();

        BiqCoeffs c[3];
        for (int i = 0; i < 3; ++i)
            c[i] = computeCoeffsForMode(bands[i].freq, bands[i].gainDb, bands[i].q, filterModes[i]);

        juce::Path curve;
        bool started = false;

        for (int px = 0; px < (int)w; px += 2)
        {
            float freq = xToFreq((float)px, w);
            if (freq < kMinHz || freq > kMaxHz) continue;

            double totalDb = 0.0;
            for (int i = 0; i < 3; ++i)
                totalDb += filterResponseDb(c[i], (double)freq);

            float y = (float)(b.getY()) + gainToY((float)juce::jlimit(-13.0, 13.0, totalDb), h);
            float fx = (float)(b.getX() + px);

            if (!started) { curve.startNewSubPath(fx, y); started = true; }
            else          { curve.lineTo(fx, y); }
        }

        if (started)
        {
            juce::Colour curveCol = eqEnabled ? juce::Colour(0xFFFF4466)
                                              : juce::Colour(0xFF885566);
            g.setColour(curveCol);
            g.strokePath(curve, juce::PathStrokeType(2.0f));
        }
    }

    void drawControlPoints(juce::Graphics& g, juce::Rectangle<int> b) const
    {
        const float w = (float)b.getWidth();
        const float h = (float)b.getHeight();

        for (int i = 0; i < 3; ++i)
        {
            float cx = (float)b.getX() + freqToX(bands[i].freq, w);
            float cy = (float)b.getY() + gainToY(bands[i].gainDb, h);

            const bool isActive   = (i == activeBand);
            const bool isDragging = (i == draggingBand);
            const float r = isActive ? 10.0f : 8.0f;

            // Shadow
            g.setColour(juce::Colour(0x88000000));
            g.fillEllipse(cx - r - 1.0f, cy - r - 1.0f, (r + 1.0f) * 2.0f, (r + 1.0f) * 2.0f);

            // Body
            juce::Colour col = isDragging ? juce::Colour(0xFF44E8FF) : juce::Colour(0xFF00CFFF);
            g.setColour(col);
            g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);

            // Active band ring — bright white outline to show which band filter mode applies to
            if (isActive && !isDragging)
            {
                g.setColour(juce::Colour(0xCCFFFFFF));
                g.drawEllipse(cx - r - 1.5f, cy - r - 1.5f, (r + 1.5f) * 2.0f, (r + 1.5f) * 2.0f, 1.5f);
            }

            // Band number
            g.setColour(juce::Colour(0xFF000000));
            g.setFont(9.0f);
            g.drawText(juce::String(i + 1),
                       (int)(cx - 4), (int)(cy - 5), 9, 10,
                       juce::Justification::centred);
        }
    }

    void drawTooltip(juce::Graphics& g, juce::Rectangle<int> b) const
    {
        if (draggingBand < 0) return;
        const float w = (float)b.getWidth(), h = (float)b.getHeight();
        float cx = (float)b.getX() + freqToX(bands[draggingBand].freq, w);
        float cy = (float)b.getY() + gainToY(bands[draggingBand].gainDb, h);

        const float f = bands[draggingBand].freq;
        juce::String freqStr = f >= 1000.0f
            ? juce::String(f / 1000.0f, 1) + "kHz"
            : juce::String((int)f) + "Hz";
        juce::String gainStr = (bands[draggingBand].gainDb >= 0 ? "+" : "")
                             + juce::String(bands[draggingBand].gainDb, 1) + "dB";
        juce::String modeStr = filterModeName(filterModes[draggingBand]);
        juce::String text = freqStr + " / " + gainStr + " [" + modeStr + "]";

        float tx = cx + 10.0f;
        float ty = cy - 18.0f;
        if (tx + 130.0f > (float)b.getRight())  tx = cx - 140.0f;
        if (ty < (float)b.getY())               ty = cy + 5.0f;

        g.setColour(juce::Colour(0xDD000000));
        g.fillRoundedRectangle(tx - 2.0f, ty - 2.0f, 132.0f, 16.0f, 3.0f);
        g.setColour(juce::Colour(0xFFCECECE));
        g.setFont(10.0f);
        g.drawText(text, (int)tx, (int)ty, 130, 12, juce::Justification::left);
    }
};


// ===========================================================================
// WaveformPeakBin and kWaveformPeakBins are defined in WaveformPeakBin.h
// (included above) so PadAudioEngine can also include it without a circular dep.
// ===========================================================================

class SampleCard : public juce::Component
{
public:
    SampleCard(juce::AudioFormatManager& formatManager)
        : formatManager(formatManager)
    {
        // Top row buttons - font size 14px to match pitch controls
        addButton.setButtonText("+");
        addButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        addButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        addAndMakeVisible(addButton);
        
        // Configure Prev button (top right)
        prevButton.setButtonText("Prev");
        prevButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        prevButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        addAndMakeVisible(prevButton);
        
        // Configure Next button (top right)
        nextButton.setButtonText("Next");
        nextButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        nextButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        addAndMakeVisible(nextButton);
        
        // Learn button - font size 14px
        learnButton.setButtonText("Learn");
        learnButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        learnButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        learnButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFE25A00));
        learnButton.onClick = [this] { toggleLearnMode(); };
        addAndMakeVisible(learnButton);
        
        // Configure waveform area with viewport
        waveformComponent = std::make_unique<WaveformComponent>(
            formatManager, currentAudioFile, pitchOffset);
        
        // Create a container for the waveform that can be larger than the viewport
        waveformContainer = std::make_unique<juce::Component>();
        waveformContainer->addAndMakeVisible(waveformComponent.get());
        
        // Set up the viewport
        waveformViewport.setViewedComponent(waveformContainer.get(), false);
        waveformViewport.setScrollBarsShown(true, false); // Show vertical scroll bar? false, show horizontal? true
        waveformViewport.setScrollOnDragEnabled(false); // Disabled so waveform can capture mouse for marker drag
        addAndMakeVisible(waveformViewport);

        // Keep zoom indicator aligned with scroll position; save scroll state on every change
        waveformViewport.onScrollChanged = [this](int scrollX)
        {
            if (waveformComponent != nullptr)
                waveformComponent->setScrollOffset(scrollX);
            if (onZoomStateChanged)
                onZoomStateChanged(waveformZoomLevel, getScrollPositionNormalized());
        };

        // Empty pad overlay — shown when no sample is loaded on this pad
        emptyStateLabel.setText("No sample loaded\nUse Prev / Next to browse",
                                 juce::dontSendNotification);
        emptyStateLabel.setJustificationType(juce::Justification::centred);
        emptyStateLabel.setColour(juce::Label::textColourId,       juce::Colour(0xFF888888));
        emptyStateLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF2A2A2A));
        emptyStateLabel.setFont(juce::Font(14.0f));
        emptyStateLabel.setVisible(false);
        addAndMakeVisible(emptyStateLabel);

        // Set up fixed info labels
        topInfoLabel.setJustificationType(juce::Justification::centred);
        topInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFCECECE));
        topInfoLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(topInfoLabel);
        
        bottomInfoLabel.setJustificationType(juce::Justification::centred);
        bottomInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD4A017));
        bottomInfoLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(bottomInfoLabel);
        
        // MIDI Note display - font size 14px (reduced from 16px)
        midiNoteLabel.setJustificationType(juce::Justification::centred);
        midiNoteLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFE25A00));
        midiNoteLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF363636));
        updateMidiNoteDisplay();
        addAndMakeVisible(midiNoteLabel);
        
        // MIDI Channel controls - font size 14px
        channelDownButton.setButtonText("-");
        channelDownButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelDownButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        channelDownButton.onClick = [this] { adjustMidiChannel(-1); };
        channelDownButton.setTooltip("Previous MIDI channel");
        addAndMakeVisible(channelDownButton);
        
        midiChannelLabel.setJustificationType(juce::Justification::centred);
        midiChannelLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9DC95C));
        midiChannelLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF363636));
        updateMidiChannelDisplay();
        addAndMakeVisible(midiChannelLabel);
        
        channelUpButton.setButtonText("+");
        channelUpButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelUpButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        channelUpButton.onClick = [this] { adjustMidiChannel(1); };
        channelUpButton.setTooltip("Next MIDI channel");
        addAndMakeVisible(channelUpButton);
        
        // Pitch adjustment controls - keep font size 14px (already correct)
        pitchDownButton.setButtonText("Down");
        pitchDownButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        pitchDownButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        // onClick fires on mouseUp — only handle if mouseDown didn't already fire the step.
        pitchDownButton.onClick = [this] { if (!pitchRepeatTimer.suppressNextClick()) adjustPitchDown(); };
        pitchDownButton.setTooltip("Lower pitch (longer duration) — hold for continuous change");
        addAndMakeVisible(pitchDownButton);
        
        pitchLabel.setJustificationType(juce::Justification::centred);
        pitchLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF8CBCDC));
        pitchLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF363636));
        pitchLabel.setTooltip("Drag up/down to change pitch (5px = 1 semitone)");
        updatePitchDisplay(pitchOffset);
        addAndMakeVisible(pitchLabel);

        // Wire drag callbacks — run the same logic as adjustPitchUp/Down
        pitchLabel.getCurrentPitch = [this] { return pitchOffset; };
        pitchLabel.getPitchStep    = [this] { return currentPitchStepCents; };
        pitchLabel.onPitchDragged  = [this](int newPitch)
        {
            if (newPitch != pitchOffset)
            {
                pitchOffset = newPitch;
                updatePitchDisplay(pitchOffset);
                listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });
            }
        };
        pitchLabel.onDragFinished  = [this]
        {
            // pitchOffsetChanged already triggers saveCurrentSampleState in MainComponent,
            // but fire it once more on release to ensure the final value is saved.
            listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });
        };
        
        pitchUpButton.setButtonText("Up");
        pitchUpButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        pitchUpButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        // onClick fires on mouseUp — only handle if mouseDown didn't already fire the step.
        pitchUpButton.onClick = [this] { if (!pitchRepeatTimer.suppressNextClick()) adjustPitchUp(); };
        pitchUpButton.setTooltip("Higher pitch (shorter duration) — hold for continuous change");
        addAndMakeVisible(pitchUpButton);

        // Wire hold-to-repeat: mouseDown fires first step instantly; timer handles subsequent steps.
        pitchRepeatTimer.onUp   = [this] { adjustPitchUp(); };
        pitchRepeatTimer.onDown = [this] { adjustPitchDown(); };
        pitchRepeatTimer.attachToButtons(pitchUpButton, pitchDownButton);

        // Pitch step cycling button — cycles through step sizes: 100¢ / 50¢ / 25¢ / 33¢
        pitchStepButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFFD4A017));
        pitchStepButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF111111));
        pitchStepButton.setTooltip("Pitch step size: click to cycle (100=semitone, 50=quarter-tone, 25=eighth-tone, 33=third-tone)");
        pitchStepButton.onClick = [this]
        {
            static const int stepValues[] = { 100, 50, 25, 33 };
            static constexpr int numStepValues = 4;
            int currentIdx = 0;
            for (int i = 0; i < numStepValues; ++i)
                if (stepValues[i] == currentPitchStepCents) { currentIdx = i; break; }
            currentPitchStepCents = stepValues[(currentIdx + 1) % numStepValues];
            updatePitchStepButton();
            updatePitchDisplay(pitchOffset);
            listeners.call([this](Listener& l) { l.pitchStepCentsChanged(currentPitchStepCents); });
        };
        updatePitchStepButton();
        addAndMakeVisible(pitchStepButton);

        pitchStepLabel.setText("Step", juce::dontSendNotification);
        pitchStepLabel.setJustificationType(juce::Justification::centred);
        pitchStepLabel.setFont(juce::Font(10.0f));
        pitchStepLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD4A017));
        addAndMakeVisible(pitchStepLabel);

        stepDescriptionLabel.setText("Western semitone", juce::dontSendNotification);
        stepDescriptionLabel.setJustificationType(juce::Justification::centredRight);
        stepDescriptionLabel.setFont(juce::Font(11.0f));
        stepDescriptionLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(stepDescriptionLabel);

        // Volume knob — neutral gray, same compact style as Master Vol
        volumeKnob.setLookAndFeel(&compactKnobLaf);
        volumeKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        volumeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volumeKnob.setRange(0.0, 1.0, 0.01);
        volumeKnob.setValue(1.0, juce::dontSendNotification);
        volumeKnob.setTooltip("Volume (drag up/down)");
        volumeKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFCECECE)); // neutral gray fill
        volumeKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        volumeKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFF1E1E1E)); // dark indicator on gray
        volumeKnob.setDoubleClickReturnValue(true, 1.0); // double-click resets to 100%
        volumeKnob.onValueChange = [this] {
            float v = (float)volumeKnob.getValue();
            volValueLabel.setText(juce::String(juce::roundToInt(v * 100)) + "%",
                                  juce::dontSendNotification);
            listeners.call([this, v](Listener& l) { l.volumeChanged(v); });
        };
        volumeKnob.onDragStart = [this] { isVolKnobDragging = true; };
        volumeKnob.onDragEnd   = [this] { isVolKnobDragging = false; };
        volumeKnob.addMouseListener(this, false); // SampleCard::mouseDoubleClick flashes volValueLabel
        addAndMakeVisible(volumeKnob);

        volumeLabel.setText("Vol", juce::dontSendNotification);
        volumeLabel.setJustificationType(juce::Justification::centredRight);
        volumeLabel.setFont(juce::Font(11.0f));
        volumeLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(volumeLabel);

        volValueLabel.setText("100%", juce::dontSendNotification);
        volValueLabel.setJustificationType(juce::Justification::centredLeft);
        volValueLabel.setFont(juce::Font(10.0f));
        volValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(volValueLabel);

        // Start point knob — deep red matching the start marker (#CC0000), white indicator
        startKnob.setLookAndFeel(&compactKnobLaf);
        startKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        startKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        startKnob.setRange(0.0, 1.0, 0.001);
        startKnob.setValue(0.0, juce::dontSendNotification);
        startKnob.setTooltip("Start point (drag to set sample start)");
        startKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFCC0000)); // matches start marker
        startKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        startKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFFFFF)); // white indicator on red
        startKnob.onValueChange = [this] {
            double seconds = startKnob.getValue() * originalDuration;
            seconds = applyGridSnap(seconds, true);
            float norm = (float)juce::jlimit(0.0, 1.0,
                originalDuration > 0.0 ? seconds / originalDuration : startKnob.getValue());
            startPointNormalized = norm;
            startKnob.setValue(norm, juce::dontSendNotification);
            if (waveformComponent != nullptr)
                waveformComponent->setStartMarker(norm);
            notifyStartPointChanged();
        };
        addAndMakeVisible(startKnob);

        startKnobLabel.setText("Start", juce::dontSendNotification);
        startKnobLabel.setJustificationType(juce::Justification::centredRight);
        startKnobLabel.setFont(juce::Font(11.0f));
        startKnobLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(startKnobLabel);

        // Wire start marker drag → knob + listeners
        waveformComponent->onMarkerDragged = [this](float newNorm) {
            if (gridSnapEnabled && originalDuration > 0.0)
            {
                double seconds = newNorm * originalDuration;
                seconds = applyGridSnap(seconds, true);
                newNorm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
            }
            startPointNormalized = newNorm;
            startKnob.setValue(newNorm, juce::dontSendNotification);
            notifyStartPointChanged();
        };

        // End point knob — cyan matching the end marker (#00AACC), white indicator
        endKnob.setLookAndFeel(&compactKnobLaf);
        endKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        endKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        endKnob.setRange(0.0, 1.0, 0.001);
        endKnob.setValue(1.0, juce::dontSendNotification);
        endKnob.setTooltip("End point (drag to set sample end)");
        endKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFF00AACC)); // matches end marker
        endKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        endKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFFFFF)); // white indicator on cyan
        endKnob.onValueChange = [this] {
            double seconds = endKnob.getValue() * originalDuration;
            seconds = applyGridSnap(seconds, false);
            float norm = (float)juce::jlimit(0.0, 1.0,
                originalDuration > 0.0 ? seconds / originalDuration : endKnob.getValue());
            endPointNormalized = norm;
            endKnob.setValue(norm, juce::dontSendNotification);
            if (waveformComponent != nullptr)
                waveformComponent->setEndMarker(norm);
            notifyEndPointChanged();
        };
        addAndMakeVisible(endKnob);

        endKnobLabel.setText("End", juce::dontSendNotification);
        endKnobLabel.setJustificationType(juce::Justification::centredRight);
        endKnobLabel.setFont(juce::Font(11.0f));
        endKnobLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(endKnobLabel);

        // Wire end marker drag → knob + listeners
        waveformComponent->onEndMarkerDragged = [this](float newNorm) {
            if (gridSnapEnabled && originalDuration > 0.0)
            {
                double seconds = newNorm * originalDuration;
                seconds = applyGridSnap(seconds, false);
                newNorm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
            }
            endPointNormalized = newNorm;
            endKnob.setValue(newNorm, juce::dontSendNotification);
            notifyEndPointChanged();
        };

        // Zoom: mouse wheel → zoom centered on cursor position
        waveformComponent->onZoomChanged = [this](double factor, int mouseXInComponent)
        {
            const int mouseXInContainer = mouseXInComponent + 2; // waveformComponent is at x=2 in container
            const int viewScrollX       = waveformViewport.getViewPositionX();
            const int oldContainerWidth = waveformContainer->getWidth();

            waveformZoomLevel = juce::jlimit(1.0, 100.0, waveformZoomLevel * factor);
            updateWaveformSize(true); // preserve scroll — we set it below

            const int newContainerWidth = waveformContainer->getWidth();
            const int viewportW         = waveformViewport.getWidth();
            if (oldContainerWidth > 0)
            {
                double ratio   = (double)mouseXInContainer / (double)oldContainerWidth;
                int newAbsX    = (int)(ratio * newContainerWidth);
                int screenX    = mouseXInContainer - viewScrollX; // cursor relative to viewport left
                int newScrollX = newAbsX - screenX;
                newScrollX = juce::jlimit(0, juce::jmax(0, newContainerWidth - viewportW), newScrollX);
                waveformViewport.setViewPosition(newScrollX, 0);
            }
            if (onZoomStateChanged)
                onZoomStateChanged(waveformZoomLevel, getScrollPositionNormalized());
        };

        // Zoom reset: double-click on waveform area
        waveformComponent->onZoomReset = [this]()
        {
            waveformZoomLevel = 1.0;
            updateWaveformSize(false); // also resets scroll to 0
            if (onZoomStateChanged)
                onZoomStateChanged(1.0, 0.0f);
        };

        // Loop toggle button — left of Start knob in pitch row
        loopButton.setButtonText("Loop");
        loopButton.setClickingTogglesState(true);
        loopButton.setToggleState(false, juce::dontSendNotification);
        loopButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
        loopButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF00FF88));
        loopButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        loopButton.setColour(juce::TextButton::textColourOnId,  juce::Colour(0xFF111111));
        loopButton.onClick = [this] {
            bool isOn = loopButton.getToggleState();
            if (waveformComponent != nullptr)
                waveformComponent->setLoopHighlight(isOn);
            // If Loop is turned OFF while Bounce is ON, turn Bounce OFF too.
            if (!isOn && bounceEnabledState) {
                bounceEnabledState = false;
                bounceButton.setToggleState(false, juce::dontSendNotification);
                listeners.call([](Listener& l) { l.bounceEnabledChanged(false); });
            }
            listeners.call([isOn](Listener& l) { l.loopEnabledChanged(isOn); });
        };
        addAndMakeVisible(loopButton);

        // Freeze button — left of Loop button in pitch row
        // Manual toggle state (setClickingTogglesState false) — we manage isFreezeActive ourselves
        // so the double-tap can override the toggle without any JUCE state fighting us.
        freezeButton.setButtonText("Freeze");
        freezeButton.setClickingTogglesState(false);
        applyFreezeButtonStyle(false);
        // FIX 4: instant visual on mouseDown — button lights up/dims before any audio work.
        freezeButton.addMouseListener(this, false);
        freezeButton.onClick = [this]
        {
            const juce::int64 now = static_cast<juce::int64>(juce::Time::getMillisecondCounter());
            if (lastFreezeTapMs != 0 && (now - lastFreezeTapMs) < 400)
            {
                // Double tap — panic reset: turn off both freeze and loop
                lastFreezeTapMs = 0;
                const bool wasFreeze = isFreezeActive;
                isFreezeActive = false;
                loopWasOnBeforeFreeze = false;

                // Flash white, then restore OFF style after 200 ms
                freezeButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFFFFFFFF));
                freezeButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF000000));
                juce::Timer::callAfterDelay(200, [this] { applyFreezeButtonStyle(false); });

                // Turn loop off
                setLoopEnabled(false);
                listeners.call([](Listener& l) { l.loopEnabledChanged(false); });

                // Notify freeze off (only if it was actually on — avoids redundant allNotesOff)
                if (wasFreeze)
                    listeners.call([](Listener& l) { l.freezeChanged(false); });
            }
            else
            {
                lastFreezeTapMs = now;
                toggleFreeze();
            }
        };
        addAndMakeVisible(freezeButton);

        // One Shot button — plays sample fully through regardless of note duration
        oneShotButton.setClickingTogglesState(true);
        oneShotButton.setToggleState(false, juce::dontSendNotification);
        oneShotButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        oneShotButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFFFB300)); // warm amber
        oneShotButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        oneShotButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFF111111));
        oneShotButton.setTooltip("One Shot: sample plays fully through regardless of note length. Overrides Loop.");
        oneShotButton.onClick = [this] {
            oneShotEnabled = oneShotButton.getToggleState();
            listeners.call([this](Listener& l) { l.oneShotEnabledChanged(oneShotEnabled); });
        };
        addAndMakeVisible(oneShotButton);

        // Reverse playback button — plays sample backwards from End to Start
        reverseButton.setClickingTogglesState(true);
        reverseButton.setToggleState(false, juce::dontSendNotification);
        reverseButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        reverseButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFFF6600)); // bright orange
        reverseButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        reverseButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFF111111));
        reverseButton.setTooltip("Reverse: play sample backwards from End to Start");
        reverseButton.onClick = [this] {
            reverseEnabledState = reverseButton.getToggleState();
            // Mutually exclusive with Bounce: turning Rev ON turns Bnc OFF.
            if (reverseEnabledState && bounceEnabledState) {
                bounceEnabledState = false;
                bounceButton.setToggleState(false, juce::dontSendNotification);
                listeners.call([](Listener& l) { l.bounceEnabledChanged(false); });
            }
            listeners.call([this](Listener& l) { l.reverseEnabledChanged(reverseEnabledState); });
        };
        addAndMakeVisible(reverseButton);

        // Bounce (ping-pong) playback button — mutually exclusive with Rev
        bounceButton.setClickingTogglesState(true);
        bounceButton.setToggleState(false, juce::dontSendNotification);
        bounceButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        bounceButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFCC44FF)); // bright purple
        bounceButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        bounceButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFF111111));
        bounceButton.setTooltip("Bounce: ping-pong loop between Start and End markers (requires Loop)");
        bounceButton.onClick = [this] {
            bounceEnabledState = bounceButton.getToggleState();
            if (bounceEnabledState) {
                // Mutually exclusive with Rev: turning Bnc ON turns Rev OFF.
                if (reverseEnabledState) {
                    reverseEnabledState = false;
                    reverseButton.setToggleState(false, juce::dontSendNotification);
                    listeners.call([](Listener& l) { l.reverseEnabledChanged(false); });
                }
                // Bounce requires Loop: auto-enable Loop if it isn't already on.
                if (!loopButton.getToggleState()) {
                    loopButton.setToggleState(true, juce::dontSendNotification);
                    if (waveformComponent != nullptr)
                        waveformComponent->setLoopHighlight(true);
                    listeners.call([](Listener& l) { l.loopEnabledChanged(true); });
                }
            }
            listeners.call([this](Listener& l) { l.bounceEnabledChanged(bounceEnabledState); });
        };
        addAndMakeVisible(bounceButton);

        // Trim button — orange-red to indicate a new-file-creating (irreversible) action
        trimButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFFE84A1A));
        trimButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        trimButton.setTooltip("Write a new WAV file containing only the audio between the Start and End markers");
        trimButton.onClick = [this] {
            if (onTrimRequested)
                onTrimRequested();
        };
        addAndMakeVisible(trimButton);

        // Toast label — overlays footer with brief confirmation/error messages (auto-hides after 2s)
        toastLabel.setJustificationType(juce::Justification::centred);
        toastLabel.setFont(juce::Font(11.0f));
        toastLabel.setColour(juce::Label::textColourId,       juce::Colour(0xFFFFFFFF));
        toastLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        toastLabel.setVisible(false);
        addAndMakeVisible(toastLabel);

        // Grid snap toggle — snaps markers to nearest grid division
        gridSnapButton.setClickingTogglesState(true);
        gridSnapButton.setToggleState(false, juce::dontSendNotification);
        gridSnapButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        gridSnapButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFAAFF00));
        gridSnapButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        gridSnapButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFF111111));
        gridSnapButton.setTooltip("Snap markers to grid time divisions");
        gridSnapButton.onClick = [this]
        {
            gridSnapEnabled = gridSnapButton.getToggleState();
            updateGridResolutionButtonState();
            if (waveformComponent != nullptr)
                waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
            listeners.call([this](Listener& l) { l.gridSnapChanged(gridSnapEnabled); });
        };
        addAndMakeVisible(gridSnapButton);

        // Grid resolution cycling button — to the right of Grid button
        updateGridResolutionButton();  // sets text + colors based on initial state
        gridResolutionButton.setTooltip("Grid resolution — click to cycle: 1ms \xe2\x86\x92 10ms \xe2\x86\x92 50ms \xe2\x86\x92 100ms \xe2\x86\x92 500ms \xe2\x86\x92 1s");
        gridResolutionButton.onClick = [this]
        {
            gridResolutionIndex = (gridResolutionIndex + 1) % numGridResolutions;
            updateGridResolutionButton();
            if (waveformComponent != nullptr)
                waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
            listeners.call([this](Listener& l) { l.gridResolutionChanged(gridResolutionIndex); });
        };
        addAndMakeVisible(gridResolutionButton);

        // Tune button — auto pitch detection
        tuneButton.setButtonText("Tune");
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        tuneButton.setTooltip("Detect fundamental pitch and auto-tune (analyzes Start-End region)");
        tuneButton.onClick = [this] { startTuneAnalysis(); };
        addAndMakeVisible(tuneButton);

        // Transient detection toggle — enables/disables the whole transient subsystem
        detectionToggleButton.setClickingTogglesState(true);
        detectionToggleButton.setToggleState(true, juce::dontSendNotification);
        detectionToggleButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF8B2500)); // dark red = active
        detectionToggleButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF8B2500));
        detectionToggleButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFFFFFFF));
        detectionToggleButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFFFFFFFF));
        detectionToggleButton.setTooltip("Enable / disable transient detection");
        detectionToggleButton.onClick = [this]
        {
            transientDetectionEnabled = detectionToggleButton.getToggleState();
            detectionToggleButton.setColour(juce::TextButton::buttonColourId,
                transientDetectionEnabled ? juce::Colour(0xFF8B2500) : juce::Colour(0xFF4A4A4A));
            detectionToggleButton.setColour(juce::TextButton::textColourOffId,
                transientDetectionEnabled ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF7A7A7A));
            updateTransientControlsState();
            // FIX 3: notify MainComponent so it can save to disk
            listeners.call([this](Listener& l) { l.transientDetectionEnabledChanged(transientDetectionEnabled); });
        };
        addAndMakeVisible(detectionToggleButton);

        // Transient snap buttons — snap start marker to nearest transient (orange-red)
        prevTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
        prevTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        prevTransientButton.setTooltip("Snap start to previous transient");
        prevTransientButton.onClick = [this] { snapToPrevTransient(); };
        addAndMakeVisible(prevTransientButton);

        nextTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
        nextTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        nextTransientButton.setTooltip("Snap start to next transient");
        nextTransientButton.onClick = [this] { snapToNextTransient(); };
        addAndMakeVisible(nextTransientButton);

        // End marker transient snap buttons (cyan-blue)
        prevEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
        prevEndTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        prevEndTransientButton.setTooltip("Snap end to previous transient");
        prevEndTransientButton.onClick = [this] { snapEndToPrevTransient(); };
        addAndMakeVisible(prevEndTransientButton);

        nextEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
        nextEndTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        nextEndTransientButton.setTooltip("Snap end to next transient");
        nextEndTransientButton.onClick = [this] { snapEndToNextTransient(); };
        addAndMakeVisible(nextEndTransientButton);

        // Sensitivity knob — orange accent, controls transient detection threshold
        sensKnob.setLookAndFeel(&compactKnobLaf);
        sensKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        sensKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        sensKnob.setRange(1.5, 10.0, 0.1);
        sensKnob.setValue(4.0, juce::dontSendNotification);
        sensKnob.setTooltip("Transient sensitivity: low=many transients, high=only strong hits");
        sensKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFF8B2500)); // dark red matching CRA
        sensKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        sensKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFFFFF));
        sensKnob.onValueChange = [this] {
            transientThreshold = sensKnob.getValue();
            sensValueLabel.setText(juce::String(transientThreshold, 1) + "x", juce::dontSendNotification);
            if (currentAudioFile.existsAsFile())
                detectTransients(currentAudioFile);
        };
        addAndMakeVisible(sensKnob);

        sensLabel.setText("Sens", juce::dontSendNotification);
        sensLabel.setJustificationType(juce::Justification::centredRight);
        sensLabel.setFont(juce::Font(11.0f));
        sensLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(sensLabel);

        sensValueLabel.setText("4.0x", juce::dontSendNotification);
        sensValueLabel.setJustificationType(juce::Justification::centredLeft);
        sensValueLabel.setFont(juce::Font(10.0f));
        sensValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(sensValueLabel);

        transientCountLabel.setText("T: 0", juce::dontSendNotification);
        transientCountLabel.setJustificationType(juce::Justification::centred);
        transientCountLabel.setFont(juce::Font(10.0f));
        transientCountLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(transientCountLabel);

        // ===== TAB BAR =====
        auto setupTab = [](juce::TextButton& btn, bool active)
        {
            btn.setColour(juce::TextButton::buttonColourId,  active ? juce::Colour(0xFF555555) : juce::Colour(0xFF333333));
            btn.setColour(juce::TextButton::textColourOffId, active ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF888888));
        };
        setupTab(controlsTabButton, true);
        controlsTabButton.onClick = [this] { setActiveTab(0); };
        addAndMakeVisible(controlsTabButton);

        setupTab(adsrTabButton, false);
        adsrTabButton.onClick = [this] { setActiveTab(1); };
        addAndMakeVisible(adsrTabButton);

        setupTab(eqTabButton, false);
        eqTabButton.onClick = [this] { setActiveTab(2); };
        addAndMakeVisible(eqTabButton);

        eqPlaceholderLabel.setText("Coming soon", juce::dontSendNotification);
        eqPlaceholderLabel.setJustificationType(juce::Justification::centred);
        eqPlaceholderLabel.setFont(juce::Font(16.0f, juce::Font::italic));
        eqPlaceholderLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF888888));
        addAndMakeVisible(eqPlaceholderLabel);
        eqPlaceholderLabel.setVisible(false);

        // ===== ADSR CONTROLS =====
        adsrEnableButton.setClickingTogglesState(true);
        adsrEnableButton.setToggleState(false, juce::dontSendNotification);
        adsrEnableButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        adsrEnableButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFCC00A0)); // fuchsia ON
        adsrEnableButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        adsrEnableButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFFFFFFFF));
        adsrEnableButton.setTooltip("Enable / disable ADSR envelope shaping");
        adsrEnableButton.onClick = [this]
        {
            adsrEnabled = adsrEnableButton.getToggleState();
            updateAdsrControlsState();
            if (waveformComponent != nullptr)
                waveformComponent->setAdsrOverlay(adsrEnabled, adsrAttackMs, adsrDecayMs, adsrSustain, adsrReleaseMs);
            listeners.call([this](Listener& l) { l.adsrParamsChanged(adsrEnabled, adsrAttackMs, adsrDecayMs, adsrSustain, adsrReleaseMs); });
        };
        addAndMakeVisible(adsrEnableButton);
        adsrEnableButton.setVisible(false);

        // ADSR knob helper lambda
        auto initAdsrKnob = [this](juce::Slider& knob, double rangeMin, double rangeMax, double defaultVal,
                                   const char* tooltip)
        {
            knob.setLookAndFeel(&compactKnobLaf);
            knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            knob.setRange(rangeMin, rangeMax, 1.0);
            knob.setValue(defaultVal, juce::dontSendNotification);
            knob.setColour(juce::Slider::rotarySliderFillColourId,    juce::Colour(0xFFBB0090)); // fuchsia knob
            knob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
            knob.setColour(juce::Slider::thumbColourId,               juce::Colour(0xFFFFFFFF));
            knob.setTooltip(tooltip);
            addAndMakeVisible(knob);
            knob.setVisible(false);
        };
        auto initAdsrLabel = [this](juce::Label& lbl, const char* text)
        {
            lbl.setText(text, juce::dontSendNotification);
            lbl.setJustificationType(juce::Justification::centred);
            lbl.setFont(juce::Font(11.0f));
            lbl.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
            addAndMakeVisible(lbl);
            lbl.setVisible(false);
        };
        auto initAdsrValueLabel = [this](juce::Label& lbl, const char* text)
        {
            lbl.setText(text, juce::dontSendNotification);
            lbl.setJustificationType(juce::Justification::centred);
            lbl.setFont(juce::Font(10.0f));
            lbl.setColour(juce::Label::textColourId, juce::Colour(0xFFCECECE));
            addAndMakeVisible(lbl);
            lbl.setVisible(false);
        };

        initAdsrKnob(adsrAtkKnob, 0.0, 500.0, 0.0,  "Attack (0-500ms)  |  Shift = fine  |  DblClick = reset");
        initAdsrKnob(adsrDcyKnob, 0.0, 500.0, 0.0,  "Decay (0-500ms)  |  Shift = fine  |  DblClick = reset");
        initAdsrKnob(adsrSusKnob, 0.0, 1.0,   1.0,  "Sustain (0-100%)  |  Shift = fine  |  DblClick = reset");
        initAdsrKnob(adsrRelKnob, 0.0, 2000.0, 0.0, "Release (0-2000ms)  |  Shift = fine  |  DblClick = reset");
        // Sus knob — fine resolution for smooth drag
        adsrSusKnob.setRange(0.0, 1.0, 0.001);

        // Per-knob drag sensitivity: units per pixel (normal drag).
        // Shift key divides by 10 for ultra-fine control.
        // Atk/Dcy: 1 px = 1 ms  → 500 px for full 500 ms range
        // Sus:     1 px = 0.2%  → 500 px for full 0–100% range
        // Rel:     1 px = 2 ms  → 1000 px for full 2000 ms range
        adsrAtkKnob.sensitivityNormal = 1.0;    adsrAtkKnob.defaultValue = 0.0;
        adsrDcyKnob.sensitivityNormal = 1.0;    adsrDcyKnob.defaultValue = 0.0;
        adsrSusKnob.sensitivityNormal = 0.002;  adsrSusKnob.defaultValue = 1.0;
        adsrRelKnob.sensitivityNormal = 2.0;    adsrRelKnob.defaultValue = 0.0;

        initAdsrLabel(adsrAtkLabel, "Atk");
        initAdsrLabel(adsrDcyLabel, "Dcy");
        initAdsrLabel(adsrSusLabel, "Sus");
        initAdsrLabel(adsrRelLabel, "Rel");

        initAdsrValueLabel(adsrAtkValueLabel, "0ms");
        initAdsrValueLabel(adsrDcyValueLabel, "0ms");
        initAdsrValueLabel(adsrSusValueLabel, "100%");
        initAdsrValueLabel(adsrRelValueLabel, "0ms");

        adsrAtkKnob.onValueChange = [this]
        {
            adsrAttackMs = (float)adsrAtkKnob.getValue();
            updateAdsrValueDisplays();
        };
        adsrDcyKnob.onValueChange = [this]
        {
            adsrDecayMs = (float)adsrDcyKnob.getValue();
            updateAdsrValueDisplays();
        };
        adsrSusKnob.onValueChange = [this]
        {
            adsrSustain = (float)adsrSusKnob.getValue();
            updateAdsrValueDisplays();
        };
        adsrRelKnob.onValueChange = [this]
        {
            adsrReleaseMs = (float)adsrRelKnob.getValue();
            updateAdsrValueDisplays();
        };

        // Drag start/end callbacks — set isAdsrKnobDragging flag so MainComponent
        // defers disk save during drag (one write fires 400ms after drag ends).
        // Note: overlay repaints in real time on every value change (no suppression).
        auto wireAdsrDrag = [this](juce::Slider& knob)
        {
            knob.onDragStart = [this] { isAdsrKnobDragging = true; };
            knob.onDragEnd   = [this] { isAdsrKnobDragging = false; };
        };
        wireAdsrDrag(adsrAtkKnob);
        wireAdsrDrag(adsrDcyKnob);
        wireAdsrDrag(adsrSusKnob);
        wireAdsrDrag(adsrRelKnob);

        updateAdsrControlsState();

        // Update pitch button labels with tooltips
        updatePitchButtonLabels();
        
        // Configure sample name label (bottom left)
        sampleNameLabel.setJustificationType(juce::Justification::centredLeft);
        sampleNameLabel.setFont(juce::Font(11.0f, juce::Font::bold));  // Matches pitch indicator
        sampleNameLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF8CBCDC));
        addAndMakeVisible(sampleNameLabel);

        // Configure duration label (bottom right)
        durationLabel.setJustificationType(juce::Justification::centredRight);
        durationLabel.setFont(juce::Font(11.0f, juce::Font::bold));  // Matches other indicators
        durationLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9DC95C));
        addAndMakeVisible(durationLabel);

        // ===== EQ CONTROLS =====
        eqEnableButton.setClickingTogglesState(true);
        eqEnableButton.setToggleState(false, juce::dontSendNotification);
        eqEnableButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        eqEnableButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF00CFFF)); // ice-blue ON
        eqEnableButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        eqEnableButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFF111111));
        eqEnableButton.setTooltip("Enable / bypass the 3-band parametric EQ");
        eqEnableButton.onClick = [this]
        {
            eqEnabled = eqEnableButton.getToggleState();
            if (eqDisplay != nullptr) eqDisplay->setEqEnabled(eqEnabled);
            fireEqParamsChanged();
        };
        addAndMakeVisible(eqEnableButton);
        eqEnableButton.setVisible(false);

        // EQ Display — spectrum + curve + draggable bands
        eqDisplay = std::make_unique<EQDisplay>();
        eqDisplay->setSampleRate(44100.0);
        eqDisplay->setBands(eqBands);
        eqDisplay->onBandChanged = [this](int idx, float freq, float gainDb, float q)
        {
            eqBands[idx].freq   = freq;
            eqBands[idx].gainDb = gainDb;
            eqBands[idx].q      = q;
            eqDisplay->setBand(idx, eqBands[idx]);
            fireEqParamsChanged();
        };

        // Update the filter mode button text when the user clicks a different band.
        eqDisplay->onActiveBandChanged = [this](int bandIdx)
        {
            if (bandIdx >= 0 && bandIdx < 3)
                filterModeButton.setButtonText(EQDisplay::filterModeName(eqFilterModes[bandIdx]));
        };

        addAndMakeVisible(*eqDisplay);
        eqDisplay->setVisible(false);

        // Filter mode selector — cycles through 6 filter types for the active EQ band
        filterModeButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF333355));
        filterModeButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        filterModeButton.onClick = [this]
        {
            const int band = (eqDisplay != nullptr) ? eqDisplay->getActiveBand() : -1;
            if (band < 0) return;  // no band selected yet

            // Cycle mode: 0 LowCut → 1 LowShelf → 2 Bell → 3 Notch → 4 HighShelf → 5 HighCut → 0
            eqFilterModes[band] = (eqFilterModes[band] + 1) % 6;
            eqDisplay->setFilterMode(band, eqFilterModes[band]);
            filterModeButton.setButtonText(EQDisplay::filterModeName(eqFilterModes[band]));

            // Notify listeners so MainComponent recomputes audio coefficients and saves state.
            listeners.call([this](Listener& l) {
                l.eqFilterModesChanged(eqFilterModes[0], eqFilterModes[1], eqFilterModes[2]);
            });
        };
        addAndMakeVisible(filterModeButton);
        filterModeButton.setVisible(false);

        // EQ Reset button — resets all bands to flat defaults, leaves EQ on/off and Norm unchanged.
        eqResetButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
        eqResetButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        eqResetButton.setTooltip("Reset all EQ bands to flat defaults (100/500/8000 Hz, 0dB, Bell)");
        eqResetButton.onClick = [this]
        {
            const juce::int64 t0 = juce::Time::getMillisecondCounter();
            printf("[RESET-TIMING] Reset button clicked\n");
            fflush(stdout);

            // FIX 5: Visual update is instant — update bands/modes before any audio work.
            eqBands[0] = { 100.0f,  0.0f, 1.0f };
            eqBands[1] = { 500.0f,  0.0f, 1.0f };
            eqBands[2] = { 8000.0f, 0.0f, 1.0f };
            eqFilterModes[0] = eqFilterModes[1] = eqFilterModes[2] = 2; // Bell

            if (eqDisplay != nullptr)
            {
                eqDisplay->setBands(eqBands);
                eqDisplay->setFilterMode(0, 2);
                eqDisplay->setFilterMode(1, 2);
                eqDisplay->setFilterMode(2, 2);
            }
            filterModeButton.setButtonText("Bell");

            const juce::int64 tVis = juce::Time::getMillisecondCounter();
            printf("[RESET-TIMING] visual update: %lldms\n", (long long)(tVis - t0));

            // Update mode tracking in MainComponent (deferred save via filterModeSaveTimer — no disk I/O).
            listeners.call([](Listener& l) { l.eqFilterModesChanged(2, 2, 2); });

            const juce::int64 tModes = juce::Time::getMillisecondCounter();
            printf("[RESET-TIMING] eqFilterModesChanged fired: %lldms\n", (long long)(tModes - t0));

            // FIX 1+2: Write pre-computed default coefficients directly — no calculation, no sync save.
            // MainComponent::onEqReset does: eqCoeffDB.writeFromUI(defaultFlatCoeffs) + defer save.
            if (onEqReset) onEqReset();

            const juce::int64 tDone = juce::Time::getMillisecondCounter();
            printf("[RESET-TIMING] total reset handler time: %lldms\n", (long long)(tDone - t0));
            fflush(stdout);

            // Brief white flash to confirm reset was applied.
            eqResetButton.setColour(juce::TextButton::buttonColourId,  juce::Colours::white);
            eqResetButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
            juce::Timer::callAfterDelay(150, [this]
            {
                eqResetButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
                eqResetButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
            });
        };
        addAndMakeVisible(eqResetButton);
        eqResetButton.setVisible(false);

        // ===== NORMALIZE CONTROLS =====
        // Three mutually-exclusive target dB buttons (default: -6 active)
        auto setupNormTargetBtn = [](juce::TextButton& btn)
        {
            btn.setClickingTogglesState(false);
            btn.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
            btn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        };
        setupNormTargetBtn(normTargetMinus12Button);
        setupNormTargetBtn(normTargetMinus6Button);
        setupNormTargetBtn(normTargetZeroButton);
        normTargetMinus12Button.onClick = [this] { setNormTarget(-12.0f); };
        normTargetMinus6Button.onClick  = [this] { setNormTarget(-6.0f);  };
        normTargetZeroButton.onClick    = [this] { setNormTarget(0.0f);   };
        addAndMakeVisible(normTargetMinus12Button); normTargetMinus12Button.setVisible(false);
        addAndMakeVisible(normTargetMinus6Button);  normTargetMinus6Button.setVisible(false);
        addAndMakeVisible(normTargetZeroButton);    normTargetZeroButton.setVisible(false);
        updateNormTargetButtonColors();  // highlight default (-6)

        normButton.setClickingTogglesState(true);
        normButton.setToggleState(false, juce::dontSendNotification);
        normButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        normButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF00FF88));
        normButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        normButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFF111111));
        normButton.onClick = [this]
        {
            normEnabled = normButton.getToggleState();
            if (!normEnabled) { normAppliedGainDb = 0.0f; updateNormGainLabel(); }
            listeners.call([this](Listener& l) { l.normChanged(normEnabled, normTargetDb); });
        };
        addAndMakeVisible(normButton);
        normButton.setVisible(false);

        normGainLabel.setText("", juce::dontSendNotification);
        normGainLabel.setFont(juce::Font(10.0f));
        normGainLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        normGainLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(normGainLabel);
        normGainLabel.setVisible(false);

        // 60fps playhead animation timer — cheap when idle (position stays at -1, no repaint)
        playheadTimer.startTimer(16);
    }
    
    ~SampleCard() override
    {
        // Stop background tune analysis before destruction
        if (tuneThread != nullptr)
        {
            tuneThread->stopThread(2000);
            tuneThread = nullptr;
        }
        volumeKnob.setLookAndFeel(nullptr);
        startKnob.setLookAndFeel(nullptr);
        endKnob.setLookAndFeel(nullptr);
        sensKnob.setLookAndFeel(nullptr);
        adsrAtkKnob.setLookAndFeel(nullptr);
        adsrDcyKnob.setLookAndFeel(nullptr);
        adsrSusKnob.setLookAndFeel(nullptr);
        adsrRelKnob.setLookAndFeel(nullptr);
    }

    void resetViewport()
    {
        waveformViewport.setViewPosition(0, 0);
    }

    // Deliver pre-computed peak data to WaveformComponent.
    // Call this BEFORE setWaveform() inside the callAsync lambda so paint() has
    // data ready on the very first repaint — no disk I/O on the message thread.
    void setWaveformPeaks(std::unique_ptr<WaveformPeakBin[]> peaks,
                          int numCh, juce::int64 numSamples, double sampleRate)
    {
        if (waveformComponent != nullptr)
            waveformComponent->setAudioPeaks(std::move(peaks), numCh, numSamples, sampleRate);
    }

    // Deliver cached peak data from a PadAudioEngine (for instant pad switching).
    // Unlike setWaveformPeaks(), this takes a raw pointer + size so the engine can
    // keep its own copy while the WaveformComponent gets a deep copy.
    void setAudioPeaksFromBuffer(const WaveformPeakBin* src, int numBins,
                                 int numCh, juce::int64 numSamples, double sampleRate)
    {
        if (waveformComponent == nullptr || src == nullptr || numBins <= 0) return;
        auto copy = std::make_unique<WaveformPeakBin[]>((size_t)numBins);
        std::memcpy(copy.get(), src, sizeof(WaveformPeakBin) * (size_t)numBins);
        waveformComponent->setAudioPeaks(std::move(copy), numCh, numSamples, sampleRate);
    }

    // Update the waveform display for a new file WITHOUT resetting any UI controls
    // (pitch, loop, ADSR, start/end markers, etc.).  Used when switching pads
    // where the audio is already in RAM — no full setWaveform() reset needed.
    // IMPORTANT: call setAudioPeaksFromBuffer() AFTER this so setFile()'s
    // peaksReady.store(false) is overwritten by the subsequent setAudioPeaks().
    void setWaveformFileOnly(const juce::File& file, juce::int64 knownSamples, double knownSR)
    {
        currentAudioFile = file;
        if (waveformComponent != nullptr)
        {
            // setFile() clears peaksReady — caller must call setAudioPeaksFromBuffer() after this.
            waveformComponent->setFile(file);
        }
        // Set duration so markers and knobs render at the correct scale.
        if (knownSamples > 0 && knownSR > 0.0)
            setDuration((double)knownSamples / knownSR);
        sampleNameLabel.setText(file.getFileName(), juce::dontSendNotification);
    }

    // Show "Loading..." in the waveform area immediately on button press — before
    // the background thread finishes reading the file. Cleared by setWaveform().
    void showLoadingState()
    {
        if (waveformComponent != nullptr)
            waveformComponent->setLoading(true);
    }

    // Run transient detection now (called by deferred timer after navigation stops).
    // Safe to call on the message thread — file I/O is proportional to file length.
    void runTransientDetection()
    {
        if (transientDetectionEnabled && currentAudioFile.existsAsFile())
        {
            printf("[NAV-TRANSIENT] Deferred transient detection starting: %s\n",
                   currentAudioFile.getFileName().toRawUTF8());
            detectTransients(currentAudioFile);
        }
    }
    
    void resized() override
    {
        auto area = getLocalBounds();
        // Add margin around the entire card content (10px on each side)
        area.reduce(10, 10);
        
        // ===== TOP ROW: uniform height matches tab buttons (24px bar, 20px buttons) =====
        constexpr int kTopBtnH = 20; // same as kBtnH in Controls tab
        auto topRow = area.removeFromTop(24); // matches tab bar height

        // + button on left (40px wide)
        addButton.setBounds(topRow.removeFromLeft(40).withSizeKeepingCentre(36, kTopBtnH));

        // Leave some space between + button and MIDI controls
        topRow.removeFromLeft(10);

        // Learn button (60px wide)
        learnButton.setBounds(topRow.removeFromLeft(60).withSizeKeepingCentre(56, kTopBtnH));

        // MIDI Note display (100px wide)
        midiNoteLabel.setBounds(topRow.removeFromLeft(100).withSizeKeepingCentre(96, kTopBtnH));

        // Space between note and channel controls
        topRow.removeFromLeft(10);

        // MIDI Channel controls (total 120px: 30 + 60 + 30)
        channelDownButton.setBounds(topRow.removeFromLeft(30).withSizeKeepingCentre(26, kTopBtnH));
        midiChannelLabel.setBounds(topRow.removeFromLeft(60).withSizeKeepingCentre(56, kTopBtnH));
        channelUpButton.setBounds(topRow.removeFromLeft(30).withSizeKeepingCentre(26, kTopBtnH));

        // Gap before Vol knob
        topRow.removeFromLeft(8);

        // Vol knob with left label and right percentage display
        // Layout: [Vol label (28px)] [knob (20x20)] [value% (32px)]
        {
            auto volLabelCol = topRow.removeFromLeft(28);
            volumeLabel.setBounds(volLabelCol.withSizeKeepingCentre(26, kTopBtnH));
            auto volKnobCol = topRow.removeFromLeft(20);
            volumeKnob.setBounds(volKnobCol.withSizeKeepingCentre(kTopBtnH, kTopBtnH));
            auto volValueCol = topRow.removeFromLeft(32);
            volValueLabel.setBounds(volValueCol.withSizeKeepingCentre(30, kTopBtnH));
        }

        // Gap after Vol knob area
        topRow.removeFromLeft(6);

        // Prev/Next buttons on right: Next rightmost, Prev to its left (L→R: Prev | Next)
        nextButton.setBounds(topRow.removeFromRight(60).withSizeKeepingCentre(56, kTopBtnH));
        prevButton.setBounds(topRow.removeFromRight(60).withSizeKeepingCentre(56, kTopBtnH));
        // Step description label: fills remaining space on the right, flush left of Prev
        stepDescriptionLabel.setBounds(topRow.removeFromRight(140).withSizeKeepingCentre(136, kTopBtnH));
        
        // Add 5px margin between top row and waveform
        area.removeFromTop(5);
        
        // Calculate waveform height based on 4cm at 96 DPI (fixed height)
        const int waveformHeight = static_cast<int>(4 * 37.8); // ~151px
        
        // ===== CRITICAL FIX #1: Reserve scrollbar space in viewport bounds =====
        // Viewport needs extra height to accommodate scrollbar without squishing content
        auto waveformRect = area.removeFromTop(waveformHeight + SCROLLBAR_HEIGHT);
        waveformViewport.setBounds(waveformRect);
        emptyStateLabel.setBounds(waveformRect);   // overlays the waveform area
        fixedViewportWidth = waveformRect.getWidth();
        
        // Position fixed info labels within the viewport area
        auto labelArea = waveformRect;
        topInfoLabel.setBounds(labelArea.removeFromTop(20).reduced(2));
        
        // ===== CRITICAL FIX #2: Remove bottomInfoLabel from waveform viewport =====
        // It will now appear in the bottom row with the duration indicator
        // bottomInfoLabel.setBounds(labelArea.removeFromBottom(25).reduced(2));  // REMOVE THIS
        
        // ===== CRITICAL FIX #3: Always show scrollbar (disable when not needed) =====
        // This prevents height changes when scrollbar appears/disappears
        waveformViewport.setScrollBarsShown(false, true); // false for vertical, ALWAYS true for horizontal
        
        // ADD SAFETY CHECK HERE
        if (waveformComponent != nullptr && waveformContainer != nullptr)
        {
            // Update the waveform container and component size
            updateWaveformSize();
            waveformViewport.setViewPosition(0, 0);  // Reset scroll position
        }
        
        // Add 5px margin between waveform and controls
        area.removeFromTop(5);

        // ===== TAB BAR (24px) =====
        {
            auto tabBar = area.removeFromTop(24);
            applyTabStyle(controlsTabButton, activeTab == 0);
            applyTabStyle(adsrTabButton,     activeTab == 1);
            applyTabStyle(eqTabButton,       activeTab == 2);
            controlsTabButton.setBounds(tabBar.removeFromLeft(80).reduced(1, 2));
            adsrTabButton.setBounds    (tabBar.removeFromLeft(80).reduced(1, 2));
            eqTabButton.setBounds      (tabBar.removeFromLeft(80).reduced(1, 2));
            // ADSR and EQ on/off buttons share the same right-aligned slot (44px).
            // Only one is ever visible at a time — updateTabVisibility() enforces this.
            auto rightToggleSlot = tabBar.withLeft(tabBar.getRight() - 44);
            adsrEnableButton.setBounds(rightToggleSlot.reduced(1, 2));
            eqEnableButton.setBounds(tabBar.removeFromRight(44).reduced(1, 2));
            // Filter mode selector — left of EQ enable button (~108px), only shown on EQ tab
            filterModeButton.setBounds(tabBar.removeFromRight(108).reduced(1, 2));
            // Normalize controls — left of filter mode button; only shown on EQ tab
            // Layout (right→left removal): gainLabel | Norm | 0 | -6 | -12
            normGainLabel.setBounds           (tabBar.removeFromRight(52).reduced(1, 2));
            normButton.setBounds              (tabBar.removeFromRight(42).reduced(1, 2));
            normTargetZeroButton.setBounds    (tabBar.removeFromRight(30).reduced(1, 2));
            normTargetMinus6Button.setBounds  (tabBar.removeFromRight(30).reduced(1, 2));
            normTargetMinus12Button.setBounds (tabBar.removeFromRight(36).reduced(1, 2));
            // EQ Reset — leftmost of the EQ controls group, right-adjacent to -12 button.
            eqResetButton.setBounds           (tabBar.removeFromRight(52).reduced(1, 2));
        }
        area.removeFromTop(4); // gap below tab bar

        // ===== Tab content area — height capped so footer is always at a fixed bottom Y =====
        // Footer is anchored at: getHeight() - 10 (bottom margin) - 24 (footer height).
        // Tab content fills exactly the space between the tab bar and the footer.
        constexpr int kFooterH = 24;
        const int footerY       = getHeight() - 10 - kFooterH;
        auto tabContentArea     = area.withHeight(juce::jmax(0, footerY - area.getY()));

        // Print layout diagnostics once per launch
        static bool diagPrinted = false;
        if (!diagPrinted)
        {
            diagPrinted = true;
            const int wvH = static_cast<int>(4 * 37.8) + SCROLLBAR_HEIGHT;
            printf("[LAYOUT] SamplerPad total height:  %dpx\n", getHeight());
            printf("[LAYOUT] Waveform area:            %dpx  (y=%d)\n", wvH, 10 + 24 + 5);
            printf("[LAYOUT] Tab bar area:             24px  (y=%d)\n", 10 + 24 + 5 + wvH + 5);
            printf("[LAYOUT] Tab content area:         %dpx  (y=%d)\n", tabContentArea.getHeight(), tabContentArea.getY());
            printf("[LAYOUT] Footer area:              %dpx  (y=%d)\n", kFooterH, footerY);
            printf("[LAYOUT] EQ display height:        %dpx\n", tabContentArea.getHeight());
        }

        // ===== TAB CONTENT (routed by active tab; all use tabContentArea) =====
        if (activeTab == 1)
            layoutAdsrTabContent(tabContentArea);
        else if (activeTab == 2)
            layoutEqTabContent(tabContentArea);

        if (activeTab == 0)
        {
        // Standard button height = tab button height (tab bar 24px, reduced(1,2) = 20px).
        static bool layoutPrinted = false;
        if (!layoutPrinted) {
            DBG("[LAYOUT] Tab button height: 20px — applying to all Controls tab buttons");
            layoutPrinted = true;
        }
        constexpr int kBtnH = 20;

        // ===== ROW 1: Playback controls (24px) =====
        auto row1 = tabContentArea.removeFromTop(24);

        {
            // Pitch controls: Down | Pitch display | Up  (180px, kBtnH tall, vertically centred)
            const int pitchCtrlW = 180;
            auto pitchArea = row1.removeFromLeft(pitchCtrlW);
            auto pitchControlArea = pitchArea.withSizeKeepingCentre(pitchCtrlW, kBtnH);
            pitchDownButton.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
            pitchLabel.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
            pitchUpButton.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
        }
        row1.removeFromLeft(4); // gap
        {
            // Pitch Step cycling button — label hidden at this row height
            auto col = row1.removeFromLeft(60);
            pitchStepLabel.setBounds({});  // no room at 24px row height
            pitchStepButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }
        row1.removeFromLeft(4); // gap between step and toggle buttons
        {
            // Tune button — pitch detection (right of pitch controls)
            auto col = row1.removeFromLeft(62);
            tuneButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }
        {
            // Freeze button
            auto col = row1.removeFromLeft(62);
            freezeButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }
        {
            // Loop button
            auto col = row1.removeFromLeft(62);
            loopButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }
        {
            // One Shot button — right of Loop
            auto col = row1.removeFromLeft(62);
            oneShotButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }
        {
            // Rev (reverse) button — right of One Shot
            auto col = row1.removeFromLeft(62);
            reverseButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }
        {
            // Bnc (bounce/ping-pong) button — right of Rev
            auto col = row1.removeFromLeft(62);
            bounceButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }

        // 4px gap between rows (matches tab-bar gap)
        tabContentArea.removeFromTop(4);

        // ===== ROW 2: Marker controls (24px) =====
        auto row2 = tabContentArea.removeFromTop(24);
        transientCountLabel.setBounds({});  // hidden — no room at uniform row height

        {
            // "CRA" — transient detection toggle
            auto col = row2.removeFromLeft(44);
            detectionToggleButton.setBounds(col.withSizeKeepingCentre(42, kBtnH));
        }
        {
            // "< T" — snap start to PREV transient
            auto col = row2.removeFromLeft(38);
            prevTransientButton.setBounds(col.withSizeKeepingCentre(36, kBtnH));
        }
        {
            // "T >" — snap start to NEXT transient
            auto col = row2.removeFromLeft(38);
            nextTransientButton.setBounds(col.withSizeKeepingCentre(36, kBtnH));
        }
        {
            // Start knob — 60px column: left 30px = "Start" label, right 30px = knob
            auto col = row2.removeFromLeft(60);
            startKnobLabel.setBounds(col.removeFromLeft(30).withSizeKeepingCentre(28, kBtnH));
            startKnob.setBounds(col.withSizeKeepingCentre(kBtnH, kBtnH));
        }
        {
            // End knob — 60px column: left 30px = "End" label, right 30px = knob
            auto col = row2.removeFromLeft(60);
            endKnobLabel.setBounds(col.removeFromLeft(30).withSizeKeepingCentre(28, kBtnH));
            endKnob.setBounds(col.withSizeKeepingCentre(kBtnH, kBtnH));
        }
        {
            // "< T" — snap end to PREV transient
            auto col = row2.removeFromLeft(38);
            prevEndTransientButton.setBounds(col.withSizeKeepingCentre(36, kBtnH));
        }
        {
            // "T >" — snap end to NEXT transient
            auto col = row2.removeFromLeft(38);
            nextEndTransientButton.setBounds(col.withSizeKeepingCentre(36, kBtnH));
        }
        {
            // Sens knob — 90px column: left 30px = "Sens" label, 20px = knob, right 35px = value
            auto col = row2.removeFromLeft(90);
            sensLabel.setBounds(col.removeFromLeft(30).withSizeKeepingCentre(28, kBtnH));
            sensKnob.setBounds(col.removeFromLeft(20).withSizeKeepingCentre(kBtnH, kBtnH));
            sensValueLabel.setBounds(col.withSizeKeepingCentre(33, kBtnH));
        }
        {
            // Grid snap button — moved here from Row 1
            auto col = row2.removeFromLeft(62);
            gridSnapButton.setBounds(col.withSizeKeepingCentre(58, kBtnH));
        }
        {
            // Grid resolution cycling button — moved here from Row 1
            auto col = row2.removeFromLeft(52);
            gridResolutionButton.setBounds(col.withSizeKeepingCentre(50, kBtnH));
        }
        // Vol knob is now in the top row — skip its row2 layout

        // 4px gap then Row 3: Trim button (24px)
        tabContentArea.removeFromTop(4);
        {
            auto row3 = tabContentArea.removeFromTop(24);
            trimButton.setBounds(row3.removeFromLeft(62).withSizeKeepingCentre(58, kBtnH));
        }

        } // end if (activeTab == 0)

        updateTabVisibility();

        // ===== Footer — fixed at absolute bottom regardless of active tab =====
        footerBounds = { 10, footerY, getWidth() - 20, kFooterH };
        auto bottomRow = footerBounds;

        const int rowW = bottomRow.getWidth();
        const int pitchIndicatorWidth = static_cast<int>(rowW * 0.22);
        const int durationWidth       = static_cast<int>(rowW * 0.12);
        const int filenameWidth       = rowW - pitchIndicatorWidth - durationWidth - 8;

        sampleNameLabel.setBounds (bottomRow.removeFromLeft(filenameWidth).reduced(3, 0));
        bottomInfoLabel.setBounds (bottomRow.removeFromRight(pitchIndicatorWidth).reduced(3, 0));
        durationLabel.setBounds   (bottomRow.removeFromRight(durationWidth).reduced(3, 0));
        // remaining 8px is a spacer between filename and duration

        // Toast overlay — covers entire footer; invisible by default, shown on trim result
        toastLabel.setBounds(footerBounds);

        durationLabel.setFont(juce::Font(11.0f));
        bottomInfoLabel.setFont(juce::Font(11.0f, juce::Font::bold));
        bottomInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD4A017));
    }
    
    void paint(juce::Graphics& g) override
    {
        // Draw card background
        g.setColour(juce::Colour(0xFF636363));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);

        // Draw card border
        g.setColour(juce::Colour(0xFF5A5A5A));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 8.0f, 1.5f);

        // Draw footer background — slightly darker than card to visually frame the info row.
        // Clip to the card's rounded rect so bottom corners stay rounded.
        if (!footerBounds.isEmpty())
        {
            g.saveState();
            juce::Path cardClip;
            cardClip.addRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
            g.reduceClipRegion(cardClip);
            g.setColour(juce::Colour(0xFF2A2A2A));
            g.fillRect(footerBounds);
            g.restoreState();
            // Subtle top separator line
            g.setColour(juce::Colour(0xFF444444));
            g.drawHorizontalLine(footerBounds.getY(),
                                 (float)footerBounds.getX(),
                                 (float)footerBounds.getRight());
        }

        // Draw dark outline around every VISIBLE TextButton child.
        // Must skip invisible components — hidden tab controls still exist as children
        // but must not draw their outlines when their tab is not active.
        g.setColour(juce::Colour(0xFF0A0A0A));
        for (auto* child : getChildren())
            if (child->isVisible() && dynamic_cast<juce::TextButton*>(child) != nullptr)
                g.drawRect(child->getBounds(), 1);
    }
    
    // Public methods
    juce::TextButton& getAddButton() { return addButton; }
    juce::TextButton& getPrevButton() { return prevButton; }
    juce::TextButton& getNextButton() { return nextButton; }
    
    int getMidiNote() const { return currentMidiNote; }
    int getMidiChannel() const { return currentMidiChannel; }
    int getPitchOffset() const { return pitchOffset; }
    float getVolume() const { return (float)volumeKnob.getValue(); }

    void setVolume(float volume)
    {
        float v = juce::jlimit(0.0f, 1.0f, volume);
        volumeKnob.setValue(v, juce::dontSendNotification);
        volValueLabel.setText(juce::String(juce::roundToInt(v * 100)) + "%",
                              juce::dontSendNotification);
    }

    // Start point — in seconds. Pass 0 to reset to the beginning.
    double getStartPointSeconds() const
    {
        if (originalDuration <= 0.0) return 0.0;
        return startPointNormalized * originalDuration;
    }

    void setStartPoint(double seconds)
    {
        if (originalDuration <= 0.0) return;
        float norm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
        startPointNormalized = norm;
        startKnob.setValue(norm, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setStartMarker(norm);
    }

    void resetStartPoint()
    {
        startPointNormalized = 0.0f;
        startKnob.setValue(0.0, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setStartMarker(0.0f);
    }

    // End point — in seconds. Pass a value ≥ duration to snap to the very end.
    double getEndPointSeconds() const
    {
        if (originalDuration <= 0.0) return 0.0;
        return endPointNormalized * originalDuration;
    }

    void setEndPoint(double seconds)
    {
        if (originalDuration <= 0.0) return;
        float norm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
        endPointNormalized = norm;
        endKnob.setValue(norm, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setEndMarker(norm);
    }

    void resetEndPoint()
    {
        endPointNormalized = 1.0f;
        endKnob.setValue(1.0, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setEndMarker(1.0f);
    }

    double getTransientThreshold() const { return transientThreshold; }

    void setTransientThreshold(double threshold, bool runDetection = true)
    {
        transientThreshold = juce::jlimit(1.5, 10.0, threshold);
        sensKnob.setValue(transientThreshold, juce::dontSendNotification);
        sensValueLabel.setText(juce::String(transientThreshold, 1) + "x", juce::dontSendNotification);
        if (runDetection && currentAudioFile.existsAsFile())
            detectTransients(currentAudioFile);
    }

    // Auto-tune detection result — read by persistence, written on detection or restore
    juce::String getDetectedNoteName() const { return detectedNoteName; }
    double       getDetectedFreqHz()   const { return detectedFreqHz; }
    int          getBasePitchOffset()  const { return basePitchOffset; }

    // Quietly restore the hidden base offset on session load — does NOT fire any listener.
    void setBasePitchOffset(int base)
    {
        basePitchOffset = juce::jlimit(-48, 48, base);
    }

    // Restore a previously-detected note (called on session load — does NOT fire the listener).
    void setDetectedNoteName(const juce::String& name, double freqHz)
    {
        detectedNoteName = name;
        detectedFreqHz   = freqHz;
        if (name.isNotEmpty())
        {
            tuneButton.setButtonText(name);
            tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF1A5A1A)); // dark green
            tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF00FF88)); // bright green text
            // Build bottomInfoLabel text
            juce::String info = name;
            if (freqHz > 0.0)
                info += " - " + juce::String((int)std::round(freqHz)) + " Hz";
            bottomInfoLabel.setText(info, juce::dontSendNotification);
        }
        else
        {
            tuneButton.setButtonText("Tune");
            tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
            tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
            updatePitchDisplay(pitchOffset); // restore normal bottom label
        }
    }

    bool isTransientDetectionEnabled() const { return transientDetectionEnabled; }

    bool isOneShotEnabled() const { return oneShotEnabled; }

    // Quietly restore one-shot state on startup — does NOT fire listener.
    void setOneShotEnabled(bool enabled)
    {
        oneShotEnabled = enabled;
        oneShotButton.setToggleState(enabled, juce::dontSendNotification);
    }

    bool isReverseEnabled() const { return reverseEnabledState; }

    // Quietly restore reverse state on startup — does NOT fire listener.
    void setReverseEnabled(bool enabled)
    {
        reverseEnabledState = enabled;
        reverseButton.setToggleState(enabled, juce::dontSendNotification);
    }

    bool isBounceEnabled() const { return bounceEnabledState; }

    // Quietly restore bounce state on startup — does NOT fire listener.
    void setBounceEnabled(bool enabled)
    {
        bounceEnabledState = enabled;
        bounceButton.setToggleState(enabled, juce::dontSendNotification);
    }

    void resetBounce()
    {
        bounceEnabledState = false;
        bounceButton.setToggleState(false, juce::dontSendNotification);
    }

    // Called by MainComponent when a note-off arrives while oneshot is active (tail started),
    // or when all voices have finished (tail ended).  Pulses the button during the tail.
    void setOneShotTailActive(bool active)
    {
        if (active)
        {
            oneShotPulsePhase = 0;
            oneShotPulseTimer.startTimer(350);
        }
        else
        {
            oneShotPulseTimer.stopTimer();
            // Restore full amber if still ON
            if (oneShotEnabled)
            {
                oneShotButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFFFB300));
                oneShotButton.repaint();
            }
        }
    }

    int  getPitchStepCents() const { return currentPitchStepCents; }

    // Quietly restore step size on startup — does NOT fire listener.
    void setPitchStepCents(int cents)
    {
        static const int valid[] = { 100, 50, 25, 33 };
        for (auto v : valid)
            if (v == cents) { currentPitchStepCents = v; updatePitchStepButton(); return; }
    }

    double getBaseTuningHz() const { return baseTuningHz; }

    // Quietly set tuning frequency on startup — does NOT fire a listener.
    void setBaseTuningHz(double hz)
    {
        baseTuningHz = juce::jlimit(400.0, 480.0, hz);
    }

    // Quietly restore transient detection state on startup — does NOT fire listener.
    void setTransientDetectionEnabled(bool enabled)
    {
        transientDetectionEnabled = enabled;
        detectionToggleButton.setToggleState(enabled, juce::dontSendNotification);
        detectionToggleButton.setColour(juce::TextButton::buttonColourId,
            enabled ? juce::Colour(0xFF8B2500) : juce::Colour(0xFF4A4A4A));
        detectionToggleButton.setColour(juce::TextButton::textColourOffId,
            enabled ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF7A7A7A));
        updateTransientControlsState();
    }

    bool isGridSnapEnabled() const { return gridSnapEnabled; }

    int getGridResolutionIndex() const { return gridResolutionIndex; }

    double getGridInterval() const
    {
        static const double vals[] = { 0.001, 0.01, 0.05, 0.1, 0.5, 1.0 };
        return vals[juce::jlimit(0, numGridResolutions - 1, gridResolutionIndex)];
    }

    void setGridResolutionIndex(int index)
    {
        gridResolutionIndex = juce::jlimit(0, numGridResolutions - 1, index);
        userHasSetGridResolution = true; // marks a saved preference — blocks auto-select override
        updateGridResolutionButton();
        if (waveformComponent != nullptr)
            waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
    }

    void setGridSnapEnabled(bool enabled)
    {
        gridSnapEnabled = enabled;
        gridSnapButton.setToggleState(enabled, juce::dontSendNotification);
        updateGridResolutionButtonState();
        if (waveformComponent != nullptr)
            waveformComponent->setGridResolution(enabled, getGridInterval());
    }

    bool isLoopEnabled() const { return loopButton.getToggleState(); }

    void setLoopEnabled(bool enabled)
    {
        loopButton.setToggleState(enabled, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setLoopHighlight(enabled);
    }

    bool isFreezeEnabled() const { return isFreezeActive; }

    // Called by MainComponent when a new sample is loaded.
    // Always resets freeze (it never persists). Fires freezeChanged(false) only if was active.
    void resetFreeze()
    {
        lastFreezeTapMs = 0;
        loopWasOnBeforeFreeze = false;
        if (isFreezeActive)
        {
            isFreezeActive = false;
            applyFreezeButtonStyle(false);
            listeners.call([](Listener& l) { l.freezeChanged(false); });
        }
    }

    void setPitchOffset(int cents)
    {
        // Constrain to ±4800 cents (±48 semitones × 100)
        if (cents >= -4800 && cents <= 4800 && cents != pitchOffset)
        {
            pitchOffset = cents;
            updatePitchDisplay(pitchOffset);

            printf("Pitch offset set to: %+d cents\n", pitchOffset);
        }
    }

    // notify=false: update the UI display only — no listener fired.
    // Use notify=false during pad switch (updateUIFromSettings) to avoid triggering
    // updateSamplerSounds() and allNotesOff() which are not needed when the engine
    // was already preloaded with the correct MIDI note.
    void setMidiNote(int note, bool notify = true)
    {
        if (note >= 0 && note <= 127 && note != currentMidiNote)
        {
            currentMidiNote = note;
            updateMidiNoteDisplay();

            if (notify)
                listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });

            printf("MIDI note set to: %d (%s)%s\n",
                   currentMidiNote,
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8(),
                   notify ? "" : " [silent]");
        }
    }

    void setMidiChannel(int channel, bool notify = true)
    {
        if (channel >= 0 && channel <= 16 && channel != currentMidiChannel)
        {
            currentMidiChannel = channel;
            updateMidiChannelDisplay();

            if (notify)
                listeners.call([this](Listener& l) { l.midiChannelChanged(currentMidiChannel); });

            printf("MIDI channel set to: %s%s\n",
                   currentMidiChannel == 0 ? "All Channels" : juce::String(currentMidiChannel).toRawUTF8(),
                   notify ? "" : " [silent]");
        }
    }
    
    void setSampleName(const juce::String& name)
    {
        sampleNameLabel.setText(name, juce::dontSendNotification);
        repaint();
    }

    //==========================================================================
    // Empty pad state — shown when the pad has no sample loaded.
    void setEmptyState(bool isEmpty)
    {
        isEmptyPad = isEmpty;
        emptyStateLabel.setVisible(isEmpty);
        if (isEmpty)
        {
            if (waveformComponent != nullptr)
                waveformComponent->setFile(juce::File{});
            sampleNameLabel.setText ("Empty pad", juce::dontSendNotification);
            durationLabel.setText   ("",           juce::dontSendNotification);
            bottomInfoLabel.setText ("",           juce::dontSendNotification);
        }
        repaint();
    }

    //==========================================================================
    // Restore all UI controls from a PadSettings snapshot.
    // Called when the user switches to a different pad, silently (no listeners fired
    // for parameter changes that would trigger saves).
    void updateUIFromSettings(const PadSettings& s)
    {
        // Pitch (no listeners — caller is responsible for propagating to audio engine)
        setPitchOffset     (s.pitchCents);
        setBasePitchOffset (s.basePitchOffset);
        setBaseTuningHz    (s.baseTuningHz);
        setPitchStepCents  (s.pitchStepCents);
        if (s.detectedNoteName.isNotEmpty())
            setDetectedNoteName(s.detectedNoteName, s.detectedFreqHz);

        // MIDI routing — silent: engine already has the correct MIDI note from preloading.
        // Firing the listener would call updateSamplerSounds() + allNotesOff() unnecessarily.
        setMidiNote    (s.midiNote,    /*notify=*/false);
        setMidiChannel (s.midiChannel, /*notify=*/false);

        // Playback modes
        setLoopEnabled    (s.loopEnabled);
        setOneShotEnabled (s.oneShotEnabled);
        setReverseEnabled (s.reverseEnabled);
        setBounceEnabled  (s.bounceEnabled);

        // Volume
        setVolume (s.volumeLevel);

        // Normalize (silent — no listener, no disk write)
        setNormParams (s.normEnabled, s.normTargetDb, /*notifyListeners=*/false);

        // ADSR (silent)
        setAdsrParams (s.adsrEnabled,
                       s.adsrAttackMs, s.adsrDecayMs,
                       s.adsrSustain,  s.adsrReleaseMs,
                       /*notifyListeners=*/false);

        // EQ (silent)
        setEqParams (s.eqEnabled,
                     s.eq1Freq, s.eq1Gain, s.eq1Q,
                     s.eq2Freq, s.eq2Gain, s.eq2Q,
                     s.eq3Freq, s.eq3Gain, s.eq3Q,
                     /*notifyListeners=*/false);
        setEqFilterModes (s.eq1Mode, s.eq2Mode, s.eq3Mode, /*notifyListeners=*/false);

        // Transient detection — runDetection=false prevents blocking disk I/O during pad switch;
        // detection runs later when the waveform is actually displayed for this pad.
        setTransientDetectionEnabled (s.transientDetectionEnabled);
        setTransientThreshold        (s.transientThreshold, /*runDetection=*/false);

        // Grid snap
        setGridSnapEnabled     (s.gridSnapEnabled);
        setGridResolutionIndex (s.gridResolutionIndex);

        // Active tab
        setActiveTabQuiet (s.activeTab);
    }

    void setDuration(double seconds)
    {
        originalDuration = seconds;
        
        // Apply current pitch factor to the displayed duration
        double adjustedDuration = seconds / currentPitchFactor;
        durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
        
        // Force repaint
        durationLabel.repaint();
    }
    
    void setPitchFactor(double semitones)
    {
        // Convert semitones to pitch factor (2^(semitones/12))
        currentPitchFactor = std::pow(2.0, semitones / 12.0);
        
        // Update duration display based on pitch factor
        // Pitch up = shorter duration, Pitch down = longer duration
        if (originalDuration > 0.0)
        {
            double adjustedDuration = originalDuration / currentPitchFactor;
            durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
        }
        
        // Trigger a repaint to show the "stretched" waveform visually
        // Note: We can't actually stretch the thumbnail, but we can indicate pitch change
        // by changing the waveform color or adding an overlay
        repaint();
        if (waveformComponent != nullptr)
            waveformComponent->repaint();
    }
    
    void updatePitchDisplay(int cents)
    {
        // Format pitch display — whole semitones show "N st", fractional show "N¢"
        juce::String displayText;
        if (cents % 100 == 0)
        {
            int semitones = cents / 100;
            if (semitones == 0)
                displayText = "0 st";
            else if (semitones > 0)
                displayText = "+" + juce::String(semitones) + " st";
            else
                displayText = juce::String(semitones) + " st";
        }
        else
        {
            // Microtonal — show in cents
            if (cents > 0)
                displayText = "+" + juce::String(cents) + "c"; // ¢
            else
                displayText = juce::String(cents) + "c";
        }

        pitchLabel.setText(displayText, juce::dontSendNotification);

        // Pitch factor from cents: pow(2, cents/1200)
        double pitchFactor = std::pow(2.0, (double)cents / 1200.0);
        currentPitchFactor = pitchFactor;

        if (waveformComponent != nullptr)
            waveformComponent->setPitchFactor(pitchFactor, cents / 100);

        // Update duration display
        if (originalDuration > 0)
        {
            double adjustedDuration = originalDuration / pitchFactor;
            durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
            printf("Pitch: %+d cents, Factor: %.3f, Original: %.2f, Adjusted: %.2f\n",
                cents, pitchFactor, originalDuration, adjustedDuration);
        }

        // Top info label
        if (cents > 0)
            topInfoLabel.setText("COMPRESSED", juce::dontSendNotification);
        else if (cents < 0)
            topInfoLabel.setText("EXPANDED", juce::dontSendNotification);
        else
            topInfoLabel.setText("", juce::dontSendNotification);

        // Bottom info label — shift detected note name when tune result is active
        if (detectedNoteName.isNotEmpty())
        {
            const int nearestMidi = tuneRootMidiNote - basePitchOffset;
            // shiftedMidi = nearestMidi + user_semitones (approximate for note name display)
            const int approxSemitones = cents / 100;
            const int shiftedMidi = juce::jlimit(0, 127, nearestMidi + approxSemitones);
            static const char* noteNames[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
            const juce::String shiftedName = juce::String(noteNames[shiftedMidi % 12])
                                           + juce::String(shiftedMidi / 12 - 1);
            if (cents == 0)
            {
                const int freqInt = detectedFreqHz > 0.0 ? (int)std::round(detectedFreqHz) : 0;
                juce::String info = detectedNoteName;
                if (freqInt > 0)
                    info += " - " + juce::String(freqInt) + " Hz";
                bottomInfoLabel.setText(info, juce::dontSendNotification);
            }
            else
            {
                const double shiftedFreq = detectedFreqHz * pitchFactor;
                const int shiftedFreqInt = shiftedFreq > 0.0 ? (int)std::round(shiftedFreq) : 0;
                juce::String info = shiftedName;
                if (shiftedFreqInt > 0)
                    info += " - " + juce::String(shiftedFreqInt) + " Hz";
                bottomInfoLabel.setText(info, juce::dontSendNotification);
            }
        }
        else
        {
            // No tune result: show generic pitch direction
            if (cents % 100 == 0)
            {
                int s = cents / 100;
                if (s > 0)
                    bottomInfoLabel.setText("PITCH UP: +" + juce::String(s) + " st", juce::dontSendNotification);
                else if (s < 0)
                    bottomInfoLabel.setText("PITCH DOWN: " + juce::String(s) + " st", juce::dontSendNotification);
                else
                    bottomInfoLabel.setText("ORIGINAL WAVE", juce::dontSendNotification);
            }
            else
            {
                if (cents > 0)
                    bottomInfoLabel.setText("PITCH UP: +" + juce::String(cents) + "c", juce::dontSendNotification);
                else if (cents < 0)
                    bottomInfoLabel.setText("PITCH DOWN: " + juce::String(cents) + "c", juce::dontSendNotification);
                else
                    bottomInfoLabel.setText("ORIGINAL WAVE", juce::dontSendNotification);
            }
        }

        topInfoLabel.repaint();
        bottomInfoLabel.repaint();
        durationLabel.repaint();

        updateWaveformSize();
    }
        
    // skipTransients=true during rapid navigation: skip detectTransients() here and
    // let the deferred timer in MainComponent run it 800ms after navigation stops.
    // knownTotalSamples / knownSampleRate — pass values already obtained on the background
    // thread to avoid creating a reader on the message thread (which blocks for ~7s on MP3).
    void setWaveform(const juce::File& audioFile, bool skipTransients = false,
                     juce::int64 knownTotalSamples = 0, double knownSampleRate = 0.0)
    {
        // Reset ADSR silently — notifyListeners=false prevents saveCurrentSampleState() from
        // firing here, which would clobber the PropertiesFile in-memory store with defaults
        // before getSampleState() reads the previously saved values.
        setAdsrParams(false, 0.0f, 0.0f, 1.0f, 0.0f, /*notifyListeners=*/false);
        printf("[ADSR-DBG] setWaveform() silent reset — ADSR defaults applied, no save triggered\n");
        // Reset EQ filter modes silently — same pattern as ADSR; real values restored by load lambda.
        setEqFilterModes(2, 2, 2, /*notifyListeners=*/false);
        // Reset normalize silently — real values restored by load lambda.
        setNormParams(false, -6.0f, /*notifyListeners=*/false);

        // Reset tune detection state for the new file
        detectedNoteName = "";
        detectedFreqHz   = 0.0;
        basePitchOffset  = 0;
        tuneButton.setButtonText("Tune");
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        tuneButton.setEnabled(true);

        // CRITICAL FIX #1: Store file FIRST
        currentAudioFile = audioFile;

        if (audioFile.existsAsFile())
        {
            // If sample metadata was already read on the background thread, use it directly —
            // avoids calling createReaderFor() on the message thread (blocks ~7s for MP3).
            const bool haveMetadata = (knownTotalSamples > 0 && knownSampleRate > 0.0);
            if (haveMetadata)
            {
                originalLengthInSamples = knownTotalSamples;
                originalSampleRate      = knownSampleRate;
            }

            // Only create a reader if we don't already have the metadata.
            std::unique_ptr<juce::AudioFormatReader> reader;
            if (!haveMetadata)
                reader.reset(formatManager.createReaderFor(audioFile));

            if (haveMetadata || reader != nullptr)
            {
                if (!haveMetadata)
                {
                    originalLengthInSamples = reader->lengthInSamples;
                    originalSampleRate = reader->sampleRate;
                }
                
                // CRITICAL FIX #2: Reset scroll position BEFORE anything else
                // Reset zoom on new sample load
                waveformZoomLevel = 1.0;
                if (waveformComponent != nullptr)
                    waveformComponent->setZoomLevel(1.0);
                waveformViewport.setViewPosition(0, 0);
                
                // CRITICAL FIX #3: Force waveform component to invalidate ALL cache
                if (waveformComponent != nullptr)
                {
                    waveformComponent->setFile(audioFile);  // This clears cachedTotalLength, cachedNumChannels
                }
                
                // CRITICAL FIX #4: Update container size AFTER file is set
                updateWaveformSize();

                // Reset start/end points when a new file is loaded
                resetStartPoint();
                resetEndPoint();

                // Transient detection: run immediately on normal load (+button / startup).
                // Skip during navigation (skipTransients=true) — deferred timer will run it.
                if (skipTransients)
                {
                    // Clear stale markers from previous sample; show "?" count until timer fires
                    transientPositionsSeconds.clear();
                    transientCountLabel.setText("T: ?", juce::dontSendNotification);
                    if (waveformComponent != nullptr)
                        waveformComponent->setTransients({});
                }
                else if (transientDetectionEnabled)
                    detectTransients(audioFile);
                else
                {
                    // CRA is OFF — clear any stale markers without running detection
                    transientPositionsSeconds.clear();
                    transientCountLabel.setText("T: 0", juce::dontSendNotification);
                    if (waveformComponent != nullptr)
                        waveformComponent->setTransients({});
                }

                // Auto-select grid resolution based on the new sample's duration
                {
                    double sampleDur = (originalSampleRate > 0.0 && originalLengthInSamples > 0)
                                       ? (double)originalLengthInSamples / originalSampleRate
                                       : 0.0;
                    if (sampleDur > 0.0)
                        autoSelectGridResolution(sampleDur);
                }

                printf("Waveform set for: %s (sample rate: %.1f kHz, length: %lld samples)\n",
                    audioFile.getFileName().toRawUTF8(),
                    originalSampleRate / 1000.0,
                    originalLengthInSamples);
            }
            else
            {
                printf("ERROR: Could not create reader for file: %s\n",
                    audioFile.getFileName().toRawUTF8());
                
                if (waveformComponent != nullptr)
                    waveformComponent->setFile(juce::File());
            }
        }
        else
        {
            originalLengthInSamples = 0;
            originalSampleRate = 0.0;
            
            if (waveformComponent != nullptr)
                waveformComponent->setFile(juce::File());
            
            repaint();
        }
    }
    
    void clearWaveform()
    {
        repaint();
        if (waveformComponent != nullptr)
            waveformComponent->repaint();
    }
    
    juce::Rectangle<int> getWaveformArea() const 
    { 
        if (waveformComponent != nullptr)
            return waveformComponent->getBounds();
        return juce::Rectangle<int>();
    }

    // Add listener for MIDI note changes
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void midiNoteChanged(int newNote) = 0;
        virtual void midiChannelChanged(int newChannel) = 0;
        virtual void learningModeChanged(bool isLearning) = 0;
        virtual void pitchOffsetChanged(int pitchOffset) = 0;
        virtual void volumeChanged(float volume) = 0;
        virtual void startPointChanged(double startPointSeconds) = 0;
        virtual void endPointChanged(double endPointSeconds) = 0;
        virtual void loopEnabledChanged(bool isLooping) = 0;
        virtual void freezeChanged(bool isFrozen) = 0;
        virtual void gridSnapChanged(bool isEnabled) = 0;
        virtual void gridResolutionChanged(int index) = 0;
        virtual void detectedNoteChanged(const juce::String& noteName, double freqHz) = 0;
        virtual void transientDetectionEnabledChanged(bool enabled) = 0;
        virtual void oneShotEnabledChanged(bool enabled) = 0;
        virtual void reverseEnabledChanged(bool enabled) = 0;
        virtual void bounceEnabledChanged(bool enabled) = 0;
        virtual void pitchStepCentsChanged(int cents) = 0;
        virtual void adsrParamsChanged(bool enabled, float attackMs, float decayMs, float sustain, float releaseMs) = 0;
        virtual void activeTabChanged(int tabIndex) = 0;
        virtual void eqParamsChanged(bool enabled,
                                     float f1, float g1, float q1,
                                     float f2, float g2, float q2,
                                     float f3, float g3, float q3) = 0;
        // Called when the filter mode changes for any band (0=LowCut 1=LowShelf 2=Bell 3=Notch 4=HighShelf 5=HighCut)
        virtual void eqFilterModesChanged(int mode1, int mode2, int mode3) = 0;
        // Called when Norm toggle or target dB changes.
        virtual void normChanged(bool enabled, float targetDb) = 0;
    };
    
    void addListener(Listener* listener)
    {
        listeners.add(listener);
    }
    
    void removeListener(Listener* listener)
    {
        listeners.remove(listener);
    }
    
    // MIDI Learn public methods
    void setLearningMode(bool learning)
    {
        isLearning = learning;
        learnButton.setToggleState(learning, juce::dontSendNotification);
        if (isLearning)
        {
            learnButton.setButtonText("Learning...");
            listeners.call([this](Listener& l) { l.learningModeChanged(true); });
        }
        else
        {
            learnButton.setButtonText("Learn");
            listeners.call([this](Listener& l) { l.learningModeChanged(false); });
        }
    }
    
    bool isInLearningMode() const { return isLearning; }
    
    void setMidiNoteFromLearn(int note)
    {
        if (isLearning && note != currentMidiNote)
        {
            currentMidiNote = note;
            updateMidiNoteDisplay();
            
            // Notify listeners of the new note
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            // Automatically exit learn mode after setting the note
            if (isLearning)
            {
                isLearning = false;
                learnButton.setToggleState(false, juce::dontSendNotification);
                learnButton.setButtonText("Learn");
                listeners.call([this](Listener& l) { l.learningModeChanged(false); });
            }
            
            printf("MIDI note learned: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
    }

    private:

        // Background thread that runs YIN pitch detection
        class TuneAnalyzerThread : public juce::Thread
        {
        public:
            TuneAnalyzerThread(SampleCard& owner)
                : juce::Thread("TuneAnalyzer"), owner(owner) {}
            void run() override { owner.runTuneAnalysis(); }
        private:
            SampleCard& owner;
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TuneAnalyzerThread)
        };

        // Draggable pitch label — click+drag up/down to change semitone offset.
        // 5px of vertical movement = 1 semitone; drag-up = pitch up.
        class DraggablePitchLabel : public juce::Label
        {
        public:
            // Called during drag AND on release with the new absolute pitch value.
            std::function<void(int newPitch)> onPitchDragged;
            // Called once on mouse-up to persist the final value.
            std::function<void()>             onDragFinished;
            // Provide the current pitch at drag-start.
            std::function<int()>              getCurrentPitch;
            // Provide the current step size in cents (default 100 = 1 semitone).
            std::function<int()>              getPitchStep;

            void mouseEnter(const juce::MouseEvent&) override
            {
                isHovered = true;
                setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
                repaint();
            }

            void mouseExit(const juce::MouseEvent&) override
            {
                isHovered = false;
                setMouseCursor(juce::MouseCursor::NormalCursor);
                repaint();
            }

            void mouseDown(const juce::MouseEvent& e) override
            {
                dragStartY       = e.getScreenPosition().y;
                pitchAtDragStart = getCurrentPitch ? getCurrentPitch() : 0;
                isDragging       = true;
                repaint();
            }

            void mouseDrag(const juce::MouseEvent& e) override
            {
                // drag UP (negative delta-Y on screen) → pitch up
                // Each 5 px = 1 step; step size determined by getPitchStep() in cents
                int deltaY   = dragStartY - e.getScreenPosition().y;
                int stepSize = getPitchStep ? getPitchStep() : 100;
                int newPitch = juce::jlimit(-4800, 4800, pitchAtDragStart + (deltaY / 5) * stepSize);
                if (onPitchDragged)
                    onPitchDragged(newPitch);
            }

            void mouseUp(const juce::MouseEvent& e) override
            {
                // Compute the same final value as mouseDrag so it is consistent
                int deltaY   = dragStartY - e.getScreenPosition().y;
                int stepSize = getPitchStep ? getPitchStep() : 100;
                int newPitch = juce::jlimit(-4800, 4800, pitchAtDragStart + (deltaY / 5) * stepSize);
                if (onPitchDragged)
                    onPitchDragged(newPitch);
                if (onDragFinished)
                    onDragFinished();
                isDragging = false;
                repaint();
            }

            // Double-click resets pitch to 0 with a brief white flash to confirm.
            void mouseDoubleClick(const juce::MouseEvent&) override
            {
                if (onPitchDragged) onPitchDragged(0);
                if (onDragFinished) onDragFinished();
                flashActive = true;
                repaint();
                juce::Timer::callAfterDelay(150, [safeThis = juce::Component::SafePointer<DraggablePitchLabel>(this)]()
                {
                    if (auto* p = safeThis.getComponent()) { p->flashActive = false; p->repaint(); }
                });
            }

            // Override paint to add hover/drag highlight glow
            void paint(juce::Graphics& g) override
            {
                juce::Label::paint(g);
                if (flashActive)
                {
                    // Bright white fill flash on double-click reset
                    g.setColour(juce::Colour(0x88FFFFFF));
                    g.fillRect(getLocalBounds().reduced(1));
                }
                else if (isHovered || isDragging)
                {
                    // Subtle white outline glow to signal interactivity
                    g.setColour(juce::Colour(0x44FFFFFF));
                    g.drawRect(getLocalBounds().reduced(1), 1);
                }
            }

        private:
            int  dragStartY       = 0;
            int  pitchAtDragStart = 0;
            bool isHovered        = false;
            bool isDragging       = false;
            bool flashActive      = false;
        };

        // Timer for pulsing the 1Shot button while a one-shot tail is completing.
        class OneShotPulseTimer : public juce::Timer
        {
        public:
            OneShotPulseTimer(SampleCard& owner) : owner(owner) {}
            void timerCallback() override { owner.oneShotPulseTick(); }
        private:
            SampleCard& owner;
        };

        // Scrollbar height constant (Windows default ~14px, macOS ~15px)
        static constexpr int SCROLLBAR_HEIGHT = 14;
        // High-resolution WaveformComponent that renders directly from audio data
        // In SampleCard.h - Replace the WaveformComponent class with this fixed version

        class WaveformComponent : public juce::Component,
                                private juce::Timer
        {
        public:
            WaveformComponent(juce::AudioFormatManager& formatManager,
                            juce::File& currentAudioFile,
                            int& pitchOffsetRef)
                : formatManager(formatManager),
                currentAudioFile(currentAudioFile),
                pitchOffset(pitchOffsetRef)
            {
                setOpaque(true);
                startTimer(100);
            }

            void setFile(const juce::File& newFile)
            {
                // Always clear loading state — even if the file is the same one.
                // The guard below only skips cache invalidation, but the overlay must
                // always disappear when a file (re-)arrives.
                isLoading = false;
                // Hide playhead — new sample hasn't started playing yet
                playheadNormalized = -1.0f;
                prevPlayheadX      = -1;
                printf("[LOADING] setLoading(false) called for '%s'\n",
                       newFile.getFileName().toRawUTF8());

                if (currentAudioFile != newFile)
                {
                    currentAudioFile = newFile;

                    // Invalidate pre-computed peaks — new peaks arrive via setAudioPeaks()
                    peaksReady.store(false);

                    // Clear legacy reader cache (kept for detectTransients compatibility)
                    cachedReader.reset();
                    cachedTotalLength = 0;
                    cachedNumChannels = 0;
                    lastFile = juce::File();
                }

                repaint();  // Always repaint so the loading overlay is cleared
            }

            // Show a "Loading..." overlay. Cleared automatically when setFile() is called.
            void setLoading(bool loading)
            {
                isLoading = loading;
                repaint();
            }

            void paint(juce::Graphics& g) override
            {
                auto bounds = getLocalBounds();
                
                // Fill background — light gray
                g.setColour(juce::Colour(0xFFE0E0E0));
                g.fillRect(bounds);

                // Draw border
                g.setColour(juce::Colour(0xFFB0B0B0));
                g.drawRect(bounds, 2);

                // Loading indicator — shown while background thread reads the new file
                if (isLoading)
                {
                    g.setColour(juce::Colour(0xFFD0D0D0));
                    g.fillRect(bounds);
                    g.setColour(juce::Colour(0xFF555555));
                    g.setFont(juce::Font(13.0f, juce::Font::italic));
                    g.drawText("Loading...", bounds, juce::Justification::centred, true);
                    return;
                }

                if (!currentAudioFile.existsAsFile())
                {
                    g.setColour(juce::Colour(0xFF888888));
                    g.setFont(juce::Font(14.0f, juce::Font::italic));
                    g.drawText("No waveform", bounds, juce::Justification::centred, true);
                    return;
                }

                // Use pre-computed peaks — zero disk I/O on the message thread.
                // Peaks are built on the background thread before callAsync and set via
                // setAudioPeaks(). Until they arrive paint() shows a brief placeholder.
                if (!peaksReady.load())
                {
                    g.setColour(juce::Colour(0xFFD0D0D0));
                    g.fillRect(bounds);
                    g.setColour(juce::Colour(0xFF555555));
                    g.setFont(juce::Font(13.0f, juce::Font::italic));
                    g.drawText("Building waveform...", bounds, juce::Justification::centred, true);
                    return;
                }

                // Split inner area: top strip = time ruler, remaining = waveform
                const int rulerHeight = 16;
                auto innerBounds = bounds.reduced(2);
                if (innerBounds.isEmpty())
                    return;

                // Dimensions derived from pre-computed peak metadata (no file read)
                const double originalDuration = (peakSampleRate > 0.0)
                    ? (double)peakTotalSamples / peakSampleRate : 0.0;

                auto rulerBounds    = innerBounds.removeFromTop(rulerHeight);
                auto waveformBounds = innerBounds;

                int renderWidth  = waveformBounds.getWidth();
                int renderHeight = waveformBounds.getHeight();
                int renderTop    = waveformBounds.getY();
                int renderBottom = waveformBounds.getBottom();
                int renderCenter = waveformBounds.getCentreY();

                double totalLength = static_cast<double>(peakTotalSamples);
                int    numChannels = peakNumChannels;

                if (totalLength <= 0 || numChannels <= 0)
                {
                    g.setColour(juce::Colour(0xFF888888));
                    g.setFont(juce::Font(14.0f, juce::Font::italic));
                    g.drawText("Invalid audio data", bounds, juce::Justification::centred, true);
                    return;
                }
                
                // ===== CRITICAL FIX #2: Calculate samplesPerPixel correctly =====
                double samplesPerPixel;
                double expansionFactor = 1.0;
                
                if (pitchOffset < 0) // Pitch DOWN - EXPANDED view
                {
                    expansionFactor = std::pow(2.0, std::abs(pitchOffset) / 1200.0);
                    expansionFactor = juce::jmin(expansionFactor, 16.0);

                    // renderWidth is already viewportWidth * expansionFactor (set in updateWaveformSize)
                    // so simply divide totalLength by renderWidth to get samples per pixel
                    samplesPerPixel = totalLength / renderWidth;

                    printf("EXPANDED: pitch=%d, expansion=%.2fx, renderWidth=%d, samplesPerPixel=%.4f, totalSamples=%lld\n",
                        pitchOffset, expansionFactor, renderWidth, samplesPerPixel, peakTotalSamples);
                }
                else if (pitchOffset > 0) // Pitch UP - COMPRESSED view
                {
                    double compressionFactor = std::pow(2.0, (double)pitchOffset / 1200.0);
                    compressionFactor = juce::jmin(compressionFactor, 16.0);
                    samplesPerPixel = (totalLength / renderWidth) * compressionFactor;
                }
                else // Normal view
                {
                    samplesPerPixel = totalLength / renderWidth;
                }
                
                // ===== DRAW MAIN WAVEFORM =====
                if (numChannels > 1)
                {
                    // STEREO - Draw channels in separate vertical spaces
                    juce::Path leftPath, rightPath;
                    
                    int halfHeight = renderHeight / 2;
                    int leftTop = renderTop;
                    int leftBottom = renderTop + halfHeight;
                    int rightTop = renderTop + halfHeight;
                    int rightBottom = renderBottom;
                    
                    // Use pre-computed peaks — array lookup only, no disk I/O
                    for (int x = 0; x < renderWidth; ++x)
                    {
                        if ((double)x * samplesPerPixel >= totalLength)
                            break;

                        float leftMin, leftMax, rightMin, rightMax;
                        getPeaksForPixel(x, samplesPerPixel, leftMin, leftMax, rightMin, rightMax);

                        float xPos = waveformBounds.getX() + x;

                        // Left channel
                        float leftCenterY    = leftTop + halfHeight * 0.5f;
                        float leftHalfHeight = halfHeight * 0.5f;
                        float leftYTop    = juce::jlimit((float)leftTop + 1.0f, (float)leftBottom - 1.0f,
                                                          leftCenterY - leftMax * leftHalfHeight);
                        float leftYBottom = juce::jlimit((float)leftTop + 1.0f, (float)leftBottom - 1.0f,
                                                          leftCenterY - leftMin * leftHalfHeight);
                        leftPath.startNewSubPath(xPos, leftYTop);
                        leftPath.lineTo(xPos, leftYBottom);

                        // Right channel
                        float rightCenterY    = rightTop + halfHeight * 0.5f;
                        float rightHalfHeight = halfHeight * 0.5f;
                        float rightYTop    = juce::jlimit((float)rightTop + 1.0f, (float)rightBottom - 1.0f,
                                                           rightCenterY - rightMax * rightHalfHeight);
                        float rightYBottom = juce::jlimit((float)rightTop + 1.0f, (float)rightBottom - 1.0f,
                                                           rightCenterY - rightMin * rightHalfHeight);
                        rightPath.startNewSubPath(xPos, rightYTop);
                        rightPath.lineTo(xPos, rightYBottom);
                    }
                    
                    // Left channel — dark charcoal gradient, lighter at edges
                    {
                        juce::ColourGradient grad(juce::Colour(0xFF606060), 0.0f, (float)leftTop,
                                                  juce::Colour(0xFF606060), 0.0f, (float)leftBottom, false);
                        grad.addColour(0.5, juce::Colour(0xFF1A1A1A));
                        g.setGradientFill(grad);
                        g.strokePath(leftPath, juce::PathStrokeType(1.5f));
                    }

                    // Right channel — same gradient in its vertical range
                    {
                        juce::ColourGradient grad(juce::Colour(0xFF606060), 0.0f, (float)rightTop,
                                                  juce::Colour(0xFF606060), 0.0f, (float)rightBottom, false);
                        grad.addColour(0.5, juce::Colour(0xFF1A1A1A));
                        g.setGradientFill(grad);
                        g.strokePath(rightPath, juce::PathStrokeType(1.5f));
                    }

                    // Channel divider
                    g.setColour(juce::Colour(0xFF555555));
                    g.drawHorizontalLine(renderTop + halfHeight,
                                        waveformBounds.getX(), waveformBounds.getRight());

                    g.setColour(juce::Colour(0xFF555555));
                    g.setFont(juce::Font(10.0f));
                    g.drawText("L", waveformBounds.getX() + 5, leftTop + 2, 20, 15,
                            juce::Justification::left);
                    g.drawText("R", waveformBounds.getX() + 5, rightTop + 2, 20, 15,
                            juce::Justification::left);
                }
                else
                {
                    // MONO - draw single waveform using pre-computed peaks
                    juce::Path waveformPath;

                    for (int x = 0; x < renderWidth; ++x)
                    {
                        if ((double)x * samplesPerPixel >= totalLength)
                            break;

                        float minVal, maxVal, dummyR1, dummyR2;
                        getPeaksForPixel(x, samplesPerPixel, minVal, maxVal, dummyR1, dummyR2);

                        float xPos      = waveformBounds.getX() + x;
                        float centerY   = renderTop + renderHeight * 0.5f;
                        float halfHt    = renderHeight * 0.5f;
                        float yTop      = juce::jlimit((float)renderTop + 1.0f, (float)renderBottom - 1.0f,
                                                        centerY - maxVal * halfHt);
                        float yBottom   = juce::jlimit((float)renderTop + 1.0f, (float)renderBottom - 1.0f,
                                                        centerY - minVal * halfHt);
                        waveformPath.startNewSubPath(xPos, yTop);
                        waveformPath.lineTo(xPos, yBottom);
                    }
                    
                    // Dark charcoal gradient — lighter at top/bottom edges, darkest in center
                    {
                        juce::ColourGradient grad(juce::Colour(0xFF606060), 0.0f, (float)renderTop,
                                                  juce::Colour(0xFF606060), 0.0f, (float)renderBottom, false);
                        grad.addColour(0.5, juce::Colour(0xFF1A1A1A));
                        g.setGradientFill(grad);
                        g.strokePath(waveformPath, juce::PathStrokeType(1.5f));
                    }
                }
                
                // Draw center line
                g.setColour(juce::Colour(0xFF555555).withAlpha(0.5f));
                g.drawHorizontalLine(renderCenter, waveformBounds.getX(), waveformBounds.getRight());

                // ===== TIME RULER AND GRID LINES =====
                float actualWaveformWidth = getActualWaveformWidth(renderWidth);

                if (originalDuration > 0.0 && actualWaveformWidth > 0.0f)
                {
                    float pixelsPerSecond = actualWaveformWidth / (float)originalDuration;
                    double majorInterval, minorInterval;
                    getTickIntervals(pixelsPerSecond, majorInterval, minorInterval);

                    // --- Grid lines over waveform — use selected resolution when snap is ON ---
                    double gridMajor, gridMinor;
                    if (gridSnapActive && gridResolutionSeconds > 0.0)
                    {
                        gridMajor = gridResolutionSeconds;
                        gridMinor = gridResolutionSeconds / 2.0;
                    }
                    else
                    {
                        gridMajor = majorInterval;
                        gridMinor = minorInterval;
                    }

                    float majorPx = (gridMajor > 0.0 && originalDuration > 0.0)
                                    ? (float)(gridMajor / originalDuration) * actualWaveformWidth
                                    : 0.0f;
                    float minorPx = majorPx * 0.5f;

                    if (majorPx >= 2.0f)
                    {
                        for (double t = gridMinor; t < originalDuration; t += gridMinor)
                        {
                            int n = (int)std::round(t / gridMinor);
                            bool isMajor = (n % 2 == 0);
                            if (minorPx < 2.0f && !isMajor) continue; // skip minor when too dense
                            float x = waveformBounds.getX() + (float)(t / originalDuration * actualWaveformWidth);
                            if (x >= waveformBounds.getRight()) break;
                            g.setColour(juce::Colour(0xFF000000).withAlpha(isMajor ? 0.15f : 0.08f));
                            g.drawVerticalLine((int)(x + 0.5f), waveformBounds.getY(), waveformBounds.getBottom());
                        }
                    }

                    // --- Ruler background — slightly darker than waveform ---
                    g.setColour(juce::Colour(0xFFC8C8C8));
                    g.fillRect(rulerBounds);
                    g.setColour(juce::Colour(0xFFAAAAAA));
                    g.drawRect(rulerBounds, 1);

                    g.setFont(juce::Font(9.0f));

                    // "0" at origin
                    g.setColour(juce::Colour(0xFF2A2A2A));
                    g.drawVerticalLine(rulerBounds.getX(),
                                       rulerBounds.getBottom() - 8, rulerBounds.getBottom() - 1);
                    g.drawText("0", rulerBounds.getX() + 2, rulerBounds.getY(),
                               20, rulerHeight - 2, juce::Justification::centredLeft, false);

                    // Minor and major ticks + labels
                    for (double t = minorInterval; t < originalDuration; t += minorInterval)
                    {
                        int n = (int)std::round(t / minorInterval);
                        bool isMajor = (n % 2 == 0);
                        float x = rulerBounds.getX() + (float)(t / originalDuration * actualWaveformWidth);
                        if (x >= rulerBounds.getRight()) break;

                        g.setColour(isMajor ? juce::Colour(0xFF2A2A2A) : juce::Colour(0xFF555555));
                        int tickH = isMajor ? 8 : 4;
                        g.drawVerticalLine((int)(x + 0.5f),
                                           rulerBounds.getBottom() - tickH, rulerBounds.getBottom() - 1);

                        if (isMajor)
                        {
                            double rounded = std::round(t * 1000.0) / 1000.0;
                            juce::String label;
                            if (rounded >= 10.0 || rounded == (double)(int)(rounded + 0.5))
                                label = juce::String((int)(rounded + 0.5));
                            else
                                label = juce::String(rounded, majorInterval < 0.1 ? 2 : 1);

                            g.setColour(juce::Colour(0xFF2A2A2A));
                            g.drawText(label, (int)(x + 0.5f) - 15, rulerBounds.getY(),
                                       30, rulerHeight - 4, juce::Justification::centred, false);
                        }
                    }
                }

                // Draw transient positions as small tick marks at waveform top
                if (!transientPositionsNormalized.empty())
                {
                    g.setColour(juce::Colour(0xFFE84A1A).withAlpha(0.75f));
                    for (float tn : transientPositionsNormalized)
                    {
                        float tx = waveformBounds.getX() + tn * actualWaveformWidth;
                        if (tx >= waveformBounds.getRight()) break;
                        g.drawVerticalLine((int)(tx + 0.5f),
                                           waveformBounds.getY(),
                                           waveformBounds.getY() + 6);
                        // Small downward triangle cap
                        juce::Path tick;
                        tick.addTriangle(tx - 2.5f, (float)waveformBounds.getY(),
                                         tx + 2.5f, (float)waveformBounds.getY(),
                                         tx,         (float)waveformBounds.getY() + 5.0f);
                        g.fillPath(tick);
                    }
                }

                // Draw loop region highlight (semi-transparent overlay between start and end)
                if (loopHighlightEnabled)
                {
                    float hlStartX = waveformBounds.getX() + startMarkerNormalized * actualWaveformWidth;
                    float hlEndX   = waveformBounds.getX() + endMarkerNormalized   * actualWaveformWidth;
                    g.setColour(juce::Colour(0xFFB4FF00).withAlpha(0.20f)); // rgba(180,255,0,0.20)
                    g.fillRect(juce::Rectangle<float>(hlStartX, (float)waveformBounds.getY(),
                                                      hlEndX - hlStartX, (float)waveformBounds.getHeight()));
                }

                // ===== ADSR ENVELOPE OVERLAY =====
                if (adsrOverlayEnabled && originalDuration > 0.0)
                {
                    float startX  = waveformBounds.getX() + startMarkerNormalized * actualWaveformWidth;
                    float endX    = waveformBounds.getX() + endMarkerNormalized   * actualWaveformWidth;
                    float regionW = endX - startX;
                    if (regionW > 2.0f)
                    {
                        double regionDur = (double)(endMarkerNormalized - startMarkerNormalized) * originalDuration;
                        float yTop    = (float)waveformBounds.getY();
                        float yBottom = (float)waveformBounds.getBottom();
                        float sustY   = yBottom - (yBottom - yTop) * juce::jlimit(0.0f, 1.0f, adsrSustain);

                        float atkFrac  = (regionDur > 0.0) ? juce::jlimit(0.0f, 1.0f, (adsrAttackMs  / 1000.0f) / (float)regionDur) : 0.0f;
                        float dcyFrac  = (regionDur > 0.0) ? juce::jlimit(0.0f, 1.0f - atkFrac, (adsrDecayMs   / 1000.0f) / (float)regionDur) : 0.0f;
                        float relFrac  = (regionDur > 0.0) ? juce::jlimit(0.0f, 1.0f, (adsrReleaseMs / 1000.0f) / (float)regionDur) : 0.0f;

                        float atkX      = startX + atkFrac * regionW;
                        float dcyX      = atkX   + dcyFrac * regionW;
                        float relStartX = juce::jmax(dcyX, endX - relFrac * regionW);

                        juce::Path envPath;
                        envPath.startNewSubPath(startX,    yBottom);
                        envPath.lineTo         (atkX,      yTop);
                        envPath.lineTo         (dcyX,      sustY);
                        envPath.lineTo         (relStartX, sustY);
                        envPath.lineTo         (endX,      yBottom);
                        envPath.closeSubPath();

                        g.setColour(juce::Colour(0xFFFF00C8).withAlpha(0.25f)); // fuchsia fill
                        g.fillPath(envPath);
                        g.setColour(juce::Colour(0xFFFF00C8).withAlpha(0.60f)); // fuchsia outline
                        g.strokePath(envPath, juce::PathStrokeType(1.5f));
                    }
                }

                // Draw start point marker — deep red normally, flash orange on transient snap
                // Flash alternates bright/dim each 100ms tick for a pulse effect
                bool flashOn = (markerFlashCountdown > 0) && (markerFlashCountdown % 2 != 0);
                juce::Colour startMarkerColour = flashOn
                    ? juce::Colour(0xFFE84A1A)  // orange-red flash matching start snap buttons
                    : juce::Colour(0xFFCC0000); // normal deep red
                float markerX = waveformBounds.getX() + startMarkerNormalized * actualWaveformWidth;
                g.setColour(startMarkerColour.withAlpha(0.9f));
                g.drawLine(markerX, (float)rulerBounds.getY(),
                           markerX, (float)waveformBounds.getBottom(), 2.0f);
                {
                    juce::Path handle;
                    handle.addTriangle(markerX - 5, (float)rulerBounds.getY(),
                                       markerX + 5, (float)rulerBounds.getY(),
                                       markerX,     (float)rulerBounds.getY() + 8);
                    g.fillPath(handle);
                }

                // Draw end point marker — cyan normally, flash orange on transient snap
                bool endFlashOn = (endMarkerFlashCountdown > 0) && (endMarkerFlashCountdown % 2 != 0);
                juce::Colour endMarkerColour = endFlashOn
                    ? juce::Colour(0xFF00B4D8)  // cyan flash matching end snap buttons
                    : juce::Colour(0xFF00AACC); // normal cyan
                float endMarkerX = waveformBounds.getX() + endMarkerNormalized * actualWaveformWidth;
                g.setColour(endMarkerColour.withAlpha(0.9f));
                g.drawLine(endMarkerX, (float)rulerBounds.getY(),
                           endMarkerX, (float)waveformBounds.getBottom(), 2.0f);
                {
                    juce::Path endHandle;
                    endHandle.addTriangle(endMarkerX - 5, (float)waveformBounds.getBottom(),
                                          endMarkerX + 5, (float)waveformBounds.getBottom(),
                                          endMarkerX,     (float)waveformBounds.getBottom() - 8);
                    g.fillPath(endHandle);
                }

                // ===== ZOOM INDICATOR — top-left corner of ruler, tracks visible area =====
                if (zoomLevel > 1.005)
                {
                    juce::String zoomText = juce::String(zoomLevel, 1) + "x";
                    auto indicRect = juce::Rectangle<int>(viewScrollX + 4, rulerBounds.getY() + 1,
                                                          44, rulerHeight - 2);
                    g.setColour(juce::Colour(0xCC1E1E1E));
                    g.fillRoundedRectangle(indicRect.toFloat(), 2.0f);
                    g.setColour(juce::Colour(0xFFAAFF00));
                    g.setFont(juce::Font(9.0f, juce::Font::bold));
                    g.drawText(zoomText, indicRect, juce::Justification::centred, false);
                }

                // ===== PLAYHEAD — drawn on top of everything =====
                if (playheadNormalized >= 0.0f && playheadNormalized <= 1.0f)
                {
                    const float phX = waveformBounds.getX() + playheadNormalized * actualWaveformWidth;

                    // Thin black vertical line spanning full waveform height
                    g.setColour(juce::Colour(0xFF000000));
                    g.drawLine(phX, (float)waveformBounds.getY(),
                               phX, (float)waveformBounds.getBottom(), 1.5f);

                    // Arrow indicator: directional left/right in bounce mode, downward otherwise.
                    juce::Path phTri;
                    if (playheadBounceMode)
                    {
                        // Directional triangle pointing in the current travel direction.
                        const float ry = (float)rulerBounds.getY() + 1.0f;
                        if (playheadDir >= 0)
                        {
                            // Right-pointing arrow ▶
                            phTri.addTriangle(phX,        ry,
                                              phX,        ry + 8.0f,
                                              phX + 6.0f, ry + 4.0f);
                        }
                        else
                        {
                            // Left-pointing arrow ◀
                            phTri.addTriangle(phX,        ry,
                                              phX,        ry + 8.0f,
                                              phX - 6.0f, ry + 4.0f);
                        }
                    }
                    else
                    {
                        // Default: small downward triangle at the top of the ruler
                        phTri.addTriangle(phX - 3.0f, (float)rulerBounds.getY(),
                                          phX + 3.0f, (float)rulerBounds.getY(),
                                          phX,        (float)rulerBounds.getY() + 6.0f);
                    }
                    g.fillPath(phTri);
                }
            }

            void setPitchFactor(double factor, int semitones)
            {
                currentPitchFactor = factor;
                currentSemitones = semitones;
                repaint();
            }

            // Set start marker position (0.0 = start, 1.0 = end).
            // Uses dirty-region repaint (only the affected vertical strip) when ADSR overlay
            // is inactive, so the message thread stays free during continuous knob drag.
            // Falls back to full repaint when ADSR overlay is on (its shape spans the region).
            void setStartMarker(float normalized)
            {
                const float newNorm = juce::jlimit(0.0f, endMarkerNormalized - 0.001f, normalized);
                if (newNorm == startMarkerNormalized) return;

                if (adsrOverlayEnabled)
                {
                    startMarkerNormalized = newNorm;
                    repaint();
                    return;
                }

                auto fullBounds  = getLocalBounds();
                auto innerBounds = fullBounds.reduced(2);
                const float actualWidth = getActualWaveformWidth(innerBounds.getWidth());
                const int margin = 3;
                const int oldX = innerBounds.getX() + (int)(startMarkerNormalized * actualWidth);
                const int newX = innerBounds.getX() + (int)(newNorm * actualWidth);
                startMarkerNormalized = newNorm;

                // Union of old and new strips — covers loop-highlight left-edge change too
                const int x1 = juce::jmin(oldX, newX) - margin;
                const int x2 = juce::jmax(oldX, newX) + margin + 2;
                repaint(x1, fullBounds.getY(), x2 - x1, fullBounds.getHeight());
            }

            float getStartMarker() const { return startMarkerNormalized; }

            // Set end marker position (0.0 = start, 1.0 = end).
            // Same dirty-region optimisation as setStartMarker.
            void setEndMarker(float normalized)
            {
                const float newNorm = juce::jlimit(startMarkerNormalized + 0.001f, 1.0f, normalized);
                if (newNorm == endMarkerNormalized) return;

                if (adsrOverlayEnabled)
                {
                    endMarkerNormalized = newNorm;
                    repaint();
                    return;
                }

                auto fullBounds  = getLocalBounds();
                auto innerBounds = fullBounds.reduced(2);
                const float actualWidth = getActualWaveformWidth(innerBounds.getWidth());
                const int margin = 3;
                const int oldX = innerBounds.getX() + (int)(endMarkerNormalized * actualWidth);
                const int newX = innerBounds.getX() + (int)(newNorm * actualWidth);
                endMarkerNormalized = newNorm;

                // Union of old and new strips — covers loop-highlight right-edge change too
                const int x1 = juce::jmin(oldX, newX) - margin;
                const int x2 = juce::jmax(oldX, newX) + margin + 2;
                repaint(x1, fullBounds.getY(), x2 - x1, fullBounds.getHeight());
            }

            float getEndMarker() const { return endMarkerNormalized; }

            void setLoopHighlight(bool enabled)
            {
                loopHighlightEnabled = enabled;
                repaint();
            }

            // Push snap-grid state so paint() draws lines at the chosen resolution.
            void setGridResolution(bool snapActive, double resolutionSec)
            {
                gridSnapActive       = snapActive;
                gridResolutionSeconds = resolutionSec;
                repaint();
            }

            // Transient markers — normalized positions [0,1]
            void setTransients(const std::vector<float>& positions)
            {
                transientPositionsNormalized = positions;
                repaint();
            }

            // Briefly flash the start marker orange to show a transient snap occurred
            void flashStartMarker()
            {
                markerFlashCountdown = 6; // ~600ms at 100ms timer intervals
                repaint();
            }

            // Briefly flash the end marker orange to show a transient snap occurred
            void flashEndMarker()
            {
                endMarkerFlashCountdown = 6;
                repaint();
            }

            void setZoomLevel(double z) { zoomLevel = z; }

            void setScrollOffset(int scrollX) { viewScrollX = scrollX; }

            // Set pre-computed peak data (computed on background thread before callAsync).
            // After this call paint() has everything it needs — zero disk I/O on message thread.
            void setAudioPeaks(std::unique_ptr<WaveformPeakBin[]> peaks,
                               int numCh, juce::int64 numSamples, double sampleRate)
            {
                peakBinData      = std::move(peaks);
                peakNumChannels  = numCh;
                peakTotalSamples = numSamples;
                peakSampleRate   = sampleRate;
                peaksReady.store(true);
                printf("[LOAD-TIMING] setAudioPeaks() — %d ch  %lld samples  %.1f Hz  peaks ready\n",
                       numCh, numSamples, sampleRate);
            }

            // Called by SampleCard's updatePlayhead() before setPlayheadPosition().
            // Stores bounce mode and direction so paint() draws the correct arrow.
            // Triggers a repaint of the playhead strip when direction changes.
            void setPlayheadBounceAndDirection(int dir, bool bounceActive)
            {
                const bool dirChanged    = (dir != playheadDir);
                const bool modeChanged   = (bounceActive != playheadBounceMode);
                playheadDir        = dir;
                playheadBounceMode = bounceActive;
                // Repaint playhead strip so the arrow updates in the current frame.
                if ((dirChanged || modeChanged) && prevPlayheadX >= 0)
                {
                    const int margin = 8; // wider margin to cover the 6px triangle
                    repaint(prevPlayheadX - margin, getLocalBounds().getY(),
                            2 * margin + 2, getLocalBounds().getHeight());
                }
            }

            // Called by SampleCard's 60fps PlayheadTimer.
            // normalizedPos is the audio playback position mapped to [0,1] over the full sample.
            // Pass -1 to hide the playhead (no note playing).
            // Only repaints the thin dirty strip where the playhead moved — not the full waveform.
            void setPlayheadPosition(float normalizedPos)
            {
                if (normalizedPos == playheadNormalized) return;

                auto fullBounds   = getLocalBounds();
                auto innerBounds  = fullBounds.reduced(2);
                innerBounds.removeFromTop(16); // ruler height
                int renderWidth   = innerBounds.getWidth();
                float actualWidth = getActualWaveformWidth(renderWidth);

                const int margin = 3; // px either side of the 1.5px line

                // Erase previous strip
                if (prevPlayheadX >= 0)
                    repaint(prevPlayheadX - margin, fullBounds.getY(),
                            2 * margin + 2, fullBounds.getHeight());

                playheadNormalized = normalizedPos;

                if (normalizedPos >= 0.0f && normalizedPos <= 1.0f)
                {
                    int px = innerBounds.getX() + (int)(normalizedPos * actualWidth);
                    repaint(px - margin, fullBounds.getY(),
                            2 * margin + 2, fullBounds.getHeight());
                    prevPlayheadX = px;
                }
                else
                {
                    prevPlayheadX = -1;
                }
            }

            // ADSR envelope overlay — drawn as semi-transparent fuchsia shape over the waveform.
            // Uses startMarkerNormalized / endMarkerNormalized already stored in this component.
            // Dirty-region repaint: only repaints the start-to-end marker area so the full
            // waveform peak rendering is clipped to that region — fast enough for real-time
            // drag updates without needing to suppress repaints during knob drag.
            void setAdsrOverlay(bool enabled, float atkMs, float dcyMs, float sus, float relMs)
            {
                adsrOverlayEnabled = enabled;
                adsrAttackMs  = atkMs;
                adsrDecayMs   = dcyMs;
                adsrSustain   = sus;
                adsrReleaseMs = relMs;
                // When enabled state changes, do a full repaint to clear/show the shape.
                // During drag (enabled stays true), use dirty region for performance.
                if (enabled && getWidth() > 4)
                {
                    float renderW = (float)(getWidth() - 4);
                    float actualW = getActualWaveformWidth((int)renderW);
                    int x1 = juce::jmax(0,          (int)(2.0f + startMarkerNormalized * actualW) - 4);
                    int x2 = juce::jmin(getWidth(),  (int)(2.0f + endMarkerNormalized   * actualW) + 4);
                    repaint(x1, 0, x2 - x1, getHeight());
                }
                else
                {
                    repaint();  // full repaint when disabling to clear the overlay
                }
            }

            // Callbacks: invoked when user drags either marker
            std::function<void(float)> onMarkerDragged;
            std::function<void(float)> onEndMarkerDragged;

            // Zoom callbacks
            std::function<void(double factor, int mouseXInComponent)> onZoomChanged;
            std::function<void()> onZoomReset;

            void mouseDown(const juce::MouseEvent& event) override
            {
                if (event.getNumberOfClicks() > 1)
                    return; // double-click handled by mouseDoubleClick

                // Pick the marker closest to the click position
                auto bounds = getLocalBounds().reduced(2);
                float actualWidth = getActualWaveformWidth(bounds.getWidth());
                float startX = bounds.getX() + startMarkerNormalized * actualWidth;
                float endX   = bounds.getX() + endMarkerNormalized   * actualWidth;
                float dStart = std::abs((float)event.x - startX);
                float dEnd   = std::abs((float)event.x - endX);
                currentDragTarget = (dStart <= dEnd) ? DragTarget::Start : DragTarget::End;
                updateMarkerFromMouse(event.x);
            }

            void mouseDrag(const juce::MouseEvent& event) override
            {
                if (currentDragTarget != DragTarget::None)
                    updateMarkerFromMouse(event.x);
            }

            void mouseUp(const juce::MouseEvent& event) override
            {
                currentDragTarget = DragTarget::None;
            }

            void mouseWheelMove(const juce::MouseEvent& event,
                                const juce::MouseWheelDetails& wheel) override
            {
                // Vertical scroll → zoom in/out centered on cursor
                if (wheel.deltaY != 0.0f && onZoomChanged)
                {
                    const double factor = wheel.deltaY > 0.0f ? 1.2 : (1.0 / 1.2);
                    onZoomChanged(factor, event.x);
                }
                // Note: consume event — use scrollbar to pan when zoomed
            }

            void mouseDoubleClick(const juce::MouseEvent&) override
            {
                if (onZoomReset)
                    onZoomReset();
            }

        private:
            // Choose "nice" tick intervals so major ticks are at least 40px apart.
            static void getTickIntervals(float pixelsPerSecond, double& majorOut, double& minorOut)
            {
                static constexpr double niceVals[] = {
                    0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0
                };
                majorOut = 300.0;
                for (auto v : niceVals)
                {
                    if (pixelsPerSecond * (float)v >= 40.0f)
                    {
                        majorOut = v;
                        break;
                    }
                }
                minorOut = majorOut / 2.0;
            }

            // Returns the pixel width actually covered by waveform data.
            // For pitch-up the waveform only fills the left portion of renderWidth;
            // for pitch-down/normal the waveform fills the full renderWidth.
            float getActualWaveformWidth(int renderWidth) const
            {
                if (pitchOffset > 0)
                {
                    double compressionFactor = std::pow(2.0, (double)pitchOffset / 1200.0);
                    compressionFactor = juce::jmin(compressionFactor, 16.0);
                    return (float)(renderWidth / compressionFactor);
                }
                return (float)renderWidth;
            }

            void updateMarkerFromMouse(int mouseX)
            {
                auto bounds = getLocalBounds().reduced(2);
                if (bounds.getWidth() <= 0) return;
                float actualWidth = getActualWaveformWidth(bounds.getWidth());
                float rawNorm = (mouseX - bounds.getX()) / actualWidth;

                const float minGap  = 0.001f;
                const int   margin  = 3;

                if (currentDragTarget == DragTarget::Start)
                {
                    float newNorm = juce::jlimit(0.0f, endMarkerNormalized - minGap, rawNorm);
                    if (adsrOverlayEnabled)
                    {
                        startMarkerNormalized = newNorm;
                        repaint();
                    }
                    else
                    {
                        const int oldX = bounds.getX() + (int)(startMarkerNormalized * actualWidth);
                        const int newX = bounds.getX() + (int)(newNorm * actualWidth);
                        startMarkerNormalized = newNorm;
                        const int x1 = juce::jmin(oldX, newX) - margin;
                        const int x2 = juce::jmax(oldX, newX) + margin + 2;
                        repaint(x1, 0, x2 - x1, getHeight());
                    }
                    if (onMarkerDragged)
                        onMarkerDragged(newNorm);
                }
                else if (currentDragTarget == DragTarget::End)
                {
                    float newNorm = juce::jlimit(startMarkerNormalized + minGap, 1.0f, rawNorm);
                    if (adsrOverlayEnabled)
                    {
                        endMarkerNormalized = newNorm;
                        repaint();
                    }
                    else
                    {
                        const int oldX = bounds.getX() + (int)(endMarkerNormalized * actualWidth);
                        const int newX = bounds.getX() + (int)(newNorm * actualWidth);
                        endMarkerNormalized = newNorm;
                        const int x1 = juce::jmin(oldX, newX) - margin;
                        const int x2 = juce::jmax(oldX, newX) + margin + 2;
                        repaint(x1, 0, x2 - x1, getHeight());
                    }
                    if (onEndMarkerDragged)
                        onEndMarkerDragged(newNorm);
                }
            }

            void timerCallback() override
            {
                if (currentAudioFile != lastFile)
                {
                    cachedReader.reset();
                    lastFile = currentAudioFile;
                    repaint();
                }
                if (markerFlashCountdown > 0)
                {
                    --markerFlashCountdown;
                    repaint();
                }
                if (endMarkerFlashCountdown > 0)
                {
                    --endMarkerFlashCountdown;
                    repaint();
                }
            }

            juce::AudioFormatManager& formatManager;
            juce::File& currentAudioFile;
            int& pitchOffset;
            double currentPitchFactor = 1.0;
            int currentSemitones = 0;
            juce::File lastFile;
            std::unique_ptr<juce::AudioFormatReader> cachedReader;
            juce::int64 cachedTotalLength = 0;
            int cachedNumChannels = 0;
            float startMarkerNormalized = 0.0f;
            float endMarkerNormalized   = 1.0f;
            bool loopHighlightEnabled   = false;
            bool isLoading              = false;  // true while background thread reads new file

            // Grid snap display state
            bool   gridSnapActive        = false;
            double gridResolutionSeconds = 1.0;

            // Transient detection display
            std::vector<float> transientPositionsNormalized;
            int markerFlashCountdown    = 0; // start marker flash countdown (100ms ticks)
            int endMarkerFlashCountdown = 0; // end marker flash countdown

            enum class DragTarget { None, Start, End };
            DragTarget currentDragTarget = DragTarget::None;

            double zoomLevel   = 1.0;  // Current zoom level for indicator display
            int    viewScrollX = 0;    // Viewport scroll offset — updated via setScrollOffset()

            // Playhead — written from SampleCard's 60fps timer, read only in paint()
            float playheadNormalized  = -1.0f;  // -1 = hidden
            int   prevPlayheadX       = -1;     // pixel X from last frame, for dirty-region erase
            int   playheadDir         = 1;      // +1=forward, -1=backward (bounce mode only)
            bool  playheadBounceMode  = false;  // true while Bnc is active — draws directional arrow

            // ADSR envelope overlay state
            bool   adsrOverlayEnabled = false;
            float  adsrAttackMs   = 0.0f;
            float  adsrDecayMs    = 0.0f;
            float  adsrSustain    = 1.0f;
            float  adsrReleaseMs  = 0.0f;

            // Pre-computed peak data — set by setAudioPeaks(), used by paint()
            std::unique_ptr<WaveformPeakBin[]> peakBinData;
            std::atomic<bool> peaksReady { false };
            juce::int64 peakTotalSamples = 0;
            int         peakNumChannels  = 0;
            double      peakSampleRate   = 44100.0;

            // Look up min/max for display pixel x at the given samplesPerPixel ratio.
            // Spans one or more bins; returns symmetric values if only one channel stored.
            void getPeaksForPixel(int x, double samplesPerPixel,
                                  float& minL, float& maxL,
                                  float& minR, float& maxR) const noexcept
            {
                const double invTotal = (peakTotalSamples > 0)
                                        ? (double)kWaveformPeakBins / (double)peakTotalSamples
                                        : 0.0;
                const int b0 = juce::jlimit(0, kWaveformPeakBins - 1,
                                             (int)(x * samplesPerPixel * invTotal));
                const int b1 = juce::jlimit(b0 + 1, kWaveformPeakBins,
                                             (int)((x + 1) * samplesPerPixel * invTotal) + 1);
                minL = minR = 1.0f;
                maxL = maxR = -1.0f;
                for (int b = b0; b < b1; ++b)
                {
                    minL = juce::jmin(minL, peakBinData[b].minL);
                    maxL = juce::jmax(maxL, peakBinData[b].maxL);
                    minR = juce::jmin(minR, peakBinData[b].minR);
                    maxR = juce::jmax(maxR, peakBinData[b].maxR);
                }
            }

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
        };
    
    
    
    void adjustMidiNote(int delta)
    {
        int newNote = currentMidiNote + delta;
        
        // Constrain to valid MIDI range (0-127)
        if (newNote >= 0 && newNote <= 127)
        {
            currentMidiNote = newNote;
            updateMidiNoteDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            printf("MIDI note changed to: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
    }
    
    void adjustMidiChannel(int delta)
    {
        int newChannel = currentMidiChannel + delta;
        
        // Allow channel 0 for "All Channels", then 1-16
        if (delta > 0) // Incrementing
        {
            if (currentMidiChannel == 16)
                newChannel = 0; // Wrap to All Channels
            else if (currentMidiChannel == 0)
                newChannel = 1; // From All to channel 1
        }
        else if (delta < 0) // Decrementing
        {
            if (currentMidiChannel == 1)
                newChannel = 0; // Wrap to All Channels
            else if (currentMidiChannel == 0)
                newChannel = 16; // From All to channel 16
        }
        
        // Ensure valid range
        if (newChannel >= 0 && newChannel <= 16)
        {
            currentMidiChannel = newChannel;
            updateMidiChannelDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiChannelChanged(currentMidiChannel); });
            
            printf("MIDI channel changed to: %s\n", 
                   currentMidiChannel == 0 ? "All Channels" : juce::String(currentMidiChannel).toRawUTF8());
        }
    }
    
    void updateMidiNoteDisplay()
    {
        juce::String noteName = juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true);
        midiNoteLabel.setText(juce::String(currentMidiNote) + " (" + noteName + ")", 
                              juce::dontSendNotification);
    }
    
    void updateMidiChannelDisplay()
    {
        if (currentMidiChannel == 0)
            midiChannelLabel.setText("Ch All", juce::dontSendNotification);
        else
            midiChannelLabel.setText("Ch " + juce::String(currentMidiChannel), 
                                     juce::dontSendNotification);
    }
    
    void toggleLearnMode()
    {
        isLearning = !isLearning;
        learnButton.setToggleState(isLearning, juce::dontSendNotification);
        
        if (isLearning)
        {
            learnButton.setButtonText("Learning...");
            // Notify listeners that we're entering learn mode
            listeners.call([this](Listener& l) { l.learningModeChanged(true); });
        }
        else
        {
            learnButton.setButtonText("Learn");
            // Notify listeners that we're exiting learn mode
            listeners.call([this](Listener& l) { l.learningModeChanged(false); });
        }
    }
    
    void notifyStartPointChanged()
    {
        double seconds = (originalDuration > 0.0) ? startPointNormalized * originalDuration : 0.0;
        listeners.call([&](Listener& l) { l.startPointChanged(seconds); });
    }

    void notifyEndPointChanged()
    {
        double seconds = (originalDuration > 0.0) ? endPointNormalized * originalDuration : originalDuration;
        listeners.call([&](Listener& l) { l.endPointChanged(seconds); });
    }

    // Helper method to update pitch button tooltips
    void updatePitchButtonLabels()
    {
        // Update button tooltips to reflect the new behavior
        pitchDownButton.setTooltip("Lower pitch (longer duration)");
        pitchUpButton.setTooltip("Higher pitch (shorter duration)");
    }
    
void adjustPitchDown()
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();
    printf("[PITCH-TIMING] Down button clicked — current=%+d cents  step=%d cents\n",
           pitchOffset, currentPitchStepCents);
    fflush(stdout);

    int newOffset = pitchOffset - currentPitchStepCents;

    if (newOffset >= -4800 && newOffset <= 4800)
    {
        pitchOffset = newOffset;
        updatePitchDisplay(pitchOffset);  // FIX 5: visual update happens before listener call
        listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });

        const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
        printf("[PITCH-TIMING] Pitch updated atomically: %lldms  new=%+d cents\n",
               (long long)elapsed, pitchOffset);
        printf("[PITCH-TIMING] Save deferred: 500ms timer started\n");
        fflush(stdout);
    }
}

void adjustPitchUp()
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();
    printf("[PITCH-TIMING] Up button clicked — current=%+d cents  step=%d cents\n",
           pitchOffset, currentPitchStepCents);
    fflush(stdout);

    int newOffset = pitchOffset + currentPitchStepCents;

    if (newOffset >= -4800 && newOffset <= 4800)
    {
        pitchOffset = newOffset;
        updatePitchDisplay(pitchOffset);  // FIX 5: visual update happens before listener call
        listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });

        const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
        printf("[PITCH-TIMING] Pitch updated atomically: %lldms  new=%+d cents\n",
               (long long)elapsed, pitchOffset);
        printf("[PITCH-TIMING] Save deferred: 500ms timer started\n");
        fflush(stdout);
    }
}
    
    
    // =========================================================================
    // Transient detection and snapping

    // Analyse the audio file and build the transientPositionsSeconds list.
    // Uses a 512-sample sliding window; marks onset when RMS jumps > 4× previous window.
    // NOTE: duration is computed from the reader — does NOT depend on originalDuration so
    // this is safe to call from setWaveform() before setDuration() has been called.
    void detectTransients(const juce::File& audioFile)
    {
        transientPositionsSeconds.clear();
        if (waveformComponent != nullptr)
            waveformComponent->setTransients({});

        if (!audioFile.existsAsFile())
            return;

        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(audioFile));
        if (reader == nullptr)
            return;

        const double sr           = reader->sampleRate;
        const juce::int64 totalSamples = reader->lengthInSamples;

        // Compute duration directly from the reader — independent of originalDuration
        const double duration = (sr > 0.0 && totalSamples > 0)
                                    ? (double)totalSamples / sr
                                    : 0.0;
        if (duration <= 0.0)
            return;

        const int    windowSize    = 512;
        const double threshold     = transientThreshold;  // controlled by Sens knob
        const double minGapSeconds = 0.02;  // ignore transients closer than 20 ms

        const int numCh = juce::jmin((int)reader->numChannels, 2);

        juce::AudioBuffer<float> buf(numCh, windowSize);
        double prevRMS        = 0.0;
        double lastTransientT = -1.0;

        for (juce::int64 pos = 0; pos + windowSize <= totalSamples; pos += windowSize)
        {
            reader->read(&buf, 0, windowSize, pos, true, true);

            double sumSq = 0.0;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* data = buf.getReadPointer(ch);
                for (int i = 0; i < windowSize; ++i)
                    sumSq += (double)data[i] * data[i];
            }
            double rms = std::sqrt(sumSq / (windowSize * numCh));

            if (prevRMS > 0.001 && rms > prevRMS * threshold)
            {
                double t = (double)pos / sr;
                if (t - lastTransientT >= minGapSeconds)
                {
                    transientPositionsSeconds.push_back(t);
                    lastTransientT = t;
                }
            }
            prevRMS = rms;
        }

        const int transientCount = (int)transientPositionsSeconds.size();
        printf("[TRANSIENT] Startup/load complete — %d transients detected in '%s' (%.2fs, thresh=%.1fx)\n",
               transientCount,
               audioFile.getFileName().toRawUTF8(),
               duration,
               threshold);

        transientCountLabel.setText("T: " + juce::String(transientCount), juce::dontSendNotification);

        // Normalise against the reader-derived duration (not originalDuration)
        if (waveformComponent != nullptr)
        {
            std::vector<float> normalised;
            normalised.reserve(transientPositionsSeconds.size());
            for (double t : transientPositionsSeconds)
                normalised.push_back((float)(t / duration));
            waveformComponent->setTransients(normalised);
        }
    }

    // Move end marker to the nearest transient BEFORE current end (but after start marker).
    void snapEndToPrevTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentEnd   = endPointNormalized   * originalDuration;
        double currentStart = startPointNormalized  * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
        {
            // Must be before current end AND strictly after start marker
            if (t < currentEnd - epsilon && t > currentStart + epsilon)
                if (t > best) best = t;
        }

        if (best >= 0.0)
        {
            setEndPoint(best);
            notifyEndPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashEndMarker();
            printf("[TRANSIENT] End snap prev → %.3fs\n", best);
        }
        else
        {
            // Nothing valid — flash button briefly to indicate no target
            prevEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                prevEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
            });
        }
    }

    // Move end marker to the nearest transient AFTER current end.
    void snapEndToNextTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentEnd = endPointNormalized * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
        {
            if (t > currentEnd + epsilon && (best < 0.0 || t < best))
                best = t;
        }

        if (best >= 0.0 && best < originalDuration)
        {
            setEndPoint(best);
            notifyEndPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashEndMarker();
            printf("[TRANSIENT] End snap next → %.3fs\n", best);
        }
        else
        {
            nextEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                nextEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
            });
        }
    }

    // Move start marker to the nearest transient BEFORE current start.
    void snapToPrevTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentStart = startPointNormalized * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
            if (t < currentStart - epsilon && t > best)
                best = t;

        if (best >= 0.0)
        {
            setStartPoint(best);
            notifyStartPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashStartMarker();
            printf("[TRANSIENT] Start snap prev → %.3fs\n", best);
        }
        else
        {
            prevTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                prevTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
            });
        }
    }

    // Move start marker to the nearest transient AFTER current start (but before end marker).
    void snapToNextTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentStart = startPointNormalized * originalDuration;
        double currentEnd   = endPointNormalized   * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
        {
            // Must be after current start AND strictly before end marker
            if (t > currentStart + epsilon && t < currentEnd - epsilon)
                if (best < 0.0 || t < best) best = t;
        }

        if (best >= 0.0)
        {
            setStartPoint(best);
            notifyStartPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashStartMarker();
            printf("[TRANSIENT] Start snap next → %.3fs\n", best);
        }
        else
        {
            nextTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                nextTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
            });
        }
    }

/*     void updateWaveformSize()
    {
        if (waveformComponent == nullptr || waveformContainer == nullptr)
            return;
        
        auto viewportBounds = waveformViewport.getLocalBounds();
        int containerWidth = viewportBounds.getWidth();
        
        if (pitchOffset != 0)
        {

            double pitchFactor = std::pow(2.0, pitchOffset / 12.0);
            containerWidth = (int)(viewportBounds.getWidth() * pitchFactor);
            // containerWidth = (int)(viewportBounds.getWidth() / pitchFactor);

        }
        
        // Ensure minimum width
        containerWidth = juce::jmax(containerWidth, viewportBounds.getWidth());   

        // Set container size
        waveformContainer->setBounds(0, 0, containerWidth, viewportBounds.getHeight());
        
        // Set waveform component to fill the container
        waveformComponent->setBounds(waveformContainer->getLocalBounds().reduced(2));

        // Always anchor to left side
        waveformViewport.setViewPosition(0, 0);
    }  */
    
    void updateWaveformSize(bool preserveScroll = false)
    {
        if (waveformComponent == nullptr || waveformContainer == nullptr)
            return;

        auto viewportBounds = waveformViewport.getLocalBounds();
        if (viewportBounds.getWidth() <= 0 || viewportBounds.getHeight() <= 0)
            return;

        // ===== CRITICAL FIX: Account for scrollbar height in container =====
        // Container height = viewport height - scrollbar height (so waveform isn't squished)
        int containerHeight = viewportBounds.getHeight() - SCROLLBAR_HEIGHT;
        containerHeight = juce::jmax(1, containerHeight);  // Ensure positive

        int containerWidth;
        if (pitchOffset < 0) // Pitch DOWN - EXPAND
        {
            double expansionFactor = std::pow(2.0, std::abs(pitchOffset) / 1200.0);
            expansionFactor = juce::jmin(expansionFactor, 16.0);
            containerWidth = (int)(viewportBounds.getWidth() * expansionFactor * waveformZoomLevel);
        }
        else
        {
            containerWidth = (int)(viewportBounds.getWidth() * waveformZoomLevel);
        }

        // Cap maximum width — dynamic to support up to 100x zoom
        const int maxWidth = juce::jmax(11200, (int)(viewportBounds.getWidth() * 110));
        containerWidth = juce::jmin(containerWidth, maxWidth);
        containerWidth = juce::jmax(containerWidth, viewportBounds.getWidth());

        // Set container bounds with adjusted height
        waveformContainer->setBounds(0, 0, containerWidth, containerHeight);
        waveformComponent->setBounds(waveformContainer->getLocalBounds().reduced(2));

        // Pass current zoom level to waveform component for indicator display
        if (waveformComponent != nullptr)
            waveformComponent->setZoomLevel(waveformZoomLevel);

        // ===== CRITICAL FIX #3: Always show scrollbar (true = always visible) =====
        waveformViewport.setScrollBarsShown(false, true);

        // Reset scroll position (unless preserving for zoom centering)
        if (!preserveScroll)
            waveformViewport.setViewPosition(0, 0);
        waveformComponent->repaint();
    }

    // Empty pad overlay
    juce::Label emptyStateLabel;
    bool        isEmptyPad = false;

    // UI Components
    juce::TextButton addButton{"+"};
    juce::TextButton prevButton{"Prev"};
    juce::TextButton nextButton{"Next"};
    juce::TextButton learnButton{"Learn"};  // MIDI Learn button
    WaveformViewport waveformViewport;
    std::unique_ptr<juce::Component> waveformContainer;
    std::unique_ptr<WaveformComponent> waveformComponent;
    
    // Fixed info labels for waveform display (don't scroll with viewport)
    juce::Label topInfoLabel;      // For compression/stretching percentage
    juce::Label bottomInfoLabel;   // For pitch information
    
    // MIDI Note controls
    juce::Label midiNoteLabel;
    int currentMidiNote = 60;  // Default to Middle C
    
    // MIDI Channel controls
    juce::TextButton channelDownButton{"-"};
    juce::Label midiChannelLabel;
    juce::TextButton channelUpButton{"+"};
    int currentMidiChannel = 1;  // Default to channel 1
    
    juce::Label sampleNameLabel;
    juce::Label durationLabel;
    
    // Pitch adjustment controls
    juce::TextButton pitchDownButton{"Down"};  // Lower pitch = negative cents = longer duration
    DraggablePitchLabel pitchLabel;
    juce::TextButton pitchUpButton{"Up"};      // Higher pitch = positive cents = shorter duration
    int pitchOffset = 0;  // Pitch offset in CENTS (±4800; 100 cents = 1 semitone)

    // Pitch step size (cycles: 100, 50, 25, 33 cents)
    int currentPitchStepCents = 100;
    juce::TextButton pitchStepButton;
    juce::Label      pitchStepLabel;
    juce::Label      stepDescriptionLabel;  // Musical meaning of current step size, shown left of Prev

    // Base tuning frequency — default A4 = 440.0 Hz; range 400–480 Hz
    double baseTuningHz     = 440.0;
    double tuneBaseTuningHz = 440.0; // captured at Tune analysis start (thread param)

    // Loop / Freeze / OneShot / Reverse / Grid / Tune buttons
    juce::TextButton loopButton;
    juce::TextButton freezeButton { "Freeze" };
    juce::TextButton oneShotButton { "1Shot" };
    bool oneShotEnabled   = false;
    juce::TextButton reverseButton { "Rev" };
    bool reverseEnabledState = false;
    juce::TextButton bounceButton { "Bnc" };
    bool bounceEnabledState = false;
    OneShotPulseTimer oneShotPulseTimer { *this };
    int  oneShotPulsePhase = 0;
    juce::TextButton gridSnapButton { "Grid" };
    juce::TextButton gridResolutionButton { "1s" };
    juce::TextButton tuneButton { "Tune" };
    bool gridSnapEnabled = false;
    int  gridResolutionIndex = 5;   // default = 1s (index into the 6-step table)
    bool userHasSetGridResolution = false; // true once user manually clicks or a saved value is restored
    static constexpr int numGridResolutions = 6;

    // Auto-tune / pitch detection state
    juce::String detectedNoteName;               // e.g. "D3" — empty until Tune is run
    double       detectedFreqHz   = 0.0;         // detected fundamental frequency in Hz
    int          basePitchOffset  = 0;           // hidden correction from Tune; user sees 0 st = this note
    std::atomic<bool> isTuneRunning { false };
    // Analysis parameters captured on message thread before background thread starts
    juce::File   tuneFile;
    double       tuneStartSec     = 0.0;
    double       tuneEndSec       = 0.0;
    int          tuneRootMidiNote = 60;
    std::unique_ptr<TuneAnalyzerThread> tuneThread;

    // Transient detection toggle
    juce::TextButton detectionToggleButton { "Tra" };
    bool transientDetectionEnabled = true;

    // Transient snap buttons — Start marker (left of Start knob)
    juce::TextButton prevTransientButton { "< T" };
    juce::TextButton nextTransientButton { "T >" };

    // Transient snap buttons — End marker (right of End knob)
    juce::TextButton prevEndTransientButton { "< T" };
    juce::TextButton nextEndTransientButton { "T >" };

    // Detected transient positions in seconds (populated on each sample load)
    std::vector<double> transientPositionsSeconds;
    double transientThreshold = 4.0;  // RMS multiplier for detection (1.5–10)

    // Sensitivity knob + labels
    juce::Slider sensKnob;
    juce::Label  sensLabel;
    juce::Label  sensValueLabel;     // numeric threshold display to the right of Sens knob (e.g. "4.0x")
    juce::Label  transientCountLabel;

    juce::Rectangle<int> footerBounds;  // set in resized(), drawn in paint() as darker background

    // Trim feature
    juce::TextButton trimButton { "Trim" };
    juce::Label      toastLabel;   // brief status overlay on the footer (2-second auto-hide)

    // Freeze state — never persisted, always starts false
    bool isFreezeActive          = false;
    bool loopWasOnBeforeFreeze   = false;   // true if loop was ON when freeze was pressed
    juce::int64 lastFreezeTapMs  = 0;       // timestamp of last freeze tap for double-tap detection

    // Compact knob LookAndFeel shared by all three SamplerPad knobs
    CompactKnobLookAndFeel compactKnobLaf;

    // Volume knob
    juce::Slider volumeKnob;
    juce::Label  volumeLabel;
    juce::Label  volValueLabel;   // percentage display to the right of the knob (e.g. "75%")

    // Start point knob
    juce::Slider startKnob;
    juce::Label startKnobLabel;
    float startPointNormalized = 0.0f;

    // End point knob
    juce::Slider endKnob;
    juce::Label endKnobLabel;
    float endPointNormalized = 1.0f;
    
    // Pitch factor tracking for visual feedback
    double currentPitchFactor = 1.0;  // 1.0 = no pitch change
    double originalDuration = 0.0;     // Store original duration
    
    // MIDI Learn state
    bool isLearning = false;
    
    // Audio components
    juce::AudioFormatManager& formatManager;
    
    // Audio file info for resampling
    juce::File currentAudioFile;
    juce::int64 originalLengthInSamples = 0;
    double originalSampleRate = 0.0;
    
    // Listener list
    juce::ListenerList<Listener> listeners;
    int fixedViewportWidth = 700;  // Will be updated in resized()
    double waveformZoomLevel = 1.0;

    // ===== TAB BAR =====
    int activeTab = 0;  // 0=Controls, 1=ADSR, 2=EQ
    juce::TextButton controlsTabButton { "Controls" };
    juce::TextButton adsrTabButton     { "ADSR" };
    juce::TextButton eqTabButton       { "EQ" };
    juce::Label      eqPlaceholderLabel;

    // ===== ADSR ENVELOPE CONTROLS =====
    bool   adsrEnabled       = false;
    float  adsrAttackMs      = 0.0f;
    float  adsrDecayMs       = 0.0f;
    float  adsrSustain       = 1.0f;
    float  adsrReleaseMs     = 0.0f;
    bool   isAdsrKnobDragging = false;  // true while any ADSR knob is being dragged
    bool   isVolKnobDragging  = false;  // true while the Vol knob is being dragged

    // Custom rotary knob with linear pixel-delta drag and configurable sensitivity.
    // Overrides JUCE's default rotary drag with predictable ms-per-pixel behaviour.
    // Shift key divides sensitivity by 10 for fine control.
    // Double-click resets to defaultValue.
    class AdsrKnob : public juce::Slider
    {
    public:
        double sensitivityNormal = 1.0;   // units per pixel (ms/px or fraction/px)
        double defaultValue      = 0.0;   // reset value on double-click

        AdsrKnob() : juce::Slider(juce::Slider::RotaryHorizontalVerticalDrag,
                                   juce::Slider::NoTextBox) {}

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.mods.isLeftButtonDown())
            {
                dragStartY     = e.getScreenPosition().y;
                dragStartValue = getValue();
                isDraggingNow  = true;
                if (onDragStart) onDragStart();   // notifies SampleCard::isAdsrKnobDragging
            }
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (isDraggingNow)
            {
                double sens   = e.mods.isShiftDown() ? sensitivityNormal * 0.1 : sensitivityNormal;
                double newVal = juce::jlimit(getMinimum(), getMaximum(),
                                             dragStartValue + (double)(dragStartY - e.getScreenPosition().y) * sens);
                setValue(newVal, juce::sendNotificationSync);
            }
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            if (isDraggingNow)
            {
                isDraggingNow = false;
                if (onDragEnd) onDragEnd();       // notifies SampleCard::isAdsrKnobDragging = false
            }
        }

        void mouseDoubleClick(const juce::MouseEvent&) override
        {
            setValue(defaultValue, juce::sendNotificationSync);
        }

    private:
        int    dragStartY     = 0;
        double dragStartValue = 0.0;
        bool   isDraggingNow  = false;
    };

    juce::TextButton adsrEnableButton { "Env" };
    AdsrKnob         adsrAtkKnob;
    AdsrKnob         adsrDcyKnob;
    AdsrKnob         adsrSusKnob;
    AdsrKnob         adsrRelKnob;
    juce::Label      adsrAtkLabel, adsrDcyLabel, adsrSusLabel, adsrRelLabel;
    juce::Label      adsrAtkValueLabel, adsrDcyValueLabel, adsrSusValueLabel, adsrRelValueLabel;

    // ===== EQ CONTROLS =====
    bool eqEnabled = false;
    EQDisplay::Band eqBands[3] = { {100.0f, 0.0f, 1.0f},
                                   {500.0f, 0.0f, 1.0f},
                                   {8000.0f,0.0f, 1.0f} };
    juce::TextButton          eqEnableButton  { "EQ" };
    juce::TextButton          filterModeButton{ "Bell" };
    juce::TextButton          eqResetButton   { "Reset" };
    int                       eqFilterModes[3] = { 2, 2, 2 };  // per-band mode, saved per-sample
    std::unique_ptr<EQDisplay> eqDisplay;

    // ===== NORMALIZE CONTROLS =====
    bool  normEnabled       = false;
    float normTargetDb      = -6.0f;
    float normAppliedGainDb = 0.0f;  // set by MainComponent after peak computation

    juce::TextButton normTargetMinus12Button { "-12" };
    juce::TextButton normTargetMinus6Button  { "-6"  };
    juce::TextButton normTargetZeroButton    { "0"   };
    juce::TextButton normButton              { "Norm" };
    juce::Label      normGainLabel;

    //==============================================================================
    // Transient detection enable/disable

    void updateTransientControlsState()
    {
        const bool on = transientDetectionEnabled;
        const auto grayBg   = juce::Colour(0xFF555555);
        const auto grayText = juce::Colour(0xFF555555);

        // Start snap buttons: orange-red when enabled; gray when disabled
        for (auto* btn : { &prevTransientButton, &nextTransientButton })
        {
            btn->setEnabled(on);
            btn->setColour(juce::TextButton::buttonColourId,
                           on ? juce::Colour(0xFFE84A1A) : grayBg);
            btn->setColour(juce::TextButton::textColourOffId,
                           on ? juce::Colour(0xFFFFFFFF) : grayText);
        }

        // End snap buttons: cyan-blue when enabled; gray when disabled
        for (auto* btn : { &prevEndTransientButton, &nextEndTransientButton })
        {
            btn->setEnabled(on);
            btn->setColour(juce::TextButton::buttonColourId,
                           on ? juce::Colour(0xFF00B4D8) : grayBg);
            btn->setColour(juce::TextButton::textColourOffId,
                           on ? juce::Colour(0xFFFFFFFF) : grayText);
        }

        // Sens knob: dark red fill when enabled; gray when disabled
        sensKnob.setEnabled(on);
        sensKnob.setColour(juce::Slider::rotarySliderFillColourId,
                           on ? juce::Colour(0xFF8B2500) : juce::Colour(0xFF555555));
        sensKnob.setColour(juce::Slider::thumbColourId,
                           on ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF777777));

        // Count label: white when enabled; gray when disabled
        transientCountLabel.setColour(juce::Label::textColourId,
                                      on ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF555555));

        // Sens value label: white when enabled; gray when disabled
        sensValueLabel.setColour(juce::Label::textColourId,
                                 on ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF555555));

        repaint();
    }

    //==============================================================================
    // Playhead — 60fps timer reads voice atomic, forwards normalized position to WaveformComponent

    class PlayheadTimer : public juce::Timer
    {
    public:
        PlayheadTimer(SampleCard& o) : owner(o) {}
        void timerCallback() override { owner.updatePlayhead(); }
    private:
        SampleCard& owner;
    };
    PlayheadTimer playheadTimer { *this };

    //==============================================================================
    // Hold-to-repeat timer for the Up/Down pitch buttons.
    // mouseDown fires one step immediately; after 400ms hold, repeats every 100ms;
    // after 1000ms hold, accelerates to every 50ms. mouseUp stops cleanly.
    class PitchRepeatTimer : public juce::Timer, public juce::MouseListener
    {
    public:
        PitchRepeatTimer() = default;

        std::function<void()> onUp;
        std::function<void()> onDown;

        void attachToButtons(juce::Button& upBtn, juce::Button& downBtn)
        {
            upButton   = &upBtn;
            downButton = &downBtn;
            upBtn.addMouseListener(this, false);
            downBtn.addMouseListener(this, false);
        }

        // Returns true (and consumes the flag) when mouseDown already handled the press.
        // Called from onClick to prevent double-fire on mouse release.
        bool suppressNextClick()
        {
            if (suppressClick) { suppressClick = false; return true; }
            return false;
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if      (e.eventComponent == upButton)   startHold(1);
            else if (e.eventComponent == downButton) startHold(-1);
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.eventComponent == upButton || e.eventComponent == downButton)
                stopHold();
        }

    private:
        juce::Button*  upButton    = nullptr;
        juce::Button*  downButton  = nullptr;
        int            direction   = 0;          // +1=up, -1=down, 0=idle
        juce::int64    holdStartMs = 0;
        juce::int64    lastFireMs  = 0;
        bool           suppressClick = false;

        void startHold(int dir)
        {
            direction    = dir;
            holdStartMs  = (juce::int64)juce::Time::getMillisecondCounter();
            lastFireMs   = holdStartMs;
            suppressClick = true;   // onClick fires on mouseUp — skip it

            // Fire the first step immediately on press (feels instant).
            if (direction > 0 && onUp)   onUp();
            else if (direction < 0 && onDown) onDown();

            startTimer(50);   // poll every 50ms — matches our fastest repeat rate
            updateButtonVisual(dir, true);
        }

        void stopHold()
        {
            stopTimer();
            updateButtonVisual(direction, false);
            direction = 0;
        }

        void timerCallback() override
        {
            if (direction == 0) return;

            const juce::int64 now           = (juce::int64)juce::Time::getMillisecondCounter();
            const juce::int64 heldMs        = now - holdStartMs;
            const juce::int64 sinceLastFire = now - lastFireMs;

            if (heldMs < 400) return;   // Initial hold delay — no repeat yet

            // 400–1000ms: slow repeat every 100ms.  >1000ms: fast repeat every 50ms.
            const juce::int64 repeatMs = (heldMs < 1000) ? 100LL : 50LL;

            if (sinceLastFire >= repeatMs)
            {
                lastFireMs = now;
                if (direction > 0 && onUp)   onUp();
                else if (direction < 0 && onDown) onDown();
            }
        }

        void updateButtonVisual(int dir, bool active)
        {
            auto* btn = (dir > 0) ? upButton : (dir < 0 ? downButton : nullptr);
            if (btn == nullptr) return;
            if (active)
            {
                btn->setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF6A6A6A));
                btn->setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
            }
            else
            {
                btn->setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
                btn->setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
            }
        }
    };
    PitchRepeatTimer pitchRepeatTimer;

    void updatePlayhead()
    {
        if (waveformComponent == nullptr) return;
        float pos = getPlayheadPosition
                    ? (float)getPlayheadPosition()
                    : -1.0f;
        int dir = (getPlayheadDirection && bounceEnabledState)
                  ? getPlayheadDirection()
                  : 1;
        waveformComponent->setPlayheadBounceAndDirection(dir, bounceEnabledState);
        waveformComponent->setPlayheadPosition(pos);
    }

    //==============================================================================
    // Tab bar helpers

    static void applyTabStyle(juce::TextButton& btn, bool active)
    {
        btn.setColour(juce::TextButton::buttonColourId,
                      active ? juce::Colour(0xFF555555) : juce::Colour(0xFF333333));
        btn.setColour(juce::TextButton::textColourOffId,
                      active ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF888888));
    }

    void setActiveTab(int tab)
    {
        activeTab = juce::jlimit(0, 2, tab);
        resized();
        repaint();  // Force full redraw — clears ghost outlines left by hidden tab components
        listeners.call([this](Listener& l) { l.activeTabChanged(activeTab); });
    }

public:
    // Quiet restore from session — no listener fired.
    void setActiveTabQuiet(int tab)
    {
        activeTab = juce::jlimit(0, 2, tab);
        resized();
        repaint();  // Force full redraw — clears ghost outlines left by hidden tab components
    }

    // Restore saved zoom level and scroll position after a sample load.
    // Must be called after setWaveform() so the container is already sized.
    void restoreZoomAndScroll(double zoomLevel, float normalizedScroll)
    {
        waveformZoomLevel = juce::jlimit(1.0, 100.0, zoomLevel);
        if (waveformComponent != nullptr)
            waveformComponent->setZoomLevel(waveformZoomLevel);
        updateWaveformSize(true);
        const int containerW = waveformContainer != nullptr ? waveformContainer->getWidth() : 0;
        const int viewportW  = waveformViewport.getWidth();
        const int maxScroll  = juce::jmax(0, containerW - viewportW);
        waveformViewport.setViewPosition((int)(normalizedScroll * (float)maxScroll), 0);
    }

    // Fired whenever zoom or scroll changes interactively.
    // Hook this in MainComponent to save zoom state to disk.
    std::function<void(double zoomLevel, float normalizedScroll)> onZoomStateChanged;

    // Wired by MainComponent to return the current playback position normalized [0,1]
    // over the full sample, or -1 if no voice is active.  Read 60 times per second.
    std::function<double()> getPlayheadPosition;
    // Wired by MainComponent to return current bounce direction: +1=forward, -1=backward.
    // Returns 1 when not in bounce mode (or no voice active).
    std::function<int()> getPlayheadDirection;

    // Fired when the user confirms Trim — MainComponent performs the actual file write.
    std::function<void()> onTrimRequested;

    // Fired when the EQ Reset button is clicked — MainComponent writes pre-computed flat
    // coefficients directly to EqCoeffDoubleBuffer (no computation, no synchronous save).
    std::function<void()> onEqReset;

    // Called by MainComponent to show/hide the "Trimming..." state.
    void setTrimInProgress(bool inProgress)
    {
        trimButton.setButtonText(inProgress ? "Trimming..." : "Trim");
        trimButton.setEnabled(!inProgress);
    }

    // Show a brief status message overlaid on the footer for 2 seconds, then auto-hide.
    // isError=true uses a dark-red background; false uses a dark-green background.
    void showTrimToast(const juce::String& msg, bool isError = false)
    {
        toastLabel.setText(msg, juce::dontSendNotification);
        toastLabel.setColour(juce::Label::backgroundColourId,
                             isError ? juce::Colour(0xFF8B0000) : juce::Colour(0xFF006000));
        toastLabel.setVisible(true);
        juce::Component::SafePointer<SampleCard> safe(this);
        juce::Timer::callAfterDelay(2000, [safe]() mutable {
            if (auto* self = safe.getComponent())
                self->toastLabel.setVisible(false);
        });
    }

private:

    // Normalized scroll position [0, 1] — 0 = fully left, 1 = fully right.
    float getScrollPositionNormalized() const
    {
        if (waveformContainer == nullptr) return 0.0f;
        const int containerW = waveformContainer->getWidth();
        const int viewportW  = waveformViewport.getWidth();
        const int maxScroll  = juce::jmax(0, containerW - viewportW);
        if (maxScroll <= 0) return 0.0f;
        return (float)waveformViewport.getViewPositionX() / (float)maxScroll;
    }

    void updateTabVisibility()
    {
        // Controls tab (row 1 + row 2 contents)
        const bool showCtrl = (activeTab == 0);
        for (auto* c : std::initializer_list<juce::Component*>{
            &pitchDownButton, &pitchLabel, &pitchUpButton,
            &pitchStepButton, &pitchStepLabel,
            &tuneButton, &freezeButton, &loopButton, &oneShotButton, &reverseButton, &bounceButton,
            &gridSnapButton, &gridResolutionButton,
            &detectionToggleButton,
            &prevTransientButton, &nextTransientButton,
            &startKnob, &startKnobLabel,
            &endKnob,   &endKnobLabel,
            &prevEndTransientButton, &nextEndTransientButton,
            &sensKnob, &sensLabel, &sensValueLabel, &transientCountLabel,
            &trimButton})
            c->setVisible(showCtrl);

        // ADSR tab content (knobs only — Env toggle lives in the tab bar)
        const bool showAdsr = (activeTab == 1);
        for (auto* c : std::initializer_list<juce::Component*>{
            &adsrAtkKnob, &adsrAtkLabel, &adsrAtkValueLabel,
            &adsrDcyKnob, &adsrDcyLabel, &adsrDcyValueLabel,
            &adsrSusKnob, &adsrSusLabel, &adsrSusValueLabel,
            &adsrRelKnob, &adsrRelLabel, &adsrRelValueLabel})
            c->setVisible(showAdsr);
        // Env toggle: tab-bar button, visible only when ADSR tab is active
        adsrEnableButton.setVisible(showAdsr);

        // EQ tab
        eqPlaceholderLabel.setVisible(false);  // replaced by eqDisplay
        const bool showEq = (activeTab == 2);
        eqEnableButton.setVisible(showEq);    // only visible on the EQ tab
        filterModeButton.setVisible(showEq);  // same rule as EQ enable button
        eqResetButton.setVisible(showEq);     // same rule
        normTargetMinus12Button.setVisible(showEq);
        normTargetMinus6Button.setVisible(showEq);
        normTargetZeroButton.setVisible(showEq);
        normButton.setVisible(showEq);
        normGainLabel.setVisible(showEq);
        if (eqDisplay != nullptr)
        {
            eqDisplay->setVisible(showEq);
            if (showEq)  eqDisplay->startAnimation();
            else         eqDisplay->stopAnimation();
        }
    }

    void layoutAdsrTabContent(juce::Rectangle<int>& area)
    {
        // Env toggle button now lives in the tab bar row — no row needed here.
        // Row B (60px): 4 ADSR knobs — fills the full content area directly.
        auto rowB = area.removeFromTop(60);
        const int labelH = 13;

        juce::Slider* knobs[] = { &adsrAtkKnob, &adsrDcyKnob, &adsrSusKnob, &adsrRelKnob };
        juce::Label*  vals[]  = { &adsrAtkValueLabel, &adsrDcyValueLabel, &adsrSusValueLabel, &adsrRelValueLabel };
        juce::Label*  lbls[]  = { &adsrAtkLabel, &adsrDcyLabel, &adsrSusLabel, &adsrRelLabel };

        for (int i = 0; i < 4; ++i)
        {
            auto col = rowB.removeFromLeft(60);
            lbls[i]->setBounds(col.removeFromBottom(labelH).reduced(2, 0));
            vals[i]->setBounds(col.removeFromBottom(labelH).reduced(2, 0));
            knobs[i]->setBounds(col.reduced(2));
        }

        area.removeFromTop(5); // keep spacing consistent with Controls tab
    }

    void layoutEqTabContent(juce::Rectangle<int>& area)
    {
        // EQ toggle button now lives in the tab bar row — no row needed here.
        // EQDisplay fills the full available content area.
        if (eqDisplay != nullptr)
            eqDisplay->setBounds(area.reduced(2, 0));
        else
            area.removeFromTop(area.getHeight());
    }

    //==============================================================================
    // ADSR helpers

public:
    // Quietly restore ADSR state (called from per-sample state load — no listener fired).
    // notifyListeners=false for silent restores (setWaveform reset, load lambda).
    // Must be false in setWaveform() to avoid clobbering the PropertiesFile in-memory store
    // BEFORE getSampleState() reads it — the root cause of ADSR not surviving restart.
    void setAdsrParams(bool enabled, float atkMs, float dcyMs, float sus, float relMs,
                       bool notifyListeners = true)
    {
        adsrEnabled   = enabled;
        adsrAttackMs  = atkMs;
        adsrDecayMs   = dcyMs;
        adsrSustain   = sus;
        adsrReleaseMs = relMs;
        adsrEnableButton.setToggleState(enabled, juce::dontSendNotification);
        adsrAtkKnob.setValue(atkMs,  juce::dontSendNotification);
        adsrDcyKnob.setValue(dcyMs,  juce::dontSendNotification);
        adsrSusKnob.setValue(sus,    juce::dontSendNotification);
        adsrRelKnob.setValue(relMs,  juce::dontSendNotification);
        updateAdsrControlsState();
        updateAdsrValueDisplays(notifyListeners);
        if (waveformComponent != nullptr)
            waveformComponent->setAdsrOverlay(enabled, atkMs, dcyMs, sus, relMs);
    }

    void updateAdsrValueDisplays(bool notifyListeners = true)
    {
        adsrAtkValueLabel.setText(juce::String((int)std::round(adsrAttackMs))  + "ms",  juce::dontSendNotification);
        adsrDcyValueLabel.setText(juce::String((int)std::round(adsrDecayMs))   + "ms",  juce::dontSendNotification);
        adsrSusValueLabel.setText(juce::String((int)std::round(adsrSustain * 100.0f)) + "%", juce::dontSendNotification);
        adsrRelValueLabel.setText(juce::String((int)std::round(adsrReleaseMs)) + "ms",  juce::dontSendNotification);
        // Always update the waveform ADSR overlay — setAdsrOverlay uses dirty-region
        // repaint (start-to-end marker region only) so this is fast even during drag.
        if (waveformComponent != nullptr)
            waveformComponent->setAdsrOverlay(adsrEnabled, adsrAttackMs, adsrDecayMs, adsrSustain, adsrReleaseMs);
        if (notifyListeners)
        {
            // Notify so MainComponent propagates values to audio engine and saves to disk.
            // Only fires for real user changes — NOT for silent resets or load restores
            // (where firing would overwrite PropertiesFile with wrong values).
            listeners.call([this](Listener& l)
            {
                l.adsrParamsChanged(adsrEnabled, adsrAttackMs, adsrDecayMs, adsrSustain, adsrReleaseMs);
            });
        }
    }

    void updateAdsrControlsState()
    {
        const bool on = adsrEnabled;
        const juce::Colour activeFill(0xFFBB0090); // fuchsia-ish for knobs
        const juce::Colour dimFill   (0xFF555555);
        const juce::Colour activeText(0xFFFFFFFF);
        const juce::Colour dimText   (0xFF555555);

        for (auto* k : {&adsrAtkKnob, &adsrDcyKnob, &adsrSusKnob, &adsrRelKnob})
        {
            k->setEnabled(on);
            k->setColour(juce::Slider::rotarySliderFillColourId, on ? activeFill : dimFill);
            k->setColour(juce::Slider::thumbColourId, on ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF777777));
        }
        for (auto* l : {&adsrAtkLabel,      &adsrDcyLabel,      &adsrSusLabel,      &adsrRelLabel,
                        &adsrAtkValueLabel, &adsrDcyValueLabel, &adsrSusValueLabel, &adsrRelValueLabel})
            l->setColour(juce::Label::textColourId, on ? activeText : dimText);
    }

    // Getters for MainComponent to read current ADSR state
    bool  isAdsrEnabled()    const { return adsrEnabled; }
    float getAdsrAttackMs()  const { return adsrAttackMs; }
    float getAdsrDecayMs()   const { return adsrDecayMs; }
    float getAdsrSustain()   const { return adsrSustain; }
    float getAdsrReleaseMs() const { return adsrReleaseMs; }
    int   getActiveTabIndex() const { return activeTab; }

    // ===== NORMALIZE public API =====
    bool  isNormEnabled()        const { return normEnabled; }
    float getNormTargetDb()      const { return normTargetDb; }
    float getNormAppliedGainDb() const { return normAppliedGainDb; }

    // Called by MainComponent after computing the peak-based gain. Updates display label.
    void setNormGainDisplay(float gainDb)
    {
        normAppliedGainDb = gainDb;
        updateNormGainLabel();
    }

    // Silently restore norm state — notifyListeners=false for load restores.
    void setNormParams(bool enabled, float targetDb, bool notifyListeners = true)
    {
        normEnabled  = enabled;
        normTargetDb = targetDb;
        normButton.setToggleState(enabled, juce::dontSendNotification);
        updateNormTargetButtonColors();
        if (!enabled) { normAppliedGainDb = 0.0f; updateNormGainLabel(); }
        if (notifyListeners)
            listeners.call([this](Listener& l) { l.normChanged(normEnabled, normTargetDb); });
    }

    // ===== EQ public API =====
    bool  isEqEnabled()           const { return eqEnabled; }
    // Returns true while the user is actively dragging a control point in the EQ display.
    // Used by MainComponent::eqParamsChanged() to skip resetEqState() and defer disk saves.
    bool  isEqDisplayDragging()   const { return eqDisplay != nullptr && eqDisplay->isEqDragging(); }
    // Returns true while any ADSR knob is being dragged (mouse button held).
    // Used by MainComponent::adsrParamsChanged() to defer disk saves during drag.
    bool  isAdsrDragging()        const { return isAdsrKnobDragging; }
    // Returns true while the Vol knob is being dragged (reserved for future use).
    bool  isVolDragging()         const { return isVolKnobDragging; }
    float getEqBandFreq(int i)    const { return (i>=0&&i<3) ? eqBands[i].freq   : 500.0f; }
    float getEqBandGain(int i)    const { return (i>=0&&i<3) ? eqBands[i].gainDb : 0.0f;   }
    float getEqBandQ(int i)       const { return (i>=0&&i<3) ? eqBands[i].q      : 1.0f;   }
    int   getEqFilterMode(int i)  const { return (i>=0&&i<3) ? eqFilterModes[i]  : 2;      }

    // Silently restore filter modes (no listener fired) — called from per-sample state load.
    void setEqFilterModes(int m1, int m2, int m3, bool notifyListeners = true)
    {
        eqFilterModes[0] = juce::jlimit(0, 5, m1);
        eqFilterModes[1] = juce::jlimit(0, 5, m2);
        eqFilterModes[2] = juce::jlimit(0, 5, m3);
        if (eqDisplay != nullptr)
        {
            eqDisplay->setFilterMode(0, eqFilterModes[0]);
            eqDisplay->setFilterMode(1, eqFilterModes[1]);
            eqDisplay->setFilterMode(2, eqFilterModes[2]);
            // Update button text for the currently active band (if any)
            const int ab = eqDisplay->getActiveBand();
            if (ab >= 0 && ab < 3)
                filterModeButton.setButtonText(EQDisplay::filterModeName(eqFilterModes[ab]));
        }
        if (notifyListeners)
        {
            listeners.call([this](Listener& l) {
                l.eqFilterModesChanged(eqFilterModes[0], eqFilterModes[1], eqFilterModes[2]);
            });
        }
    }

    // Set EQ sample rate so the frequency-response curve is accurate.
    void setEqSampleRate(double sr)
    {
        if (eqDisplay != nullptr) eqDisplay->setSampleRate(sr);
    }

    // Wire the spectrum data callback from MainComponent → EQDisplay.
    // OPT 4: Callback returns bool (true = new data available).
    void setEqSpectrumCallback(std::function<bool(float*, int)> cb)
    {
        if (eqDisplay != nullptr) eqDisplay->getSpectrumCallback = cb;
    }

    // Quietly restore EQ state (no listener fired) — used by per-sample load.
    void setEqParams(bool enabled,
                     float f1, float g1, float q1,
                     float f2, float g2, float q2,
                     float f3, float g3, float q3,
                     bool notifyListeners = true)
    {
        eqEnabled  = enabled;
        eqBands[0] = { f1, g1, q1 };
        eqBands[1] = { f2, g2, q2 };
        eqBands[2] = { f3, g3, q3 };
        eqEnableButton.setToggleState(enabled, juce::dontSendNotification);
        if (eqDisplay != nullptr)
        {
            eqDisplay->setEqEnabled(enabled);
            eqDisplay->setBands(eqBands);
        }
        if (notifyListeners) fireEqParamsChanged();
    }

private:
    //==============================================================================
    // One-shot pulse animation (called by OneShotPulseTimer when tail is playing)
    void oneShotPulseTick()
    {
        // Alternate between full amber and dimmer amber every tick (~350ms)
        oneShotPulsePhase = (oneShotPulsePhase + 1) % 2;
        juce::Colour c = (oneShotPulsePhase == 0)
                         ? juce::Colour(0xFFFFB300)   // full amber
                         : juce::Colour(0xFF886000);  // dim amber
        oneShotButton.setColour(juce::TextButton::buttonOnColourId, c);
        oneShotButton.repaint();
    }

    //==============================================================================
    // Normalize helpers

    // Update mutually-exclusive target button highlight.
    void updateNormTargetButtonColors()
    {
        const auto activeColor = juce::Colour(0xFFFFE000); // bright yellow
        const auto activeTxt   = juce::Colour(0xFF111111);
        const auto inactiveC   = juce::Colour(0xFF4A4A4A);
        const auto inactiveTxt = juce::Colour(0xFFCECECE);

        struct { juce::TextButton* btn; float db; } items[] = {
            { &normTargetMinus12Button, -12.0f },
            { &normTargetMinus6Button,  -6.0f  },
            { &normTargetZeroButton,     0.0f  },
        };
        for (auto& item : items)
        {
            const bool active = (normTargetDb == item.db);
            item.btn->setColour(juce::TextButton::buttonColourId,  active ? activeColor : inactiveC);
            item.btn->setColour(juce::TextButton::textColourOffId, active ? activeTxt   : inactiveTxt);
        }
    }

    // Set a new normalization target and re-trigger computation if Norm is ON.
    void setNormTarget(float targetDb)
    {
        normTargetDb = targetDb;
        updateNormTargetButtonColors();
        if (normEnabled)
            listeners.call([this](Listener& l) { l.normChanged(normEnabled, normTargetDb); });
    }

    // Refresh the gain display label text.
    void updateNormGainLabel()
    {
        if (!normEnabled || normAppliedGainDb == 0.0f)
        {
            normGainLabel.setText("", juce::dontSendNotification);
        }
        else
        {
            juce::String text;
            if (normAppliedGainDb >= 0.0f)
                text = "+" + juce::String(normAppliedGainDb, 1) + "dB";
            else
                text = juce::String(normAppliedGainDb, 1) + "dB";
            normGainLabel.setText(text, juce::dontSendNotification);
        }
    }

    //==============================================================================
    // EQ helpers

    void fireEqParamsChanged()
    {
        listeners.call([this](Listener& l) {
            l.eqParamsChanged(eqEnabled,
                              eqBands[0].freq, eqBands[0].gainDb, eqBands[0].q,
                              eqBands[1].freq, eqBands[1].gainDb, eqBands[1].q,
                              eqBands[2].freq, eqBands[2].gainDb, eqBands[2].q);
        });
    }

    //==============================================================================
    // Grid snap helpers

    // Pixel width actually covered by waveform data (mirrors WaveformComponent::getActualWaveformWidth).
    float computeActualWaveformWidth() const
    {
        if (waveformComponent == nullptr) return 0.0f;
        int rw = waveformComponent->getWidth();
        if (pitchOffset > 0)
        {
            double cf = juce::jmin(std::pow(2.0, (double)pitchOffset / 1200.0), 16.0);
            return (float)(rw / cf);
        }
        return (float)rw;
    }

    // Resolution label string for a given index (0-5).
    static juce::String gridResolutionLabel(int index)
    {
        static const char* labels[] = { "1ms", "10ms", "50ms", "100ms", "500ms", "1s" };
        return (index >= 0 && index < numGridResolutions) ? labels[index] : "?";
    }

    // Refresh the pitch step button text and step description label.
    void updatePitchStepButton()
    {
        juce::String text;
        juce::String description;
        switch (currentPitchStepCents)
        {
            case 100: text = "100c"; description = "Western semitone";           break;
            case 50:  text = "50c";  description = "Arabic / Turkish quarter tone"; break;
            case 25:  text = "25c";  description = "Microtonal eighth tone";     break;
            case 33:  text = "33c";  description = "Experimental third tone";    break;
            default:  text = juce::String(currentPitchStepCents) + "c";
                      description = juce::String(currentPitchStepCents) + "c step"; break;
        }
        pitchStepButton.setButtonText(text);
        stepDescriptionLabel.setText(description, juce::dontSendNotification);
    }

    // Refresh the resolution button text and colors to match current state.
    void updateGridResolutionButton()
    {
        gridResolutionButton.setButtonText(gridResolutionLabel(gridResolutionIndex));
        updateGridResolutionButtonState();
    }

    // Enable/disable + color the resolution button based on gridSnapEnabled.
    void updateGridResolutionButtonState()
    {
        gridResolutionButton.setEnabled(gridSnapEnabled);
        gridResolutionButton.setColour(juce::TextButton::buttonColourId,
            gridSnapEnabled ? juce::Colour(0xFFAAFF00) : juce::Colour(0xFF3A3A3A));
        gridResolutionButton.setColour(juce::TextButton::textColourOffId,
            gridSnapEnabled ? juce::Colour(0xFF111111) : juce::Colour(0xFF7A7A7A));
    }

    // Choose the best default resolution for a given sample duration and apply it.
    // Called on every sample load; fires the listener so the choice is persisted.
    void autoSelectGridResolution(double durationSeconds)
    {
        // Only auto-select when the user has no saved preference.
        // Once a preference exists (saved to disk and restored via setGridResolutionIndex),
        // the auto-select is skipped so the user's choice is not overridden on each load.
        if (userHasSetGridResolution)
            return;

        int newIndex;
        if      (durationSeconds < 0.5)  newIndex = 1; // 10ms
        else if (durationSeconds < 2.0)  newIndex = 2; // 50ms
        else if (durationSeconds < 5.0)  newIndex = 3; // 100ms
        else if (durationSeconds < 15.0) newIndex = 4; // 500ms
        else                             newIndex = 5; // 1s

        gridResolutionIndex = newIndex;
        updateGridResolutionButton();
        if (waveformComponent != nullptr)
            waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
        // Does NOT fire listener — auto-select never saves to disk
    }

    // Snap `seconds` to the nearest grid line if within 10 pixels (requirement 8).
    // Flashes the corresponding marker when a snap occurs (requirement 9).
    // isStart: true = start marker, false = end marker.
    double applyGridSnap(double seconds, bool isStart)
    {
        if (!gridSnapEnabled || originalDuration <= 0.0)
            return seconds;

        double interval = getGridInterval();
        float actualWidth = computeActualWaveformWidth();
        if (actualWidth <= 1.0f)
            return seconds;

        double pixelsPerSecond = actualWidth / originalDuration;
        double thresholdSeconds = 10.0 / pixelsPerSecond;

        // Find nearest grid line
        double nearest = std::round(seconds / interval) * interval;
        nearest = juce::jlimit(0.0, originalDuration, nearest);

        if (std::abs(seconds - nearest) <= thresholdSeconds)
        {
            if (std::abs(seconds - nearest) > 1e-9 && waveformComponent != nullptr)
            {
                if (isStart)
                    waveformComponent->flashStartMarker();
                else
                    waveformComponent->flashEndMarker();
            }
            return nearest;
        }
        return seconds;
    }

    //==============================================================================
    // Freeze helpers

    // FIX 4: instant visual response — mouseDown previews the NEXT toggle state.
    // The button appearance changes on press, not on release (onClick fires on mouseUp).
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &freezeButton)
        {
            // Preview next state: if currently ON flash toward OFF color, and vice-versa.
            // The actual state is committed in onClick → toggleFreeze() which runs on mouseUp.
            const bool nextState = !isFreezeActive;
            applyFreezeButtonStyle(nextState);
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        // If onClick is about to run toggleFreeze() it will call applyFreezeButtonStyle again
        // with the confirmed state — so nothing extra to do here.
        // Guard: restore correct appearance if user dragged away without completing the click.
        if (e.eventComponent == &freezeButton && !e.mouseWasClicked())
            applyFreezeButtonStyle(isFreezeActive);
    }

    // Double-click on vol knob: JUCE resets value via setDoubleClickReturnValue;
    // we add a brief white flash on volValueLabel to confirm the reset.
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &volumeKnob)
        {
            volValueLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0x88FFFFFF));
            volValueLabel.repaint();
            juce::Timer::callAfterDelay(150, [safeThis = juce::Component::SafePointer<SampleCard>(this)]()
            {
                if (auto* p = safeThis.getComponent())
                {
                    p->volValueLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
                    p->volValueLabel.repaint();
                }
            });
        }
    }

    void applyFreezeButtonStyle(bool on)
    {
        if (on)
        {
            freezeButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF00CFFF)); // ice blue
            freezeButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF111111)); // dark text
        }
        else
        {
            freezeButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A)); // dark inactive
            freezeButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE)); // off-white text
        }
    }

    //==============================================================================
    // Auto pitch detection (YIN algorithm on background thread)

    // Kick off a background pitch analysis for the current file + marker region.
    void startTuneAnalysis()
    {
        if (isTuneRunning.load()) return;
        if (!currentAudioFile.existsAsFile()) return;

        // Capture analysis params on the message thread
        tuneFile          = currentAudioFile;
        tuneStartSec      = startPointNormalized * originalDuration;
        tuneEndSec        = endPointNormalized   * originalDuration;
        tuneRootMidiNote  = currentMidiNote;
        tuneBaseTuningHz  = baseTuningHz; // use current global tuning reference

        // Visual: "Analyzing..." state
        isTuneRunning.store(true);
        tuneButton.setEnabled(false);
        tuneButton.setButtonText("...");
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF2A4A2A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF00FF88));

        // Stop any previous thread
        if (tuneThread != nullptr)
        {
            tuneThread->stopThread(500);
            tuneThread = nullptr;
        }

        tuneThread = std::make_unique<TuneAnalyzerThread>(*this);
        tuneThread->startThread();
    }

    // Runs on the background thread (called by TuneAnalyzerThread::run).
    void runTuneAnalysis()
    {
        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(tuneFile));

        if (reader == nullptr || juce::Thread::currentThreadShouldExit())
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        const double sr = reader->sampleRate;
        const juce::int64 totalSamples = reader->lengthInSamples;

        if (sr <= 0.0 || totalSamples <= 0)
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        // Derive sample indices for the analysis region
        juce::int64 startSample = (juce::int64)(tuneStartSec * sr);
        juce::int64 endSample   = (tuneEndSec > 0.0 && tuneEndSec < totalSamples / sr)
                                  ? (juce::int64)(tuneEndSec * sr)
                                  : totalSamples;
        startSample = juce::jlimit((juce::int64)0, totalSamples, startSample);
        endSample   = juce::jlimit(startSample + 1, totalSamples, endSample);

        juce::int64 numSamples = endSample - startSample;

        // Limit to 0.5 s for speed; take from start of region
        const juce::int64 maxBuf = (juce::int64)(sr * 0.5);
        numSamples = juce::jmin(numSamples, maxBuf);

        if (numSamples < 512)
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        if (juce::Thread::currentThreadShouldExit())
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        // Read audio into a multi-channel buffer then mix to mono
        const int numCh = juce::jmin((int)reader->numChannels, 2);
        juce::AudioBuffer<float> buf(numCh, (int)numSamples);
        reader->read(&buf, 0, (int)numSamples, startSample, true, true);

        std::vector<float> mono((size_t)numSamples, 0.0f);
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* data = buf.getReadPointer(ch);
            for (int i = 0; i < (int)numSamples; ++i)
                mono[(size_t)i] += data[i] / (float)numCh;
        }

        if (juce::Thread::currentThreadShouldExit())
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        const double freq = yinDetectPitch(mono.data(), (int)numSamples, sr);

        juce::MessageManager::callAsync([this, freq] { onTuneAnalysisComplete(freq); });
    }

    // Called back on the message thread with the detection result.
    void onTuneAnalysisComplete(double freqHz)
    {
        isTuneRunning.store(false);
        tuneButton.setEnabled(true);

        if (freqHz <= 0.0)
        {
            // No reliable pitch found
            detectedNoteName = "";
            detectedFreqHz   = 0.0;

            tuneButton.setButtonText("Tune");
            tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
            tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));

            bottomInfoLabel.setText("No pitch detected", juce::dontSendNotification);
            printf("[TUNE] No pitch detected\n");

            listeners.call([this](Listener& l) { l.detectedNoteChanged("", 0.0); });
            return;
        }

        // Map frequency to nearest MIDI note — use current base tuning reference
        const double exactNote = 69.0 + 12.0 * std::log2(freqHz / tuneBaseTuningHz);
        int nearestMidi = (int)std::round(exactNote);
        nearestMidi = juce::jlimit(0, 127, nearestMidi);

        // Build note name string
        static const char* noteNames[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        const int octave  = nearestMidi / 12 - 1;
        const int noteIdx = nearestMidi % 12;
        detectedNoteName = juce::String(noteNames[noteIdx]) + juce::String(octave);
        detectedFreqHz   = freqHz;

        // Store correction as the hidden base offset so "0 st" = this note.
        // User pitch resets to 0; MainComponent adds basePitchOffset when applying to the engine.
        const int correction = juce::jlimit(-48, 48, tuneRootMidiNote - nearestMidi);
        basePitchOffset = correction;
        pitchOffset     = 0;
        updatePitchDisplay(0);  // display shows "0 st" = in tune with detected note
        listeners.call([this](Listener& l) { l.pitchOffsetChanged(0); }); // MainComponent adds base

        // Now update bottomInfoLabel with detection result (overrides what updatePitchDisplay left)
        const int freqInt = (int)std::round(freqHz);
        bottomInfoLabel.setText(
            detectedNoteName + " - " + juce::String(freqInt) + " Hz",
            juce::dontSendNotification);

        // Button shows detected note in bright green
        tuneButton.setButtonText(detectedNoteName);
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF1A5A1A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF00FF88));

        printf("[TUNE] Detected: %s = %.1f Hz  (MIDI %d) → correction %+d st\n",
               detectedNoteName.toRawUTF8(), freqHz, nearestMidi, correction);

        // Notify listeners for persistence
        listeners.call([this](Listener& l) { l.detectedNoteChanged(detectedNoteName, detectedFreqHz); });
    }

    // YIN pitch detection algorithm.
    // Returns fundamental frequency in Hz, or -1.0 if not detected reliably.
    // Range: minFreqHz (C1=27Hz) to maxFreqHz (C8=4186Hz).
    static double yinDetectPitch(const float* mono, int numSamples, double sampleRate,
                                 double minFreqHz = 27.0, double maxFreqHz = 4186.0)
    {
        const int minPeriod = juce::jmax(2, (int)(sampleRate / maxFreqHz));
        const int maxPeriod = juce::jmin(numSamples / 2 - 1, (int)(sampleRate / minFreqHz));

        if (maxPeriod <= minPeriod) return -1.0;

        const int bufLen = numSamples - maxPeriod;  // usable window
        if (bufLen <= 0) return -1.0;

        // Step 1+2: Difference function d[tau] and Cumulative Mean Normalized Difference d'[tau]
        std::vector<double> cmnd((size_t)(maxPeriod + 1), 0.0);
        cmnd[0] = 1.0;
        double runningSum = 0.0;

        for (int tau = 1; tau <= maxPeriod; ++tau)
        {
            double sumSq = 0.0;
            for (int t = 0; t < bufLen; ++t)
            {
                const double diff = (double)mono[t] - (double)mono[t + tau];
                sumSq += diff * diff;
            }
            runningSum += sumSq;
            cmnd[(size_t)tau] = (runningSum > 0.0) ? sumSq * tau / runningSum : 0.0;
        }

        // Step 3: Absolute threshold — find first local minimum below 0.10
        const double absThreshold    = 0.10;
        const double fallbackThresh  = 0.30;
        int bestTau = -1;

        for (int tau = minPeriod; tau <= maxPeriod - 1; ++tau)
        {
            if (cmnd[(size_t)tau] < absThreshold && cmnd[(size_t)tau] <= cmnd[(size_t)(tau + 1)])
            {
                bestTau = tau;
                break;
            }
        }

        // If no absolute threshold minimum: use global minimum (with looser threshold)
        if (bestTau < 0)
        {
            double minVal = 1e9;
            for (int tau = minPeriod; tau <= maxPeriod; ++tau)
            {
                if (cmnd[(size_t)tau] < minVal)
                {
                    minVal = cmnd[(size_t)tau];
                    bestTau = tau;
                }
            }
            if (minVal > fallbackThresh)
                return -1.0;   // no reliable pitch
        }

        // Step 4: Parabolic interpolation for sub-sample accuracy
        double refinedTau = (double)bestTau;
        if (bestTau > minPeriod && bestTau < maxPeriod)
        {
            const double s0 = cmnd[(size_t)(bestTau - 1)];
            const double s1 = cmnd[(size_t) bestTau];
            const double s2 = cmnd[(size_t)(bestTau + 1)];
            const double denom = 2.0 * (2.0 * s1 - s0 - s2);
            if (std::abs(denom) > 1e-9)
                refinedTau = bestTau + (s2 - s0) / denom;
        }

        if (refinedTau <= 0.0) return -1.0;
        return sampleRate / refinedTau;
    }

    void toggleFreeze()
    {
        if (!isFreezeActive)
        {
            // Turning freeze ON
            loopWasOnBeforeFreeze = loopButton.getToggleState();
            isFreezeActive = true;
            applyFreezeButtonStyle(true);

            if (!loopWasOnBeforeFreeze)
            {
                // Loop was off — turn it on silently then notify
                setLoopEnabled(true);
                listeners.call([](Listener& l) { l.loopEnabledChanged(true); });
            }
            listeners.call([](Listener& l) { l.freezeChanged(true); });
        }
        else
        {
            // Turning freeze OFF
            isFreezeActive = false;
            applyFreezeButtonStyle(false);

            if (!loopWasOnBeforeFreeze)
            {
                // Loop was off before freeze — restore that state
                setLoopEnabled(false);
                listeners.call([](Listener& l) { l.loopEnabledChanged(false); });
            }
            // else: loop was already on — leave it on

            listeners.call([](Listener& l) { l.freezeChanged(false); });
        }
    }

};