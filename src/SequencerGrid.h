#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Sequencer.h"

//==============================================================================
// Seq tab — step-sequencer shell.
//
// Phase 1 is deliberately silent: it edits and persists the pattern only.  The
// playback engine lands in Phase 2.
//
// Layout:
//   [ toolbar: transport | snap | bars | bpm | mode | lock ]
//   [ fixed row header (150) ][ ruler                        ]
//   [ fixed row header        ][ scrolling 16-row step grid   ]
//==============================================================================
class SeqControlPanel : public juce::Component
{
public:
    static constexpr int kRowH     = 14;
    static constexpr int kRulerH   = 12;
    static constexpr int kHeaderW  = 150;
    static constexpr int kStepW    = 20;
    static constexpr int kToolbarH = 20;

    // Row-header column split (within kHeaderW).
    static constexpr int kNumW   = 22;
    static constexpr int kVolW   = 36;
    static constexpr int kMuteW  = 18;

    //==========================================================================
    // Fired whenever the pattern is edited (MainComponent marks the kit dirty).
    std::function<void()> onEdited;

    SeqControlPanel();
    ~SeqControlPanel() override = default;

    /** Pattern to view/edit. May be null (nothing shown). */
    void setPattern (SeqPattern* p);
    /** The 16 live pads, used only as a dim fallback label before a track is captured. */
    void setPadSettings (const PadSettings* p);
    /** Re-read the pattern after an external change (bank switch / kit load). */
    void refresh();

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    //==========================================================================
    // A Viewport exposes its scroll offset so the ruler and row header can stay
    // pinned while the grid scrolls.
    class SeqViewport : public juce::Viewport
    {
    public:
        std::function<void (int scrollX, int scrollY)> onScrolled;

        void visibleAreaChanged (const juce::Rectangle<int>& newVisibleArea) override
        {
            if (onScrolled)
                onScrolled (newVisibleArea.getX(), newVisibleArea.getY());
        }
    };

    //==========================================================================
    // The scrolling grid of steps.
    class GridCanvas : public juce::Component
    {
    public:
        SeqPattern* pattern = nullptr;
        std::function<void()> onEdited;

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;

        /** Step column under an x position, or -1. */
        int stepAt (int x) const;
        /** Row under a y position, or -1. */
        int rowAt (int y) const;
    };

    //==========================================================================
    // Fixed row header: number, sample name, volume (drag), mute.
    class HeaderCanvas : public juce::Component
    {
    public:
        SeqPattern* pattern = nullptr;
        const PadSettings* pads = nullptr;
        std::function<void()> onEdited;
        int scrollY = 0;

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp   (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;

    private:
        int   dragRow      = -1;
        float dragStartVol = 1.0f;
        int   dragStartY   = 0;

        int rowAt (int y) const;
    };

    //==========================================================================
    // Bar / beat ruler above the grid.
    class Ruler : public juce::Component
    {
    public:
        SeqPattern* pattern = nullptr;
        int scrollX = 0;

        void paint (juce::Graphics& g) override;
    };

    //==========================================================================
    void rebuild();
    void notifyEdited();

    SeqPattern*       pattern = nullptr;
    const PadSettings* pads   = nullptr;

    juce::TextButton playButton { "Play" };
    juce::TextButton stopButton { "Stop" };
    juce::TextButton recButton  { "Rec" };
    juce::ComboBox   snapBox;
    juce::ComboBox   barsBox;
    juce::Label      bpmLabel;
    juce::TextButton modeButton { "Live" };
    juce::TextButton lockButton { "Lock" };

    SeqViewport  gridViewport;
    GridCanvas   gridCanvas;
    HeaderCanvas headerCanvas;
    Ruler        ruler;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqControlPanel)
};

//==============================================================================
// Implementation
//==============================================================================
inline SeqControlPanel::SeqControlPanel()
{
    auto styleBtn = [](juce::TextButton& b, juce::Colour on)
    {
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFF3A3A3A));
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFFCECECE));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFF111111));
    };

    styleBtn (playButton, juce::Colour (0xFF00CF7F));
    styleBtn (stopButton, juce::Colour (0xFFCC4444));
    styleBtn (recButton,  juce::Colour (0xFFCC4444));

    // Phase 1: the shell shows the transport but playback arrives in Phase 2.
    playButton.setEnabled (false);
    stopButton.setEnabled (false);
    recButton .setEnabled (false);
    playButton.setTooltip ("Sequencer playback arrives in Phase 2");
    stopButton.setTooltip ("Sequencer playback arrives in Phase 2");
    recButton .setTooltip ("Step recording arrives in Phase 3");

    for (auto* b : { &playButton, &stopButton, &recButton })
        addAndMakeVisible (*b);

    // ---- Snap ----
    snapBox.setTooltip ("Grid resolution");
    snapBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xFF2A2A2A));
    snapBox.setColour (juce::ComboBox::textColourId,       juce::Colour (0xFFCECECE));
    snapBox.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xFF555555));
    snapBox.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xFF888888));
    for (int i = 0; i <= (int) SeqSnap::Free; ++i)
        snapBox.addItem (seqSnapName ((SeqSnap) i), i + 1);
    snapBox.onChange = [this]
    {
        if (pattern == nullptr) return;
        pattern->snap = (SeqSnap) juce::jlimit (0, (int) SeqSnap::Free, snapBox.getSelectedId() - 1);
        notifyEdited();
        resized();          // step count changed
        gridCanvas.repaint();
        ruler.repaint();
    };
    addAndMakeVisible (snapBox);

    // ---- Bars ----
    barsBox.setTooltip ("Pattern length");
    barsBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xFF2A2A2A));
    barsBox.setColour (juce::ComboBox::textColourId,       juce::Colour (0xFFCECECE));
    barsBox.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xFF555555));
    barsBox.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xFF888888));
    barsBox.addItem ("1 bar",  1);
    barsBox.addItem ("2 bars", 2);
    barsBox.addItem ("4 bars", 3);
    barsBox.addItem ("8 bars", 4);
    barsBox.onChange = [this]
    {
        if (pattern == nullptr) return;
        static const int kBarCounts[] = { 1, 2, 4, 8 };
        const int id = juce::jlimit (1, 4, barsBox.getSelectedId());
        pattern->bars = kBarCounts[id - 1];
        notifyEdited();
        resized();
        gridCanvas.repaint();
        ruler.repaint();
    };
    addAndMakeVisible (barsBox);

    // ---- BPM (bank tempo — read-only here; it lives with the metronome) ----
    bpmLabel.setJustificationType (juce::Justification::centredLeft);
    bpmLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    bpmLabel.setColour (juce::Label::textColourId, juce::Colour (0xFFCECECE));
    bpmLabel.setTooltip ("Bank tempo — shared with the metronome");
    addAndMakeVisible (bpmLabel);

    // ---- Mode / Lock ----
    modeButton.setClickingTogglesState (true);
    modeButton.setTooltip ("Live (read-only) / Arrange (editable)");
    styleBtn (modeButton, juce::Colour (0xFF3A6A9A));
    modeButton.onClick = [this]
    {
        if (pattern == nullptr) return;
        pattern->liveMode = modeButton.getToggleState();
        modeButton.setButtonText (pattern->liveMode ? "Live" : "Arrange");
        notifyEdited();
    };
    addAndMakeVisible (modeButton);

    lockButton.setClickingTogglesState (true);
    lockButton.setTooltip ("Lock the pattern so a performance cannot edit it");
    styleBtn (lockButton, juce::Colour (0xFFCC8800));
    lockButton.onClick = [this]
    {
        if (pattern == nullptr) return;
        pattern->locked = lockButton.getToggleState();
        notifyEdited();
    };
    addAndMakeVisible (lockButton);

    // ---- Grid ----
    gridCanvas.pattern = nullptr;
    gridCanvas.onEdited = [this] { notifyEdited(); };

    headerCanvas.onEdited = [this] { notifyEdited(); };

    gridViewport.setViewedComponent (&gridCanvas, false);
    gridViewport.setScrollBarsShown (true, true);
    gridViewport.setScrollOnDragEnabled (false);
    gridViewport.onScrolled = [this] (int x, int y)
    {
        ruler.scrollX = x;
        headerCanvas.scrollY = y;
        ruler.repaint();
        headerCanvas.repaint();
    };
    addAndMakeVisible (gridViewport);

    addAndMakeVisible (headerCanvas);
    addAndMakeVisible (ruler);
}

//==============================================================================
inline void SeqControlPanel::setPattern (SeqPattern* p)
{
    pattern = p;
    gridCanvas.pattern   = p;
    headerCanvas.pattern = p;
    ruler.pattern        = p;
    rebuild();
}

inline void SeqControlPanel::setPadSettings (const PadSettings* p)
{
    pads = p;
    headerCanvas.pads = p;
    headerCanvas.repaint();
}

inline void SeqControlPanel::refresh()
{
    rebuild();
}

inline void SeqControlPanel::rebuild()
{
    if (pattern != nullptr)
    {
        snapBox.setSelectedId ((int) pattern->snap + 1, juce::dontSendNotification);

        static const int kBarCounts[] = { 1, 2, 4, 8 };
        int barsId = 1;
        for (int i = 0; i < 4; ++i)
            if (kBarCounts[i] == pattern->bars) barsId = i + 1;
        barsBox.setSelectedId (barsId, juce::dontSendNotification);

        bpmLabel.setText (juce::String (pattern->bpm, 1) + " BPM", juce::dontSendNotification);

        modeButton.setToggleState (pattern->liveMode, juce::dontSendNotification);
        modeButton.setButtonText (pattern->liveMode ? "Live" : "Arrange");
        lockButton.setToggleState (pattern->locked, juce::dontSendNotification);

        // Live implies locked-looking read-only editing in Phase 1 (editing is
        // still allowed; Phase 3 wires the modes properly).
    }

    resized();
    gridCanvas.repaint();
    headerCanvas.repaint();
    ruler.repaint();
}

inline void SeqControlPanel::notifyEdited()
{
    if (onEdited)
        onEdited();
}

//==============================================================================
inline void SeqControlPanel::resized()
{
    auto area = getLocalBounds();

    auto bar = area.removeFromTop (kToolbarH);
    playButton.setBounds (bar.removeFromLeft (38).reduced (1));
    stopButton.setBounds (bar.removeFromLeft (34).reduced (1));
    recButton .setBounds (bar.removeFromLeft (34).reduced (1));
    bar.removeFromLeft (6);
    snapBox.setBounds (bar.removeFromLeft (78).reduced (1, 0));
    bar.removeFromLeft (4);
    barsBox.setBounds (bar.removeFromLeft (58).reduced (1, 0));
    bar.removeFromLeft (4);
    bpmLabel.setBounds (bar.removeFromLeft (62));
    bar.removeFromLeft (4);
    modeButton.setBounds (bar.removeFromLeft (60).reduced (1));
    bar.removeFromLeft (4);
    lockButton.setBounds (bar.removeFromLeft (44).reduced (1));

    auto rulerRow = area.removeFromTop (kRulerH);
    ruler.setBounds (rulerRow.withTrimmedLeft (kHeaderW));

    headerCanvas.setBounds (area.removeFromLeft (kHeaderW));
    gridViewport.setBounds (area);

    if (pattern != nullptr)
    {
        const int w = juce::jmax (1, pattern->totalSteps()) * kStepW;
        const int h = SeqPattern::kTracks * kRowH;
        gridCanvas.setSize (w, h);
    }
    else
    {
        gridCanvas.setSize (1, 1);
    }
}

inline void SeqControlPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xFF1B1B1B));
}

//==============================================================================
// GridCanvas
//==============================================================================
inline int SeqControlPanel::GridCanvas::stepAt (int x) const
{
    if (pattern == nullptr || x < 0) return -1;
    const int s = x / kStepW;
    return (s >= 0 && s < pattern->totalSteps()) ? s : -1;
}

inline int SeqControlPanel::GridCanvas::rowAt (int y) const
{
    if (y < 0) return -1;
    const int r = y / kRowH;
    return (r >= 0 && r < SeqPattern::kTracks) ? r : -1;
}

inline void SeqControlPanel::GridCanvas::paint (juce::Graphics& g)
{
    if (pattern == nullptr) return;

    const int steps  = pattern->totalSteps();
    const int tps    = juce::jmax (1, pattern->ticksPerStep());
    const int spb    = juce::jmax (1, pattern->stepsPerBar());
    const int width  = steps * kStepW;
    const int height = SeqPattern::kTracks * kRowH;

    g.fillAll (juce::Colour (0xFF1B1B1B));

    // Row backgrounds (alternating) + hit cells.
    for (int r = 0; r < SeqPattern::kTracks; ++r)
    {
        const int y = r * kRowH;
        g.setColour ((r % 2) ? juce::Colour (0xFF202020) : juce::Colour (0xFF252525));
        g.fillRect (0, y, width, kRowH);

        const auto& tr = pattern->tracks[r];
        for (const auto& h : tr.hits)
        {
            if (h.tick < 0) continue;
            const int s = h.tick / tps;
            if (s < 0 || s >= steps) continue;

            const float v = juce::jlimit (0.0f, 1.0f, h.velocity);
            const float bright = 0.35f + 0.65f * v;
            auto col = juce::Colour (0xFF00CF7F).withMultipliedBrightness (bright);
            if (tr.mute) col = col.withSaturation (0.15f);

            g.setColour (col);
            g.fillRoundedRectangle (juce::Rectangle<float> (
                (float) (s * kStepW + 1), (float) (y + 1),
                (float) (kStepW - 2), (float) (kRowH - 2)), 2.0f);
        }
    }

    // Vertical lines: bar (bright), beat (medium).
    for (int s = 0; s <= steps; ++s)
    {
        const int x = s * kStepW;
        const bool isBar  = (s % spb) == 0;
        const bool isBeat = (s % juce::jmax (1, spb / 4)) == 0;

        if (isBar)       g.setColour (juce::Colour (0xFF5A5A5A));
        else if (isBeat) g.setColour (juce::Colour (0xFF3E3E3E));
        else             g.setColour (juce::Colour (0xFF2E2E2E));

        g.drawVerticalLine (x, 0.0f, (float) height);
    }

    // Horizontal row separators.
    g.setColour (juce::Colour (0xFF2E2E2E));
    for (int r = 1; r < SeqPattern::kTracks; ++r)
        g.drawHorizontalLine (r * kRowH, 0.0f, (float) width);
}

inline void SeqControlPanel::GridCanvas::mouseDown (const juce::MouseEvent& e)
{
    if (pattern == nullptr) return;

    const int row  = rowAt (e.y);
    const int step = stepAt (e.x);
    if (row < 0 || step < 0) return;

    const int tps   = juce::jmax (1, pattern->ticksPerStep());
    const int tick0 = step * tps;

    auto& hits = pattern->tracks[row].hits;
    auto it = std::find_if (hits.begin(), hits.end(), [tick0, tps] (const SeqHit& h)
                            { return h.tick >= tick0 && h.tick < tick0 + tps; });

    if (e.mods.isPopupMenu() || it != hits.end())
    {
        if (it != hits.end())
            hits.erase (it);
    }
    else
    {
        hits.push_back ({ tick0, 0.8f });
        std::sort (hits.begin(), hits.end(),
                   [] (const SeqHit& a, const SeqHit& b) { return a.tick < b.tick; });
    }

    repaint();
    if (onEdited) onEdited();
}

//==============================================================================
// HeaderCanvas
//==============================================================================
inline int SeqControlPanel::HeaderCanvas::rowAt (int y) const
{
    const int r = (y + scrollY) / kRowH;
    return (r >= 0 && r < SeqPattern::kTracks) ? r : -1;
}

inline void SeqControlPanel::HeaderCanvas::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xFF161616));
    if (pattern == nullptr) return;

    const int w = getWidth();
    const auto nameFont = juce::Font (juce::FontOptions (9.5f));
    const auto numFont  = juce::Font (juce::FontOptions (9.5f, juce::Font::bold));

    for (int r = 0; r < SeqPattern::kTracks; ++r)
    {
        const int y = r * kRowH - scrollY;
        if (y + kRowH < 0 || y > getHeight()) continue;

        const auto& tr = pattern->tracks[r];
        const bool selectedRow = false;

        g.setColour ((r % 2) ? juce::Colour (0xFF1C1C1C) : juce::Colour (0xFF212121));
        g.fillRect (0, y, w, kRowH);

        // Number
        g.setColour (juce::Colour (0xFF9A9A9A));
        g.setFont (numFont);
        g.drawText (juce::String (r + 1),
                    juce::Rectangle<int> (0, y, kNumW, kRowH),
                    juce::Justification::centred, false);

        // Sample name — captured if present, otherwise the live pad (dimmed).
        juce::String name = tr.sound.name();
        bool captured = tr.sound.hasSound;
        if (name.isEmpty() && pads != nullptr)
            name = pads[r].sampleFilePath.isEmpty()
                       ? juce::String{}
                       : juce::File (pads[r].sampleFilePath).getFileName();

        const int nameW = w - kNumW - kVolW - kMuteW;
        g.setColour (captured ? juce::Colour (0xFFCFCFCF) : juce::Colour (0xFF6E6E6E));
        g.setFont (nameFont);
        g.drawText (name.isEmpty() ? juce::String ("-") : name,
                    juce::Rectangle<int> (kNumW, y, nameW - 2, kRowH),
                    juce::Justification::centredLeft, true);

        // Volume (drag to adjust)
        const int volX = w - kVolW - kMuteW;
        g.setColour (juce::Colour (0xFF2E2E2E));
        g.fillRect (volX + 1, y + 2, kVolW - 4, kRowH - 4);
        g.setColour (tr.volume >= 0.999f ? juce::Colour (0xFF8A8A8A) : juce::Colour (0xFF00CF7F));
        g.setFont (nameFont);
        g.drawText (juce::String (juce::roundToInt (tr.volume * 100.0f)) + "%",
                    juce::Rectangle<int> (volX, y, kVolW, kRowH),
                    juce::Justification::centred, false);

        // Mute
        const int muteX = w - kMuteW;
        g.setColour (tr.mute ? juce::Colour (0xFFCC4444) : juce::Colour (0xFF303030));
        g.fillRect (muteX + 1, y + 2, kMuteW - 3, kRowH - 4);
        g.setColour (juce::Colour (0xFFDDDDDD));
        g.setFont (nameFont);
        g.drawText ("M", juce::Rectangle<int> (muteX, y, kMuteW, kRowH),
                    juce::Justification::centred, false);

        g.setColour (juce::Colour (0xFF2E2E2E));
        g.drawHorizontalLine (y + kRowH - 1, 0.0f, (float) w);
    }
}

inline void SeqControlPanel::HeaderCanvas::mouseDown (const juce::MouseEvent& e)
{
    if (pattern == nullptr) return;

    const int row = rowAt (e.y);
    if (row < 0) return;

    const int w = getWidth();
    const int volX  = w - kVolW - kMuteW;
    const int muteX = w - kMuteW;

    if (e.x >= muteX)
    {
        pattern->tracks[row].mute = ! pattern->tracks[row].mute;
        repaint();
        if (onEdited) onEdited();
        return;
    }

    if (e.x >= volX)
    {
        dragRow      = row;
        dragStartVol = pattern->tracks[row].volume;
        dragStartY   = e.getPosition().y;
    }
}

inline void SeqControlPanel::HeaderCanvas::mouseDrag (const juce::MouseEvent& e)
{
    if (pattern == nullptr || dragRow < 0) return;

    // Vertical drag: up = louder.  200 px of travel spans the full range.
    const float delta = (float) (dragStartY - e.getPosition().y) / 200.0f;
    pattern->tracks[dragRow].volume = juce::jlimit (0.0f, 1.0f, dragStartVol + delta);
    repaint();
    if (onEdited) onEdited();
}

inline void SeqControlPanel::HeaderCanvas::mouseUp (const juce::MouseEvent&)
{
    dragRow = -1;
}

inline void SeqControlPanel::HeaderCanvas::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (pattern == nullptr) return;

    const int row = rowAt (e.y);
    if (row < 0) return;

    const int volX = getWidth() - kVolW - kMuteW;
    if (e.x >= volX)
    {
        pattern->tracks[row].volume = 1.0f;
        repaint();
        if (onEdited) onEdited();
    }
}

//==============================================================================
// Ruler
//==============================================================================
inline void SeqControlPanel::Ruler::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xFF141414));
    if (pattern == nullptr) return;

    const int steps = pattern->totalSteps();
    const int spb   = juce::jmax (1, pattern->stepsPerBar());
    const int spBeat = juce::jmax (1, spb / 4);
    const auto font = juce::Font (juce::FontOptions (9.0f, juce::Font::bold));

    g.setFont (font);

    for (int s = 0; s < steps; ++s)
    {
        const int x = s * kStepW - scrollX;
        if (x + kStepW < 0 || x > getWidth()) continue;

        if (s % spb == 0)
        {
            g.setColour (juce::Colour (0xFF8A8A8A));
            g.drawText (juce::String (s / spb + 1),
                        juce::Rectangle<int> (x + 2, 0, kStepW * 2, kRulerH),
                        juce::Justification::centredLeft, false);
        }
        else if (s % spBeat == 0)
        {
            g.setColour (juce::Colour (0xFF4A4A4A));
            g.fillRect (x, kRulerH - 4, 1, 4);
        }
    }

    g.setColour (juce::Colour (0xFF333333));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}
