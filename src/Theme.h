#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Two simple UI skins. Buttons keep their own colours in both themes; only the
// window chrome, panels and neutral text are recoloured.
enum class UiTheme { Dark = 0, Terminal = 1 };

struct Theme
{
    static inline UiTheme current = UiTheme::Dark;

    static bool isTerminal() { return current == UiTheme::Terminal; }

    static juce::Colour background()
    {
        return isTerminal() ? juce::Colour (0xFF04140A)   // near-black green
                            : juce::Colour (0xFF1E1E1E);
    }

    static juce::Colour card()
    {
        return isTerminal() ? juce::Colour (0xFF061C10)
                            : juce::Colour (0xFF636363);
    }

    static juce::Colour cardBorder()
    {
        return isTerminal() ? juce::Colour (0xFF1E8C46)
                            : juce::Colour (0xFF5A5A5A);
    }

    static juce::Colour padBackground()
    {
        return isTerminal() ? juce::Colour (0xFF020D06)
                            : juce::Colour (0xFF0E0E0E);
    }

    static juce::Colour text()
    {
        return isTerminal() ? juce::Colour (0xFF41FF7A)   // phosphor green
                            : juce::Colour (0xFFCECECE);
    }

    static juce::Colour dimText()
    {
        return isTerminal() ? juce::Colour (0xFF1FA34A)
                            : juce::Colour (0xFF888888);
    }

    // Waveform trace + oscilloscope grid.
    static juce::Colour traceEdge()
    {
        return isTerminal() ? juce::Colour (0xFF41FF7A) : juce::Colour (0xFF606060);
    }
    static juce::Colour traceCentre()
    {
        return isTerminal() ? juce::Colour (0xFF16B34D) : juce::Colour (0xFF1A1A1A);
    }
    static juce::Colour gridLine (bool major)
    {
        return isTerminal() ? juce::Colour (0xFF41FF7A).withAlpha (major ? 0.30f : 0.14f)
                            : juce::Colour (0xFF000000).withAlpha (major ? 0.15f : 0.08f);
    }
    static juce::Colour tickColour (bool major)
    {
        return isTerminal() ? juce::Colour (0xFF41FF7A).withAlpha (major ? 0.90f : 0.45f)
                            : juce::Colour (0xFF2A2A2A);
    }
    static juce::Colour crosshair()
    {
        return isTerminal() ? juce::Colour (0xFF41FF7A) : juce::Colour (0xFF303030);
    }

    static juce::Colour rulerLabel()
    {
        return isTerminal() ? juce::Colour (0xFF2FA552) : juce::Colour (0xFF2A2A2A);
    }

    // Waveform display (light "paper" panel in the dark theme).
    static juce::Colour waveBackground()
    {
        return isTerminal() ? juce::Colour (0xFF04160B) : juce::Colour (0xFFE0E0E0);
    }
    static juce::Colour waveBorder()
    {
        return isTerminal() ? juce::Colour (0xFF1E8C46) : juce::Colour (0xFFB0B0B0);
    }
    static juce::Colour waveRulerBackground()
    {
        return isTerminal() ? juce::Colour (0xFF0A2412) : juce::Colour (0xFFC8C8C8);
    }
    static juce::Colour waveRulerText()
    {
        return isTerminal() ? juce::Colour (0xFF2FA552) : juce::Colour (0xFFAAAAAA);
    }
    static juce::Colour waveLine()
    {
        return isTerminal() ? juce::Colour (0xFF41FF7A) : juce::Colour (0xFF555555);
    }
    static juce::Colour waveLoading()
    {
        return isTerminal() ? juce::Colour (0xFF0A2412) : juce::Colour (0xFFD0D0D0);
    }

    static juce::Colour outline()
    {
        return isTerminal() ? juce::Colour (0xFF0B5A28)
                            : juce::Colour (0xFF0A0A0A);
    }
};
