#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

// Dual-channel segmented LED-block level meter (VU-meter style).
// Two std::atomic<float> sources (L / R) are written by the audio thread each block.
// A 30fps timer reads them and updates peak-hold state; paint() draws the segments.
class LevelMeter : public juce::Component, public juce::Timer
{
public:
    LevelMeter()  { startTimerHz(30); }
    ~LevelMeter() override { stopTimer(); }

    void setSources(const std::atomic<float>* l, const std::atomic<float>* r)
    {
        srcL = l;
        srcR = r;
    }

    void timerCallback() override
    {
        if (!srcL) return;
        const float lv = srcL->load(std::memory_order_relaxed);
        const float rv = srcR ? srcR->load(std::memory_order_relaxed) : lv;
        tick(stateL, lv);
        tick(stateR, rv);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const float W = (float)getWidth();
        const float H = (float)getHeight();

        g.setColour(juce::Colour(0xFF111111));
        g.fillRoundedRectangle(0.0f, 0.0f, W, H, 3.0f);
        g.setColour(juce::Colour(0xFF333333));
        g.drawRoundedRectangle(0.5f, 0.5f, W - 1.0f, H - 1.0f, 3.0f, 1.0f);

        constexpr float kLabelW  = 18.0f;
        constexpr float kChanGap =  4.0f;
        constexpr float kTopPad  =  4.0f;
        constexpr float kBotPad  = 11.0f;   // room for "dB" label

        const float chanW    = (W - kLabelW * 2.0f - kChanGap) * 0.5f;
        const float lChanX   = kLabelW;
        const float rChanX   = kLabelW + chanW + kChanGap;
        const float meterTop = kTopPad;
        const float meterH   = H - kTopPad - kBotPad;

        drawChannel(g, lChanX, meterTop, chanW, meterH, stateL);
        drawChannel(g, rChanX, meterTop, chanW, meterH, stateR);
        drawLabels (g, 0.0f, meterTop, kLabelW, meterH, W);

        // "dB" label centred below the meter
        g.setColour(juce::Colour(0xFFAAAAAA));
        g.setFont(juce::Font(7.0f));
        g.drawText("dB", 0, (int)(H - kBotPad), (int)W, (int)kBotPad,
                   juce::Justification::centred, false);
    }

private:
    // ── Segment scale ────────────────────────────────────────────────────────────
    // 20 segments, bottom (index 0) = quietest, top (index 19) = loudest.
    static constexpr int   kN         = 20;
    static constexpr float kSegGap    = 2.0f;
    static constexpr int   kHoldTicks = 45;    // 1.5 s @ 30 fps
    static constexpr float kPeakDecay = 0.92f;

    static float segDb(int i) noexcept
    {
        static const float db[kN] = {
            -60.f, -50.f, -40.f, -34.f, -28.f,
            -24.f, -20.f, -16.f, -14.f, -12.f,
            -10.f,  -8.f,  -6.f,  -4.f,  -2.f,
              0.f,  +2.f,  +4.f,  +6.f,  +8.f
        };
        return db[i];
    }

    // Label string for each segment; nullptr = no label drawn.
    static const char* segLabel(int i) noexcept
    {
        static const char* lb[kN] = {
            "-60", nullptr, "-40", nullptr, "-28",
            nullptr, "-20", "-16", nullptr, "-12",
            nullptr,  "-8", nullptr,  "-4",  "-2",
               "0",  "+2",  "+4",  "+6",  "+8"
        };
        return lb[i];
    }

    // ── Colours ───────────────────────────────────────────────────────────────────
    static juce::Colour litCol(int i) noexcept
    {
        const float db = segDb(i);
        if (db >= 8.f)  return juce::Colour(0xFFCC0000);  // deep red
        if (db >= 2.f)  return juce::Colour(0xFFFF6600);  // orange
        return               juce::Colour(0xFF00E5CC);    // cyan/teal
    }

    static juce::Colour unlitCol(int i) noexcept
    {
        const float db = segDb(i);
        if (db >= 8.f)  return juce::Colour(0xFF220000);  // dark red
        if (db >= 2.f)  return juce::Colour(0xFF2E1A00);  // dark orange
        return               juce::Colour(0xFF0A2E2A);    // dark teal
    }

    // ── Per-channel state ─────────────────────────────────────────────────────────
    struct Chan
    {
        float current   = 0.0f;
        float peakHold  = 0.0f;
        int   holdCount = 0;
    };

    const std::atomic<float>* srcL = nullptr;
    const std::atomic<float>* srcR = nullptr;
    Chan stateL, stateR;

    static void tick(Chan& c, float level) noexcept
    {
        c.current = level;
        if (level > c.peakHold)
        {
            c.peakHold  = level;
            c.holdCount = kHoldTicks;
        }
        else if (c.holdCount > 0)
        {
            --c.holdCount;
        }
        else
        {
            c.peakHold *= kPeakDecay;
            if (c.peakHold < 1e-5f) c.peakHold = 0.0f;
        }
    }

    // ── Geometry helpers ──────────────────────────────────────────────────────────
    // Returns the top-Y and height of segment i.
    // Segment 0 is at the bottom; segment kN-1 is at the top.
    static void segGeom(int i, float meterTop, float meterH,
                        float& top, float& h) noexcept
    {
        h = (meterH - (kN - 1) * kSegGap) / (float)kN;
        const int fromTop = (kN - 1) - i;
        top = meterTop + (float)fromTop * (h + kSegGap);
    }

    // Y coordinate for a given dB level (interpolated between segment centres).
    static float dbToY(float db, float meterTop, float meterH) noexcept
    {
        db = juce::jlimit(segDb(0), segDb(kN - 1), db);
        for (int i = 0; i < kN - 1; ++i)
        {
            if (db >= segDb(i) && db <= segDb(i + 1))
            {
                float t1, h1, t2, h2;
                segGeom(i,     meterTop, meterH, t1, h1);
                segGeom(i + 1, meterTop, meterH, t2, h2);
                const float frac = (db - segDb(i)) / (segDb(i + 1) - segDb(i));
                const float c1   = t1 + h1 * 0.5f;
                const float c2   = t2 + h2 * 0.5f;
                return c1 + frac * (c2 - c1);
            }
        }
        float t, h;
        segGeom(kN - 1, meterTop, meterH, t, h);
        return t + h * 0.5f;
    }

    static float toDb(float linear) noexcept
    {
        if (linear < 1e-7f) return -100.0f;
        return juce::Decibels::gainToDecibels(linear);
    }

    // ── Drawing ───────────────────────────────────────────────────────────────────
    void drawChannel(juce::Graphics& g,
                     float x, float meterTop, float w, float meterH,
                     const Chan& c) const
    {
        const float levelDb = toDb(c.current);

        for (int i = 0; i < kN; ++i)
        {
            float top, h;
            segGeom(i, meterTop, meterH, top, h);
            g.setColour((levelDb >= segDb(i)) ? litCol(i) : unlitCol(i));
            g.fillRoundedRectangle(x, top, w, h, 1.0f);
        }

        // Peak-hold indicator — 2 px bright-white horizontal bar
        if (c.peakHold > 1e-4f)
        {
            const float py = dbToY(toDb(c.peakHold), meterTop, meterH);
            g.setColour(juce::Colours::white);
            g.fillRect(x, py - 1.0f, w, 2.0f);
        }
    }

    void drawLabels(juce::Graphics& g,
                    float leftX, float meterTop, float labelW,
                    float meterH, float totalW) const
    {
        g.setFont(juce::Font(7.0f));
        g.setColour(juce::Colour(0xFFAAAAAA));
        const float rightX = totalW - labelW;

        for (int i = 0; i < kN; ++i)
        {
            const char* lbl = segLabel(i);
            if (!lbl) continue;

            float top, h;
            segGeom(i, meterTop, meterH, top, h);
            const int y = (int)(top + h * 0.5f) - 3;

            g.drawText(lbl, (int)leftX, y, (int)labelW - 1, 8,
                       juce::Justification::centredRight, false);
            g.drawText(lbl, (int)rightX + 1, y, (int)labelW - 1, 8,
                       juce::Justification::centredLeft, false);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};
