#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "MasterEq.h"

class FftVisualizerView : public juce::Component,
                          public juce::Timer
{
public:
    enum class Style { EqMaster, Spectrum, RingsParticles, Kaleidoscope };

    FftVisualizerView()
    {
        styleButton.setButtonText ("EQ");
        styleButton.setColour (juce::TextButton::buttonColourId,
                               juce::Colour (0xFF1a1a2e));
        styleButton.setColour (juce::TextButton::textColourOffId,
                               juce::Colour (0xFFe040e0));
        styleButton.onClick = [this] { toggleStyle(); };
        addAndMakeVisible (styleButton);

        sensitivityKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        sensitivityKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sensitivityKnob.setRange (0.1, 15.0, 0.01);
        sensitivityKnob.setValue (1.0, juce::dontSendNotification);
        sensitivityKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xFFe040e0));
        sensitivityKnob.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xFF0A0A0A));
        sensitivityKnob.setColour (juce::Slider::thumbColourId, juce::Colour (0xFFFFFFFF));
        sensitivityKnob.setTooltip ("Visualizer Sensitivity");
        sensitivityKnob.onValueChange = [this]
        {
            sensitivityAtomic.store ((float) sensitivityKnob.getValue());
            if (onSensitivityChanged)
                onSensitivityChanged ((float) sensitivityKnob.getValue());
        };
        addAndMakeVisible (sensitivityKnob);

        sensLabel.setText ("SENS", juce::dontSendNotification);
        sensLabel.setJustificationType (juce::Justification::centred);
        sensLabel.setColour (juce::Label::textColourId, juce::Colour (0xFFe040e0));
        sensLabel.setFont (juce::Font (11.0f));
        addAndMakeVisible (sensLabel);

        // Master EQ enable toggle (only shown in the EQ view).
        eqToggleButton.setClickingTogglesState (true);
        eqToggleButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFF3A3A3A));
        eqToggleButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFFFF4466));
        eqToggleButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFFCECECE));
        eqToggleButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        eqToggleButton.setToggleState (true, juce::dontSendNotification);
        eqToggleButton.onClick = [this]
        {
            eqEnabled = eqToggleButton.getToggleState();
            if (onMasterEqEnabledChanged)
                onMasterEqEnabledChanged (eqEnabled);
            repaint();
        };
        addAndMakeVisible (eqToggleButton);

        eqResetButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xFF4A4A4A));
        eqResetButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFCECECE));
        eqResetButton.setTooltip ("Reset the master EQ to flat (100/1k/8k Hz, 0 dB, Bell)");
        eqResetButton.onClick = [this]
        {
            for (int i = 0; i < 3; ++i)
            {
                static const float f[3] = { 100.0f, 1000.0f, 8000.0f };
                eqFreq[(size_t) i] = f[i];
                eqGain[(size_t) i] = 0.0f;
                eqMode[(size_t) i] = 2;
                if (onMasterEqBandChanged) onMasterEqBandChanged (i, f[i], 0.0f);
            }
            updateShapeButtonText();
            eqCurveDirty = true;
            if (onMasterEqReset) onMasterEqReset();
            repaint();
        };
        addAndMakeVisible (eqResetButton);

        eqShapeButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xFF2A2A2A));
        eqShapeButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFE040A0));
        eqShapeButton.setTooltip ("Filter shape of the selected band: Low Cut, Low Shelf, Bell, Notch, High Shelf, High Cut");
        eqShapeButton.onClick = [this]
        {
            if (activeBand < 0 || activeBand > 2)
                return;

            eqMode[(size_t) activeBand] = (eqMode[(size_t) activeBand] + 1) % 6;
            updateShapeButtonText();
            if (onMasterEqBandModeChanged)
                onMasterEqBandModeChanged (activeBand, eqMode[(size_t) activeBand]);
            eqCurveDirty = true;
            repaint();
        };
        addAndMakeVisible (eqShapeButton);

        updateShapeButtonText();
        updateStyleUi();   // EQ view is the default → SENS hidden
    }

    ~FftVisualizerView() override
    {
        stopVisualizer();
    }

    // Called from audio thread — push time-domain samples
    void pushSamples (const float* data, int numSamples)
    {
        int start1, size1, start2, size2;
        abstractFifo.prepareToWrite (numSamples, start1, size1, start2, size2);
        if (size1 > 0) fifoBuffer.copyFrom (0, start1, data, size1);
        if (size2 > 0) fifoBuffer.copyFrom (0, start2, data + size1, size2);
        abstractFifo.finishedWrite (size1 + size2);
    }

    // Starts the background FFT worker thread and the 60Hz repaint timer.
    void startVisualizer()
    {
        if (! fftThread.isThreadRunning())
            fftThread.startThread (juce::Thread::Priority::low);

        startTimerHz (60);
    }

    // Stops the repaint timer and the background FFT worker thread.
    void stopVisualizer()
    {
        stopTimer();
        fftThread.stopThread (1000);
    }

    void resized() override
    {
        styleButton.setBounds (getWidth() - 100, 10, 90, 28);

        sensitivityKnob.setBounds (10, 10, 36, 36);
        sensLabel.setBounds (8, 47, 40, 14);
        eqToggleButton.setBounds (10, 8, 72, 28);
        eqResetButton.setBounds  (88, 8, 76, 28);
        eqShapeButton.setBounds  (170, 8, 96, 28);
    }

    // Returns the current visualizer sensitivity (multiplies FFT bin values
    // before clamping). Range 0.1 - 5.0, default 1.0.
    float getSensitivity() const { return sensitivityAtomic.load(); }

    // Sets the visualizer sensitivity and updates the SENS knob to match
    // (used to restore a previously saved value without firing onSensitivityChanged).
    void setSensitivity (float newSensitivity)
    {
        newSensitivity = juce::jlimit (0.1f, 15.0f, newSensitivity);
        sensitivityAtomic.store (newSensitivity);
        sensitivityKnob.setValue ((double) newSensitivity, juce::dontSendNotification);
    }

    // Fired whenever the user changes the SENS knob — used to persist the value.
    std::function<void (float)> onSensitivityChanged;

    // ── Master EQ (edited in the EQ view, applied by the audio engine) ──────
    std::function<void (int band, float freq, float gainDb)> onMasterEqBandChanged;
    std::function<void (bool)> onMasterEqEnabledChanged;
    std::function<void (int band, int mode)> onMasterEqBandModeChanged;
    std::function<void()>       onMasterEqEditFinished;
    std::function<void()>       onMasterEqReset;

    void setMasterEqBands (const float freqs[3], const float gains[3],
                           const int modes[3], bool shouldBeEnabled)
    {
        for (int i = 0; i < 3; ++i)
        {
            eqFreq[(size_t) i] = freqs[i];
            eqGain[(size_t) i] = gains[i];
            eqMode[(size_t) i] = modes[i];
        }
        eqEnabled = shouldBeEnabled;
        eqToggleButton.setToggleState (eqEnabled, juce::dontSendNotification);
        updateShapeButtonText();
        eqCurveDirty = true;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF050510));
        switch (currentStyle)
        {
            case Style::EqMaster:      drawEqMaster (g);       break;
            case Style::Spectrum:      drawSpectrum (g);       break;
            case Style::Kaleidoscope:  drawKaleidoscope (g);   break;
            case Style::RingsParticles: drawRingsParticles (g); break;
        }
    }

private:
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder; // 2048
    static constexpr int numBins  = 512;

    juce::dsp::FFT forwardFFT { fftOrder };
    juce::dsp::WindowingFunction<float> window {
        (size_t) fftSize,
        juce::dsp::WindowingFunction<float>::hann };

    // FIFO — written by audio thread, drained by FftThread
    juce::AbstractFifo abstractFifo { fftSize * 2 };
    juce::AudioBuffer<float> fifoBuffer { 1, fftSize * 2 };

    float fftData[fftSize * 2] = {};
    float smoothedScope[numBins] = {};  // FftThread-owned running average
    float scopeData[numBins] = {};      // shared snapshot, guarded by scopeLock
    float displayScope[numBins] = {};   // message-thread-only copy used for painting

    // Real master-output level in dBFS per bin (smoothed), used by the EQ view.
    float smoothedDb[numBins] = {};
    float dbScope[numBins] = {};
    float displayDb[numBins] = {};
    juce::SpinLock scopeLock;

    juce::TextButton styleButton;
    Style currentStyle = Style::EqMaster;

    // Master EQ band state edited by the graph.
    static constexpr float kEqMinDb = -24.0f, kEqMaxDb = 24.0f;
    float eqFreq[3] { 100.0f, 1000.0f, 8000.0f };
    float eqGain[3] { 0.0f, 0.0f, 0.0f };
    int   eqMode[3] { 2, 2, 2 };          // 0 LowCut … 5 HighCut, 2 = Bell
    bool  eqEnabled = true;
    int   activeBand = 0;                  // selected band (shape button target)
    juce::TextButton eqToggleButton { "EQ ON" };

    // Cached EQ response path — rebuilt only when the bands or the plot change.
    juce::Path eqCurvePath, eqFillPath;
    bool eqCurveDirty = true;
    juce::Rectangle<float> eqCurvePlot;
    juce::TextButton eqResetButton  { "Reset" };
    juce::TextButton eqShapeButton  { "Bell" };
    float phase = 0.0f;

    // Sensitivity control — top-left of the visualizer. Scales FFT bin values
    // before clamping. Atomic copy is read by FftThread; knob lives on the
    // message thread.
    juce::Slider sensitivityKnob;
    juce::Label  sensLabel;
    std::atomic<float> sensitivityAtomic { 1.0f };

    // Particle system for RingsParticles mode
    struct Particle {
        float x, y, vx, vy, life, size;
        juce::Colour colour;
    };
    std::vector<Particle> particles;

    // Background FFT worker — drains the FIFO and computes the spectrum so
    // the message thread (timer/paint) never touches FFT math.
    class FftThread : public juce::Thread
    {
    public:
        explicit FftThread (FftVisualizerView& ownerToUse)
            : juce::Thread ("FFT Visualizer"), owner (ownerToUse) {}

        void run() override
        {
            while (! threadShouldExit())
            {
                if (owner.abstractFifo.getNumReady() >= fftSize)
                    owner.processFftOnWorkerThread();
                else
                    wait (5);
            }
        }

    private:
        FftVisualizerView& owner;
    };

    FftThread fftThread { *this };

    void updateShapeButtonText()
    {
        const int b = juce::jlimit (0, 2, activeBand);
        eqShapeButton.setButtonText (masterEqModeName (eqMode[(size_t) b]));
    }

    void updateStyleUi()
    {
        // The EQ view shows real dBFS values, so the display sensitivity knob
        // has no meaning there.
        const bool showSens = (currentStyle != Style::EqMaster);
        sensitivityKnob.setVisible (showSens);
        sensLabel.setVisible (showSens);
        eqToggleButton.setVisible (! showSens);
        eqResetButton.setVisible (! showSens);
        eqShapeButton.setVisible (! showSens);
    }

    void toggleStyle()
    {
        switch (currentStyle)
        {
            case Style::EqMaster:
                currentStyle = Style::Spectrum;
                styleButton.setButtonText ("SPECTRUM");
                break;
            case Style::Spectrum:
                currentStyle = Style::RingsParticles;
                styleButton.setButtonText ("RINGS");
                break;
            case Style::RingsParticles:
                currentStyle = Style::Kaleidoscope;
                styleButton.setButtonText ("KALEIDO");
                break;
            case Style::Kaleidoscope:
                currentStyle = Style::EqMaster;
                styleButton.setButtonText ("EQ");
                break;
        }

        updateStyleUi();
    }

    // Master EQ view: three editable bands over a faint master spectrum.
    juce::Rectangle<float> eqPlot() const
    {
        auto area = getLocalBounds().toFloat().reduced (6.0f);
        return area.withTrimmedTop (36.0f).withTrimmedLeft (36.0f).withTrimmedBottom (14.0f);
    }

    float eqYForDb (juce::Rectangle<float> plot, float db) const
    {
        const float t = juce::jlimit (0.0f, 1.0f, (kEqMaxDb - db) / (kEqMaxDb - kEqMinDb));
        return plot.getY() + t * plot.getHeight();
    }

    float eqXForHz (juce::Rectangle<float> plot, float hz) const
    {
        const float t = juce::jlimit (0.0f, 1.0f,
                                      std::log (juce::jmax (20.0f, hz) / 20.0f) / std::log (1000.0f));
        return plot.getX() + t * plot.getWidth();
    }

    void rebuildEqCurve (juce::Rectangle<float> plot)
    {
        MasterEqCoeffs bandCoeffs[3];
        for (int i = 0; i < 3; ++i)
            bandCoeffs[i] = makeEqCoeffs (eqFreq[(size_t) i], eqGain[(size_t) i], 1.0f,
                                          eqMode[(size_t) i], 48000.0);

        eqCurvePath.clear();
        eqFillPath.clear();

        const int w = juce::jmax (1, (int) plot.getWidth());
        for (int px = 0; px < w; ++px)
        {
            const double frac = (double) px / (double) juce::jmax (1, w - 1);
            const float  hz   = (float) (20.0 * std::pow (1000.0, frac));

            float db = 0.0f;
            for (int i = 0; i < 3; ++i)
                db += juce::Decibels::gainToDecibels (biquadMagnitude (bandCoeffs[i], hz, 48000.0), -100.0f);

            const float y = eqYForDb (plot, db);
            const float x = plot.getX() + (float) px;

            if (px == 0)
            {
                eqCurvePath.startNewSubPath (x, y);
                eqFillPath.startNewSubPath (x, plot.getBottom());
                eqFillPath.lineTo (x, y);
            }
            else
            {
                eqCurvePath.lineTo (x, y);
                eqFillPath.lineTo (x, y);
            }
        }

        eqFillPath.lineTo (plot.getRight(), plot.getBottom());
        eqFillPath.closeSubPath();
    }

    void drawEqMaster (juce::Graphics& g)
    {
        auto area = getLocalBounds().toFloat().reduced (6.0f);
        auto plot = eqPlot();

        g.setColour (juce::Colour (0xFF0D0D0D));
        g.fillRect (plot);
        g.setColour (juce::Colour (0xFF333333));
        g.drawRect (plot, 1.0f);

        // Gain grid (+24 … -24 dB), same scale as the per-pad EQ graph.
        for (float db = kEqMaxDb; db >= kEqMinDb - 0.1f; db -= 12.0f)
        {
            const float y = eqYForDb (plot, db);
            g.setColour (std::abs (db) < 0.1f ? juce::Colour (0xFF555555) : juce::Colour (0xFF2E2E2E));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (juce::Colour (0xFFBBBBBB));
            g.setFont (juce::Font (9.0f));
            g.drawText ((db > 0.0f ? "+" : "") + juce::String ((int) db) + " dB",
                        (int) area.getX(), (int) y - 6, 34, 12,
                        juce::Justification::centredRight);
        }

        // Log-frequency grid.
        for (float f : { 100.0f, 1000.0f, 10000.0f })
        {
            const float x = eqXForHz (plot, f);
            g.setColour (juce::Colour (0xFF2E2E2E));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colour (0xFFBBBBBB));
            g.setFont (juce::Font (9.0f));
            g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String ((int) f),
                        (int) x - 20, (int) plot.getBottom() + 2, 40, 12,
                        juce::Justification::centred);
        }

        const int   w     = juce::jmax (1, (int) plot.getWidth());
        const float binHz = 24000.0f / (float) numBins;

        // Faint real master spectrum behind the curve (no labels: the axis is gain).
        {
            juce::Path spectrum;
            for (int px = 0; px < w; ++px)
            {
                const double frac = (double) px / (double) juce::jmax (1, w - 1);
                const double f    = 20.0 * std::pow (1000.0, frac);
                const int    bin  = juce::jlimit (1, numBins - 1, (int) (f / binHz));
                const float  db   = juce::jlimit (-80.0f, 0.0f, displayDb[(size_t) bin]);
                const float  y    = plot.getBottom() - (db + 80.0f) / 80.0f * plot.getHeight();
                const float  x    = plot.getX() + (float) px;
                if (px == 0) spectrum.startNewSubPath (x, y);
                else         spectrum.lineTo (x, y);
            }
            g.setColour (juce::Colour (0x33AAAAAA));
            g.strokePath (spectrum, juce::PathStrokeType (1.0f));
        }

        // EQ response curve — rebuilt only when the bands or plot change, so a
        // drag does not re-run trig maths for every pixel at 60 fps.
        if (eqCurveDirty || eqCurvePlot != plot)
        {
            rebuildEqCurve (plot);
            eqCurveDirty = false;
            eqCurvePlot  = plot;
        }

        g.setColour (juce::Colour (eqEnabled ? 0x22FF4466 : 0x15885566));
        g.fillPath (eqFillPath);
        g.setColour (juce::Colour (eqEnabled ? 0xFFFF4466 : 0xFF885566));
        g.strokePath (eqCurvePath, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));

        // Band handles.
        for (int i = 0; i < 3; ++i)
        {
            const float x = eqXForHz (plot, eqFreq[(size_t) i]);
            const float y = eqYForDb (plot, eqGain[(size_t) i]);
            const bool  active = (i == activeBand);

            g.setColour (juce::Colour (active ? 0xFF44E8FF : 0xFF00CFFF));
            g.fillEllipse (x - 9.0f, y - 9.0f, 18.0f, 18.0f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::Font (11.0f, juce::Font::bold));
            g.drawText (juce::String (i + 1),
                        juce::Rectangle<float> (x - 9.0f, y - 9.0f, 18.0f, 18.0f),
                        juce::Justification::centred);
        }

        g.setColour (juce::Colour (0xFFBBBBBB));
        g.setFont (juce::Font (10.0f));
        g.drawText ("MASTER EQ", (int) plot.getX() + 6, (int) plot.getY() + 4, 120, 14,
                    juce::Justification::left);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (currentStyle != Style::EqMaster)
            return;

        const auto plot = eqPlot();
        float best = 32.0f;
        for (int i = 0; i < 3; ++i)
        {
            const float x = eqXForHz (plot, eqFreq[(size_t) i]);
            const float y = eqYForDb (plot, eqGain[(size_t) i]);
            const float d = e.position.getDistanceFrom ({ x, y });
            if (d < best) { best = d; activeBand = i; }
        }
        updateShapeButtonText();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (currentStyle != Style::EqMaster || activeBand < 0)
            return;

        const auto plot = eqPlot();
        const float fx = juce::jlimit (0.0f, 1.0f,
                                       (e.position.x - plot.getX()) / juce::jmax (1.0f, plot.getWidth()));
        const float fy = juce::jlimit (0.0f, 1.0f,
                                       (e.position.y - plot.getY()) / juce::jmax (1.0f, plot.getHeight()));

        eqFreq[(size_t) activeBand] = 20.0f * std::pow (1000.0f, fx);
        eqGain[(size_t) activeBand] = kEqMaxDb - fy * (kEqMaxDb - kEqMinDb);

        if (onMasterEqBandChanged)
            onMasterEqBandChanged (activeBand, eqFreq[(size_t) activeBand], eqGain[(size_t) activeBand]);

        eqCurveDirty = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (activeBand >= 0 && onMasterEqEditFinished)
            onMasterEqEditFinished();

        repaint();
    }

    // Runs on FftThread — drains FIFO, performs FFT, compresses to numBins
    // with smoothing, then copies the result into scopeData under scopeLock.
    void processFftOnWorkerThread()
    {
        int start1, size1, start2, size2;
        abstractFifo.prepareToRead (fftSize, start1, size1, start2, size2);
        if (size1 > 0) juce::FloatVectorOperations::copy (
            fftData, fifoBuffer.getReadPointer (0, start1), size1);
        if (size2 > 0) juce::FloatVectorOperations::copy (
            fftData + size1, fifoBuffer.getReadPointer (0, start2), size2);
        abstractFifo.finishedRead (size1 + size2);

        window.multiplyWithWindowingTable (fftData, fftSize);
        forwardFFT.performFrequencyOnlyForwardTransform (fftData);

        const float sensitivity = sensitivityAtomic.load();

        for (int i = 0; i < numBins; ++i)
        {
            float mapped = juce::jmap (
                (float) i, 0.0f, (float) numBins,
                0.0f, (float) (fftSize / 2));
            float val = fftData[(int) mapped] / (float) (fftSize / 2);
            val = juce::jlimit (0.0f, 1.0f, val * sensitivity);
            // Curve low-level signal upward for a punchier, more reactive display.
            val = std::pow (val, 0.6f);
            smoothedScope[i] = smoothedScope[i] * 0.75f + val * 0.25f;

            // Unscaled magnitude -> real dBFS, smoothed in the dB domain.
            const float raw = fftData[(int) mapped] / (float) (fftSize / 2);
            const float db  = juce::Decibels::gainToDecibels (raw, -100.0f);
            smoothedDb[i]   = smoothedDb[i] * 0.82f + db * 0.18f;
        }

        const juce::SpinLock::ScopedLockType sl (scopeLock);
        std::copy (std::begin (smoothedScope), std::end (smoothedScope), scopeData);
        std::copy (std::begin (smoothedDb), std::end (smoothedDb), dbScope);
    }

    // Timer only advances the animation phase, snapshots scopeData for
    // painting, and triggers a repaint — all FFT math happens on FftThread.
    void timerCallback() override
    {
        {
            const juce::SpinLock::ScopedLockType sl (scopeLock);
            std::copy (std::begin (scopeData), std::end (scopeData), displayScope);
            std::copy (std::begin (dbScope),   std::end (dbScope),   displayDb);
        }

        phase += 0.012f;
        if (phase > juce::MathConstants<float>::twoPi)
            phase -= juce::MathConstants<float>::twoPi;

        repaint();
    }

    // ── SPECTRUM ─────────────────────────────────────────────────
    // Classic frequency analyzer: log-spaced bars across the display, driven
    // by the FFT bin magnitudes in displayScope (smoothed on FftThread).
    void drawSpectrum (juce::Graphics& g)
    {
        auto w = (float) getWidth();
        auto h = (float) getHeight();

        constexpr int kNumBars = 72;                 // visual bar count
        constexpr float kMaxBinF = (float) numBins;  // bin indices 0..numBins-1
        const float barW = w / (float) kNumBars;
        const float baseY = h * 0.92f;               // baseline above the SENS UI

        // Geometric bin spacing gives low frequencies (left) more resolution,
        // like a real analyzer. binF goes ~1..numBins across the bar row.
        for (int c = 0; c < kNumBars; ++c)
        {
            float t = (float) c / (float) (kNumBars - 1);
            float binF = 1.0f + (kMaxBinF - 1.0f) * (t * t * (3.0f - 2.0f * t)); // smoothstep-ish: more low-freq detail
            // Weighted average over a small band around binF for stable bars.
            int binLo = juce::jlimit (0, numBins - 1, (int) (binF - 1.0f));
            int binHi = juce::jlimit (0, numBins - 1, (int) (binF + 1.0f));
            float val = 0.0f;
            for (int b = binLo; b <= binHi; ++b) val += displayScope[b];
            val /= (float) (binHi - binLo + 1);

            // Brightness ramps with level; cap to keep the bars inside.
            float mag = juce::jlimit (0.0f, 1.0f, val * 1.15f);
            float barH = mag * (baseY - 14.0f);
            float x = (float) c * barW;

            // Colour sweeps blue -> violet -> magenta from low to high freq.
            float hue = std::fmod (0.6f + t * 0.35f, 1.0f);
            g.setColour (juce::Colour::fromHSV (hue, 0.85f,
                                                0.35f + mag * 0.65f,
                                                0.9f));
            if (barH >= 1.0f)
                g.fillRect (x + 0.5f, baseY - barH, barW - 1.0f, barH);

            // Thin peak cap line so small signals stay visible.
            g.setColour (juce::Colour::fromHSV (hue, 0.9f, 0.95f, 0.9f));
            if (barH >= 2.0f)
                g.fillRect (x + 0.5f, baseY - barH, barW - 1.0f, 2.0f);
        }

        // Baseline.
        g.setColour (juce::Colour (0xFF22223A));
        g.fillRect (0.0f, baseY, w, 2.0f);
    }

    // ── KALEIDOSCOPE ──────────────────────────────────────────────
    void drawKaleidoscope (juce::Graphics& g)
    {
        auto w = (float) getWidth();
        auto h = (float) getHeight();
        float cx = w * 0.5f, cy = h * 0.5f;

        int numSymmetry = 8;   // octagonal kaleidoscope

        for (int sym = 0; sym < numSymmetry; ++sym)
        {
            float angle = (float) sym * juce::MathConstants<float>::twoPi / (float) numSymmetry;
            float mirrorAngle = angle + juce::MathConstants<float>::twoPi / (float) numSymmetry;

            juce::Path p;
            bool started = false;

            for (int i = 0; i < numBins; ++i)
            {
                float t  = (float) i / (float) numBins;
                float mag = displayScope[i];
                float r = (0.08f + t * 0.42f + mag * 0.55f) * std::min (w, h);
                float a = angle + (mirrorAngle - angle) * t + phase * (sym % 2 == 0 ? 0.3f : -0.3f);

                float x = cx + r * std::cos (a);
                float y = cy + r * std::sin (a);

                if (!started) { p.startNewSubPath (x, y); started = true; }
                else          { p.lineTo (x, y); }
            }

            // Mirror arm
            for (int i = numBins - 1; i >= 0; --i)
            {
                float t   = (float) i / (float) numBins;
                float mag = displayScope[i];
                float r   = (0.08f + t * 0.42f + mag * 0.28f) * std::min (w, h);
                float a   = mirrorAngle + (angle - mirrorAngle) * t
                            + phase * (sym % 2 == 0 ? 0.3f : -0.3f);
                p.lineTo (cx + r * std::cos (a), cy + r * std::sin (a));
            }
            p.closeSubPath();

            // Colour cycles through hue based on magnitude + symmetry segment
            float hue = std::fmod (0.6f + (float) sym / (float) numSymmetry + phase * 0.05f, 1.0f);
            float energy = 0.0f;
            for (int i = 0; i < numBins; i += 8) energy += displayScope[i];
            energy = juce::jlimit (0.0f, 1.0f, energy / 64.0f);

            // Outer glow pass — wide, soft stroke for extra punch
            g.setColour (juce::Colour::fromHSV (hue, 0.9f, 0.7f + energy * 0.3f, 0.15f + energy * 0.35f));
            g.strokePath (p, juce::PathStrokeType (5.0f + energy * 10.0f));

            g.setColour (juce::Colour::fromHSV (hue, 0.95f, 0.65f + energy * 0.35f, 0.75f + energy * 0.25f));
            g.strokePath (p, juce::PathStrokeType (1.4f + energy * 4.0f));
            g.setColour (juce::Colour::fromHSV (hue, 0.8f, 0.4f + energy * 0.6f, 0.22f + energy * 0.35f));
            g.fillPath (p);
        }
    }

    // ── RINGS + PARTICLES ─────────────────────────────────────────
    void drawRingsParticles (juce::Graphics& g)
    {
        auto w = (float) getWidth();
        auto h = (float) getHeight();
        float cx = w * 0.5f, cy = h * 0.5f;

        // Energy bands for particle spawning
        float bassEnergy = 0.0f, midEnergy = 0.0f, highEnergy = 0.0f;
        for (int i = 0;   i < 40;       ++i) bassEnergy += displayScope[i];
        for (int i = 40;  i < 180;      ++i) midEnergy  += displayScope[i];
        for (int i = 180; i < numBins;  ++i) highEnergy += displayScope[i];
        bassEnergy  = juce::jlimit (0.0f, 1.0f, bassEnergy  / 40.0f);
        midEnergy   = juce::jlimit (0.0f, 1.0f, midEnergy   / 140.0f);
        highEnergy  = juce::jlimit (0.0f, 1.0f, highEnergy  / 332.0f);

        // Draw concentric frequency hexagons
        int numRings = 6;
        constexpr int sides = 6;
        for (int r = 0; r < numRings; ++r)
        {
            float t = (float) r / (float) numRings;
            juce::Path ring;

            for (int s = 0; s <= sides; ++s)
            {
                int vertexIdx = s % sides;
                float angle = (float) vertexIdx / (float) sides
                              * juce::MathConstants<float>::twoPi;
                int binIdx = juce::jlimit (0, numBins - 1,
                                            (int) (t * (float) (numBins - 1)) + vertexIdx * 12);
                float offset = displayScope[binIdx] * std::min (w, h) * 0.32f;
                float radius = (0.06f + t * 0.38f) * std::min (w, h) + offset;
                float rotated = angle + phase * (r % 2 == 0 ? 0.4f : -0.4f);
                float px = cx + radius * std::cos (rotated);
                float py = cy + radius * std::sin (rotated);

                if (s == 0) ring.startNewSubPath (px, py);
                else        ring.lineTo (px, py);
            }
            ring.closeSubPath();

            float hue = std::fmod (0.55f + t * 0.4f + phase * 0.04f, 1.0f);
            float energy = r < 2 ? bassEnergy : (r < 4 ? midEnergy : highEnergy);

            // Glow pass — wide, soft stroke for extra punch
            g.setColour (juce::Colour::fromHSV (hue, 0.85f, 0.75f + energy * 0.25f, 0.12f + energy * 0.30f));
            g.strokePath (ring, juce::PathStrokeType (6.0f + energy * 12.0f));

            g.setColour (juce::Colour::fromHSV (hue, 0.9f, 0.65f + energy * 0.35f, 0.85f));
            g.strokePath (ring, juce::PathStrokeType (1.5f + energy * 5.0f));
        }

        // Spawn particles on beats
        juce::Random rng;
        int toSpawn = (int) (bassEnergy * 10.0f) + (int) (midEnergy * 6.0f);
        for (int i = 0; i < toSpawn && particles.size() < 600; ++i)
        {
            float angle = rng.nextFloat() * juce::MathConstants<float>::twoPi;
            float speed = 0.8f + rng.nextFloat() * 4.0f + bassEnergy * 5.0f;
            float hue   = std::fmod (highEnergy * 0.4f + rng.nextFloat() * 0.3f, 1.0f);
            particles.push_back ({
                cx, cy,
                std::cos (angle) * speed,
                std::sin (angle) * speed,
                1.0f,
                2.0f + rng.nextFloat() * 4.5f,
                juce::Colour::fromHSV (hue, 0.9f, 1.0f, 0.9f)
            });
        }

        // Update + draw particles
        for (auto it = particles.begin(); it != particles.end(); )
        {
            it->x    += it->vx;
            it->y    += it->vy;
            it->life -= 0.012f;
            it->vx   *= 0.985f;
            it->vy   *= 0.985f;

            if (it->life <= 0.0f || it->x < 0 || it->x > w
                || it->y < 0 || it->y > h)
            {
                it = particles.erase (it);
            }
            else
            {
                g.setColour (it->colour.withAlpha (it->life * 0.95f));
                g.fillEllipse (it->x - it->size * 0.5f, it->y - it->size * 0.5f,
                               it->size, it->size);
                ++it;
            }
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FftVisualizerView)
};
