#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// UI skins. Dark is the original grey look; the four "terminal" skins are the
// same layout with a different phosphor colour (buttons always keep their own
// colours — only the chrome, neutral text and waveform are recoloured).
enum class UiTheme { Dark = 0, Green, Amber, Cyan, White };

struct ThemePalette
{
    juce::Colour background, card, cardBorder, padBackground;
    juce::Colour text, dimText, outline, accent;
    juce::Colour waveBackground, waveBorder, waveRulerBackground, waveRulerText, waveLine, waveLoading;
    juce::Colour traceEdge, traceCentre, crosshair, rulerLabel;
};

struct Theme
{
    static inline UiTheme current = UiTheme::Dark;

    static bool isTerminal() { return current != UiTheme::Dark; }

    static juce::Colour c (juce::uint32 argb) { return juce::Colour (argb); }

    static ThemePalette make (juce::uint32 bg, juce::uint32 card, juce::uint32 border, juce::uint32 pad,
                              juce::uint32 text, juce::uint32 dim, juce::uint32 outline, juce::uint32 accent,
                              juce::uint32 waveBg, juce::uint32 waveBorder, juce::uint32 rulerBg,
                              juce::uint32 rulerText, juce::uint32 waveLine, juce::uint32 waveLoading,
                              juce::uint32 traceEdge, juce::uint32 traceCentre,
                              juce::uint32 crosshair, juce::uint32 rulerLabel)
    {
        ThemePalette p;
        p.background          = c (bg);
        p.card                = c (card);
        p.cardBorder          = c (border);
        p.padBackground       = c (pad);
        p.text                = c (text);
        p.dimText             = c (dim);
        p.outline             = c (outline);
        p.accent              = c (accent);
        p.waveBackground      = c (waveBg);
        p.waveBorder          = c (waveBorder);
        p.waveRulerBackground = c (rulerBg);
        p.waveRulerText       = c (rulerText);
        p.waveLine            = c (waveLine);
        p.waveLoading         = c (waveLoading);
        p.traceEdge           = c (traceEdge);
        p.traceCentre         = c (traceCentre);
        p.crosshair           = c (crosshair);
        p.rulerLabel          = c (rulerLabel);
        return p;
    }

    static const ThemePalette& palette()
    {
        // Original grey skin with the light "paper" waveform panel.
        static const ThemePalette dark = make (
            0xFF1E1E1E, 0xFF636363, 0xFF5A5A5A, 0xFF0E0E0E,
            0xFFCECECE, 0xFF888888, 0xFF0A0A0A, 0xFFB8DEFF,
            0xFFE0E0E0, 0xFFB0B0B0, 0xFFC8C8C8, 0xFFAAAAAA, 0xFF555555, 0xFFD0D0D0,
            0xFF606060, 0xFF1A1A1A, 0xFF303030, 0xFF2A2A2A);

        static const ThemePalette green = make (
            0xFF05180C, 0xFF000000, 0xFF22A052, 0xFF03100A,
            0xFF4DFF8A, 0xFF23B254, 0xFF0E6B30, 0xFF4DFF8A,
            0xFF000000, 0xFF22A052, 0xFF0A2412, 0xFF2FA552, 0xFF4DFF8A, 0xFF0A2412,
            0xFF4DFF8A, 0xFF1AC262, 0xFF4DFF8A, 0xFF2FA552);

        static const ThemePalette amber = make (
            0xFF171004, 0xFF000000, 0xFFC08A16, 0xFF100B02,
            0xFFFFD24A, 0xFFD9A62E, 0xFF7A5A0E, 0xFFFFD24A,
            0xFF000000, 0xFFC08A16, 0xFF241905, 0xFFE0B33A, 0xFFFFD24A, 0xFF241905,
            0xFFFFD24A, 0xFFC08A16, 0xFFFFD24A, 0xFFE0B33A);

        static const ThemePalette cyan = make (
            0xFF03161A, 0xFF000000, 0xFF1E8C9E, 0xFF021014,
            0xFF4DE8FF, 0xFF2FA8BC, 0xFF0C5A66, 0xFF4DE8FF,
            0xFF000000, 0xFF1E8C9E, 0xFF062229, 0xFF3FC4D8, 0xFF4DE8FF, 0xFF062229,
            0xFF4DE8FF, 0xFF1FA8C0, 0xFF4DE8FF, 0xFF3FC4D8);

        static const ThemePalette white = make (
            0xFF0A0A0A, 0xFF000000, 0xFF666666, 0xFF050505,
            0xFFF5F5F5, 0xFFB0B0B0, 0xFF3A3A3A, 0xFFF5F5F5,
            0xFF000000, 0xFF666666, 0xFF161616, 0xFFC8C8C8, 0xFFF0F0F0, 0xFF161616,
            0xFFFFFFFF, 0xFFBFBFBF, 0xFFFFFFFF, 0xFFC8C8C8);

        switch (current)
        {
            case UiTheme::Green: return green;
            case UiTheme::Amber: return amber;
            case UiTheme::Cyan:  return cyan;
            case UiTheme::White: return white;
            case UiTheme::Dark:
            default:             return dark;
        }
    }

    static juce::Colour background()          { return palette().background; }
    static juce::Colour card()                { return palette().card; }
    static juce::Colour cardBorder()          { return palette().cardBorder; }
    static juce::Colour padBackground()       { return palette().padBackground; }
    static juce::Colour text()                { return palette().text; }
    static juce::Colour dimText()             { return palette().dimText; }
    static juce::Colour outline()             { return palette().outline; }
    static juce::Colour accent()              { return palette().accent; }

    static juce::Colour waveBackground()      { return palette().waveBackground; }
    static juce::Colour waveBorder()          { return palette().waveBorder; }
    static juce::Colour waveRulerBackground() { return palette().waveRulerBackground; }
    static juce::Colour waveRulerText()       { return palette().waveRulerText; }
    static juce::Colour waveLine()            { return palette().waveLine; }
    static juce::Colour waveLoading()         { return palette().waveLoading; }

    static juce::Colour traceEdge()           { return palette().traceEdge; }
    static juce::Colour traceCentre()         { return palette().traceCentre; }
    static juce::Colour crosshair()           { return palette().crosshair; }
    static juce::Colour rulerLabel()          { return palette().rulerLabel; }

    static juce::Colour gridLine (bool major)
    {
        return isTerminal() ? palette().accent.withAlpha (major ? 0.30f : 0.14f)
                            : juce::Colour (0xFF000000).withAlpha (major ? 0.15f : 0.08f);
    }

    static juce::Colour tickColour (bool major)
    {
        return isTerminal() ? palette().accent.withAlpha (major ? 0.90f : 0.45f)
                            : juce::Colour (0xFF2A2A2A);
    }
};
