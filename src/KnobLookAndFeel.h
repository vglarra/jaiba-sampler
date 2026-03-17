#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

// Compact rotary knob LookAndFeel — fills bounding box with 2.5px margin.
// Colour IDs used:
//   rotarySliderFillColourId    — circle body fill
//   rotarySliderOutlineColourId — circle border
//   thumbColourId               — indicator line (set to a contrasting colour per knob)
struct CompactKnobLookAndFeel : public juce::LookAndFeel_V4
{
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                          float pos, float startAngle, float endAngle,
                          juce::Slider& s) override
    {
        const float margin = 2.5f;
        const float d = juce::jmin((float)w, (float)h) - margin * 2.0f;
        const float r = d * 0.5f;
        const float cx = (float)x + (float)w * 0.5f;
        const float cy = (float)y + (float)h * 0.5f;
        juce::Rectangle<float> knobBounds(cx - r, cy - r, d, d);

        g.setColour(s.findColour(juce::Slider::rotarySliderFillColourId));
        g.fillEllipse(knobBounds);

        g.setColour(s.findColour(juce::Slider::rotarySliderOutlineColourId));
        g.drawEllipse(knobBounds.reduced(0.5f), 1.5f);

        const float angle = startAngle + pos * (endAngle - startAngle);
        const float tickLen = r * 0.65f;
        g.setColour(s.findColour(juce::Slider::thumbColourId));
        g.drawLine(cx, cy,
                   cx + tickLen * std::sin(angle),
                   cy - tickLen * std::cos(angle),
                   2.5f);
    }
};
