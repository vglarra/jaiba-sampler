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
class SeqControlPanel : public juce::Component,
                        private juce::Timer
{
public:
    static constexpr int kRowHDefault = 13;   // used until the first layout pass
    // Rows have no upper bound: they grow to fill a maximised window.  The floor
    // only matters when the window is too short for 16 tracks, which then scrolls.
    static constexpr int kMinRowH     = 12;
    static constexpr int kRulerH   = 12;
    static constexpr int kHeaderW  = 150;
    static constexpr int kStepW    = 20;
    static constexpr int kToolbarH = 20;   // per row; there are two

    // Row-header column split (within kHeaderW).
    static constexpr int kNumW   = 22;
    static constexpr int kVolW   = 36;
    static constexpr int kMuteW  = 18;

    //==========================================================================
    // Fired whenever the pattern is edited (MainComponent marks the kit dirty).
    std::function<void()> onEdited;

    // Metronome — mirrors the Rec tab's controls.  BPM is supplied by the owner
    // (the bank/global tempo), so this panel never invents one.
    std::function<void (bool)>  onMetronomeToggled;
    std::function<void (float)> onMetronomeVolumeChanged;
    std::function<void (double)> onTempoChanged;   // tap tempo

    // Tempo source ("song") for the current bank — D4/D9.
    struct TempoInfo
    {
        juce::StringArray names;      // one per tempo source, in index order
        int  currentGroup = 0;        // -1 = Kit, else a tempoSources index
        bool locked       = false;    // true while a take is armed
    };
    std::function<void (int)> onTempoSourceChosen;   // -1 = Kit, >= 0 = source index
    std::function<void ()>    onNewSongRequested;
    std::function<void ()>    onRenameSongRequested;
    std::function<void ()>    onDeleteSongRequested;

    // Transport — the pool lives in MainComponent; the panel just drives it.
    std::function<void ()>     onPlayRequested;
    std::function<void ()>     onStopRequested;
    std::function<double ()>   getPlayheadTicks;   // ticks, for the playhead column

    // Session transport/mix options.
    std::function<void (bool)>  onSyncClickToggled;   // lock the click to the sequencer
    std::function<void (float)> onSeqVolumeChanged;   // master level for the pool
    std::function<void (int, int)> onTimeSigChanged;  // accent grouping (num, den)

    SeqControlPanel();
    ~SeqControlPanel() override = default;

    /** Pattern to view/edit. May be null (nothing shown). */
    void setPattern (SeqPattern* p);
    /** The 16 live pads, used only as a dim fallback label before a track is captured. */
    void setPadSettings (const PadSettings* p);
    /** Re-read the pattern after an external change (bank switch / kit load). */
    void refresh();

    /** Reflect the global metronome state (kept in sync with the Rec tab). */
    void setMetronomeState (bool on, float volume);
    /** Volume only — leaves the Metro toggle as it is. */
    void setMetronomeVolume (float volume);
    /** Tempo display — the live tempo, shared with the Rec tab. */
    void setMetronomeBpm (double bpm);
    /** Which tempo source this bank follows, and the names to offer. */
    void setTempoInfo (const TempoInfo& info);
    /** Transport state from the pool: enables/disables Play/Stop and the playhead. */
    void setTransportState (bool nowPlaying);
    /** Session options: click sync + the sequencer's master level. */
    void setSeqOptions (bool syncClick, float seqVolume);
    /** Re-read mode + lock from the pattern and apply them to the controls. */
    void refreshMode();
    /** Repaint just the grid, for live capture writing hits as you play. */
    void repaintGrid()
    {
        gridCanvas.repaint();
        headerCanvas.repaint();
        ruler.repaint();
    }
    /** Metronome accent grouping. */
    void setTimeSig (int numerator, int denominator);

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
    class GridCanvas : public juce::Component,
                       private juce::Timer
    {
    public:
        SeqPattern* pattern = nullptr;
        int rowH = kRowHDefault;      // set by SeqControlPanel::resized()
        bool editable = true;         // Arrange mode and unlocked
        std::function<void()> onEdited;

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;

        /** Step column under an x position, or -1. */
        int stepAt (int x) const;
        /** Row under a y position, or -1. */
        int rowAt (int y) const;

        /** Playhead in ticks (-1 = hidden).  Repaints only the affected columns. */
        void setPlayheadTicks (double ticks);

        /** Columns to draw: the loop, plus any dimmed columns holding hits that
            were kept when the signature shortened the bar. */
        int visibleSteps() const
        {
            if (pattern == nullptr) return 0;

            return juce::jmax (pattern->totalSteps(),
                               juce::jmin (pattern->displaySteps(),
                                           pattern->totalSteps() + kMaxGhostSteps));
        }

    private:
        int playheadColumn = -1;

        // Velocity drag state.
        int   pressRow    = -1;
        int   pressTick   = -1;
        float pressVel    = SequencerVelocity::kDefault;
        int   pressY      = 0;
        bool  velDragging = false;

        // Click-to-erase is deferred by the double-click window, because JUCE
        // delivers mouseDoubleClick *after* mouseUp: erasing on mouseUp would
        // destroy the hit before a double-click could reset its velocity.
        int pendingRemoveRow  = -1;
        int pendingRemoveTick = -1;

        void timerCallback() override;
        std::vector<SeqHit>::iterator findHit (int row, int tick);
    };

    //==========================================================================
    // Fixed row header: number, sample name, volume (drag), mute.
    class HeaderCanvas : public juce::Component,
                         public juce::SettableTooltipClient
    {
    public:
        SeqPattern* pattern = nullptr;
        const PadSettings* pads = nullptr;
        int rowH = kRowHDefault;      // kept in step with GridCanvas
        std::function<void()> onEdited;
        int scrollY = 0;

        void paint (juce::Graphics& g) override;
        void mouseMove (const juce::MouseEvent& e) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp   (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;

    private:
        int   dragRow      = -1;
        float dragStartVol = 1.0f;
        int   dragStartY   = 0;
        bool  dragging     = false;

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
    /** The BPM readout.  Drag up/down to set the tempo (shift = fine), or
        double-click to snap back to 120 — no tapping required. */
    class TempoLabel : public juce::Label
    {
    public:
        std::function<void (double)> onTempoDragged;
        std::function<void ()>       onTempoReset;

        void setBpm (double b)
        {
            bpm = b;
            setText (juce::String (b, 1) + " BPM", juce::dontSendNotification);
        }

        void mouseMove (const juce::MouseEvent&) override
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragStartBpm = bpm;
            dragStartY   = e.getPosition().y;
            dragging     = false;
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const int dy = dragStartY - e.getPosition().y;

            // Ignore the first pixel or two so a double-click cannot nudge the tempo.
            if (! dragging && std::abs (dy) < 3) return;
            dragging = true;

            const double next = SequencerTempo::bpmFromDrag (dragStartBpm, dy,
                                                             e.mods.isShiftDown());

            if (onTempoDragged) onTempoDragged (next);
        }

        void mouseUp (const juce::MouseEvent&) override { dragging = false; }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            if (onTempoReset) onTempoReset();
        }

        /** The value the double-click reset lands on. */
        static constexpr double resetBpm()
        {
            return SequencerTempo::kResetBpm;
        }

    private:
        double bpm          = 120.0;
        double dragStartBpm = 120.0;
        int    dragStartY   = 0;
        bool   dragging     = false;
    };

    TempoLabel       bpmLabel;
    juce::ComboBox   tempoCombo;              // tempo source ("song") for this bank
    juce::TextButton songMenuButton { "..." }; // rename / delete the current song
    juce::TextButton modeButton { "Live" };
    juce::TextButton lockButton { "Lock" };

    // Session options: click phase sync + the sequencer's master level.
    juce::TextButton syncButton { "Sync" };
    juce::Label      seqVolumeLabel;
    juce::Slider     seqVolumeSlider;

    // Metronome accent grouping.
    juce::ComboBox   sigBox { "Sig" };

    /** The time signatures offered, in menu order. */
    static juce::StringArray timeSigChoices()
    {
        return { "2/4", "3/4", "4/4", "5/4", "6/8", "7/8", "9/8", "12/8" };
    }

    static constexpr int kNewSongId = 1000;

    /** Cap on dimmed overhang columns, so a stray far-off hit cannot make the
        canvas absurdly wide. */
    static constexpr int kMaxGhostSteps = 64;

    // Metronome — same controls as the Rec tab.
    juce::TextButton metronomeButton { "Metro" };
    juce::Slider     metronomeVolumeSlider;
    juce::TextButton tapButton { "Tap" };
    TapTempoState    tap;

    void handleTap();

    SeqViewport  gridViewport;
    GridCanvas   gridCanvas;
    HeaderCanvas headerCanvas;
    Ruler        ruler;

    int rowH = kRowHDefault;   // computed in resized() from the space available

    void timerCallback() override;

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

    // Phase 1 built the shell; playback is live now, recording is Phase 3.
    stopButton.setEnabled (false);   // enabled while playing
    playButton.setTooltip ("Play the pattern");
    stopButton.setTooltip ("Stop the pattern");

    // Rec arms Record mode: playing pads then writes steps into the grid.
    recButton.setClickingTogglesState (true);
    recButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF9E2B2B));
    recButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    recButton.setTooltip ("Arm Record - pads you play are written into the grid");
    recButton.onClick = [this]
    {
        if (pattern == nullptr) return;

        pattern->mode = recButton.getToggleState() ? SeqMode::Record : SeqMode::Arrange;
        refreshMode();
        notifyEdited();
    };

    playButton.onClick   = [this] { if (onPlayRequested) onPlayRequested(); };
    stopButton.onClick   = [this] { if (onStopRequested) onStopRequested(); };

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
    bpmLabel.setTooltip ("Drag up/down to set the tempo (shift = fine), "
                         "double-click for 120");
    bpmLabel.onTempoDragged = [this] (double bpm) { if (onTempoChanged) onTempoChanged (bpm); };
    bpmLabel.onTempoReset   = [this]
    {
        if (onTempoChanged) onTempoChanged (TempoLabel::resetBpm());
    };
    addAndMakeVisible (bpmLabel);

    // ---- Tempo source ("song") for this bank ----
    tempoCombo.setTooltip ("Which tempo this bank follows: a song, or its own kit tempo");
    tempoCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xFF2A2A2A));
    tempoCombo.setColour (juce::ComboBox::textColourId,       juce::Colour (0xFFCECECE));
    tempoCombo.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xFF555555));
    tempoCombo.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xFF888888));
    tempoCombo.onChange = [this]
    {
        const int id = tempoCombo.getSelectedId();

        if (id == kNewSongId)
        {
            if (onNewSongRequested) onNewSongRequested();
            return;
        }

        const int group = (id == 1) ? TempoSourceDefaults::kKitTempo : (id - 2);
        if (onTempoSourceChosen) onTempoSourceChosen (group);
    };
    addAndMakeVisible (tempoCombo);

    songMenuButton.setTooltip ("Rename or delete this song");
    songMenuButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xFF3A3A3A));
    songMenuButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFCECECE));
    songMenuButton.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Rename song...");
        m.addItem (2, "Delete song...");

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&songMenuButton),
            [this] (int r)
            {
                if (r == 1)      { if (onRenameSongRequested) onRenameSongRequested(); }
                else if (r == 2) { if (onDeleteSongRequested) onDeleteSongRequested(); }
            });
    };
    addAndMakeVisible (songMenuButton);

    // ---- Mode / Lock ----
    modeButton.setClickingTogglesState (true);
    modeButton.setTooltip ("Live (read-only) / Arrange (editable)");
    styleBtn (modeButton, juce::Colour (0xFF3A6A9A));
    modeButton.setClickingTogglesState (false);
    modeButton.setTooltip ("Live (plays, grid read-only) -> Record (capture) -> "
                           "Arrange (edit)");
    modeButton.onClick = [this]
    {
        if (pattern == nullptr) return;

        pattern->mode = (SeqMode) (((int) pattern->mode + 1) % 3);
        refreshMode();
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
        refreshMode();
        notifyEdited();
    };
    addAndMakeVisible (lockButton);

    // ---- Metronome (copied from the Rec tab, two-way synced) ----
    metronomeButton.setButtonText ("Metro");
    metronomeButton.setClickingTogglesState (true);
    metronomeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFF4A4A4A));
    metronomeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF00CF7F));
    metronomeButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFFCECECE));
    metronomeButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFF111111));
    metronomeButton.setTooltip ("Standalone metronome (same control as the Rec tab)");
    metronomeButton.onClick = [this]
    {
        if (onMetronomeToggled)
            onMetronomeToggled (metronomeButton.getToggleState());
    };
    addAndMakeVisible (metronomeButton);

    metronomeVolumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    metronomeVolumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    metronomeVolumeSlider.setRange (0.0, 1.0, 0.01);
    metronomeVolumeSlider.setValue (0.5, juce::dontSendNotification);
    metronomeVolumeSlider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xFF3A3A3A));
    metronomeVolumeSlider.setColour (juce::Slider::trackColourId,      juce::Colour (0xFF00CF7F));
    metronomeVolumeSlider.setColour (juce::Slider::thumbColourId,      juce::Colour (0xFFCECECE));
    metronomeVolumeSlider.setTooltip ("Metronome volume");
    metronomeVolumeSlider.onValueChange = [this]
    {
        if (onMetronomeVolumeChanged)
            onMetronomeVolumeChanged ((float) metronomeVolumeSlider.getValue());
    };
    addAndMakeVisible (metronomeVolumeSlider);

    // ---- Click sync: lock the metronome's phase to the sequencer ----
    syncButton.setClickingTogglesState (true);
    syncButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFF4A4A4A));
    syncButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF3A7ACC));
    syncButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFFCECECE));
    syncButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFF111111));
    syncButton.setTooltip ("Lock the metronome's downbeat to the sequencer's "
                           "(off = the click free-runs)");
    syncButton.onClick = [this]
    {
        if (onSyncClickToggled)
            onSyncClickToggled (syncButton.getToggleState());
    };
    addAndMakeVisible (syncButton);

    // ---- Sequencer master level (boxed label, same look as the song box) ----
    seqVolumeLabel.setText ("Seq Vol", juce::dontSendNotification);
    seqVolumeLabel.setJustificationType (juce::Justification::centred);
    seqVolumeLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    seqVolumeLabel.setColour (juce::Label::backgroundColourId,  juce::Colour (0xFF2A2A2A));
    seqVolumeLabel.setColour (juce::Label::outlineColourId,     juce::Colour (0xFF555555));
    seqVolumeLabel.setColour (juce::Label::textColourId,        juce::Colour (0xFFCECECE));
    seqVolumeLabel.setTooltip ("Sequencer master level (balances Seq against the pads)");
    addAndMakeVisible (seqVolumeLabel);

    seqVolumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    seqVolumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    seqVolumeSlider.setRange (0.0, 1.0, 0.01);
    seqVolumeSlider.setValue (1.0, juce::dontSendNotification);
    seqVolumeSlider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xFF3A3A3A));
    seqVolumeSlider.setColour (juce::Slider::trackColourId,      juce::Colour (0xFFB87800));
    seqVolumeSlider.setColour (juce::Slider::thumbColourId,      juce::Colour (0xFFCECECE));
    seqVolumeSlider.setTooltip ("Sequencer master level (balances Seq against the pads)");
    seqVolumeSlider.onValueChange = [this]
    {
        if (onSeqVolumeChanged)
            onSeqVolumeChanged ((float) seqVolumeSlider.getValue());
    };
    addAndMakeVisible (seqVolumeSlider);

    // ---- Metronome accent grouping ----
    sigBox.setTooltip ("Time signature - which click is accented as the bar's first beat");
    sigBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xFF2A2A2A));
    sigBox.setColour (juce::ComboBox::textColourId,       juce::Colour (0xFFCECECE));
    sigBox.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xFF555555));
    sigBox.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xFF888888));
    {
        const auto choices = timeSigChoices();
        for (int i = 0; i < choices.size(); ++i)
            sigBox.addItem (choices[i], i + 1);
    }
    sigBox.setSelectedId (3, juce::dontSendNotification);   // 4/4
    sigBox.onChange = [this]
    {
        const auto choices = timeSigChoices();
        const int idx = sigBox.getSelectedId() - 1;
        if (idx < 0 || idx >= choices.size()) return;

        const juce::String t = choices[idx];
        if (onTimeSigChanged)
            onTimeSigChanged (t.upToFirstOccurrenceOf ("/", false, false).getIntValue(),
                              t.fromFirstOccurrenceOf ("/", false, false).getIntValue());
    };
    addAndMakeVisible (sigBox);

    // ---- Tap tempo (same accumulator as the Rec tab) ----
    tapButton.setButtonText ("Tap");
    tapButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF4A3A00));
    tapButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFCECECE));
    tapButton.setTooltip ("Tap tempo — shared with the Rec tab");
    tapButton.onClick = [this] { handleTap(); };
    addAndMakeVisible (tapButton);

    // ---- Grid ----
    gridCanvas.pattern = nullptr;
    gridCanvas.onEdited = [this] { notifyEdited(); };

    headerCanvas.onEdited = [this] { notifyEdited(); };
    headerCanvas.setTooltip ("Drag the % up/down to set the track level, "
                             "double-click to reset it to 100%");

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

inline void SeqControlPanel::setMetronomeState (bool on, float volume)
{
    metronomeButton.setToggleState (on, juce::dontSendNotification);
    metronomeVolumeSlider.setValue (juce::jlimit (0.0, 1.0, (double) volume),
                                    juce::dontSendNotification);
}

inline void SeqControlPanel::setMetronomeVolume (float volume)
{
    metronomeVolumeSlider.setValue (juce::jlimit (0.0, 1.0, (double) volume),
                                    juce::dontSendNotification);
}

inline void SeqControlPanel::setMetronomeBpm (double bpm)
{
    bpmLabel.setBpm (bpm);
}

inline void SeqControlPanel::setTempoInfo (const TempoInfo& info)
{
    tempoCombo.clear (juce::dontSendNotification);
    tempoCombo.addItem ("Kit", 1);
    for (int i = 0; i < info.names.size(); ++i)
        tempoCombo.addItem (info.names[i], i + 2);
    tempoCombo.addSeparator();
    tempoCombo.addItem ("+ New song...", kNewSongId);

    const int id = (info.currentGroup == TempoSourceDefaults::kKitTempo)
                       ? 1
                       : juce::jlimit (2, info.names.size() + 1, info.currentGroup + 2);
    tempoCombo.setSelectedId (id, juce::dontSendNotification);

    tempoCombo.setEnabled (! info.locked);
    songMenuButton.setEnabled (! info.locked);
}

// Mode + Lock together decide whether the grid can be edited; the per-track
// faders stay live in every mode (D8), so only the pattern controls are gated.
inline void SeqControlPanel::refreshMode()
{
    if (pattern == nullptr) return;

    const bool editable = pattern->isEditable();

    modeButton.setButtonText (seqModeName (pattern->mode));

    juce::Colour modeCol (0xFF2A5A8A);   // Live
    if (pattern->locked)                       modeCol = juce::Colour (0xFF6A5000);
    else if (pattern->mode == SeqMode::Record) modeCol = juce::Colour (0xFF9E2B2B);
    else if (pattern->mode == SeqMode::Arrange) modeCol = juce::Colour (0xFF2F6B4F);

    modeButton.setColour (juce::TextButton::buttonColourId,  modeCol);
    modeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);

    lockButton.setToggleState (pattern->locked, juce::dontSendNotification);
    recButton .setToggleState (pattern->mode == SeqMode::Record, juce::dontSendNotification);

    snapBox.setEnabled (editable);
    barsBox.setEnabled (editable);
    sigBox .setEnabled (editable);

    gridCanvas.editable = editable;
    gridCanvas.repaint();

    modeButton.setTooltip (juce::String (seqModeName (pattern->mode))
                           + (pattern->locked ? " (locked)" : "")
                           + " - click to cycle: Live / Record / Arrange");
}

inline void SeqControlPanel::setTransportState (bool nowPlaying)
{
    playButton.setEnabled (! nowPlaying);
    stopButton.setEnabled (nowPlaying);

    if (nowPlaying)
    {
        startTimerHz (30);   // playhead repaint
    }
    else
    {
        stopTimer();
        gridCanvas.setPlayheadTicks (-1.0);
    }
}

inline void SeqControlPanel::setSeqOptions (bool syncClick, float seqVolume)
{
    syncButton.setToggleState (syncClick, juce::dontSendNotification);
    seqVolumeSlider.setValue (juce::jlimit (0.0, 1.0, (double) seqVolume),
                              juce::dontSendNotification);
}

inline void SeqControlPanel::timerCallback()
{
    if (getPlayheadTicks)
        gridCanvas.setPlayheadTicks (getPlayheadTicks());
}

inline void SeqControlPanel::GridCanvas::setPlayheadTicks (double ticks)
{
    const int col = (pattern != nullptr && ticks >= 0.0)
                        ? (int) (ticks / (double) juce::jmax (1, pattern->ticksPerStep()))
                        : -1;

    if (col == playheadColumn) return;

    // Repaint only the columns that change, not the whole grid.
    const int h = getHeight();
    if (playheadColumn >= 0) repaint (playheadColumn * kStepW, 0, kStepW, h);
    playheadColumn = col;
    if (playheadColumn >= 0) repaint (playheadColumn * kStepW, 0, kStepW, h);
}

inline void SeqControlPanel::setTimeSig (int numerator, int denominator)
{
    if (numerator <= 0 || denominator <= 0)
    {
        sigBox.setSelectedId (0, juce::dontSendNotification);
        return;
    }

    const juce::String want = juce::String (numerator) + "/" + juce::String (denominator);
    const auto choices = timeSigChoices();
    const int idx = choices.indexOf (want);

    // A signature outside the list (hand-edited session) still shows as nothing
    // selected, but the accent still follows it.
    sigBox.setSelectedId (idx >= 0 ? idx + 1 : 0, juce::dontSendNotification);
}

inline void SeqControlPanel::handleTap()
{
    tap.addTap (juce::Time::currentTimeMillis());

    // Flash the button orange, like the Rec tab's.
    tapButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFFB87800));
    juce::Timer::callAfterDelay (120, [sp = juce::Component::SafePointer<SeqControlPanel>(this)]
    {
        if (sp != nullptr)
            sp->tapButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF4A3A00));
    });

    const double bpm = tap.getBpm();
    if (bpm > 0.0)
    {
        const double clamped = juce::jlimit (60.0, 360.0, bpm);
        setMetronomeBpm (clamped);          // immediate feedback
        if (onTempoChanged)
            onTempoChanged (clamped);       // MainComponent applies + echoes back
    }
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

        // The BPM label is driven by setMetronomeBpm (the live/shared tempo), not
        // by pattern->bpm, so the Rec and Seq tabs can never disagree.

        refreshMode();
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

    // ---- Row 1: transport + levels ----
    {
        auto bar = area.removeFromTop (kToolbarH);
        playButton.setBounds (bar.removeFromLeft (38).reduced (1));
        stopButton.setBounds (bar.removeFromLeft (34).reduced (1));
        recButton .setBounds (bar.removeFromLeft (34).reduced (1));
        bar.removeFromLeft (8);
        metronomeButton.setBounds (bar.removeFromLeft (46).reduced (1));
        bar.removeFromLeft (2);
        metronomeVolumeSlider.setBounds (bar.removeFromLeft (60).reduced (0, 3));
        bar.removeFromLeft (8);
        syncButton.setBounds (bar.removeFromLeft (40).reduced (1));
        bar.removeFromLeft (8);
        seqVolumeLabel.setBounds (bar.removeFromLeft (52).reduced (0, 3));
        bar.removeFromLeft (4);
        seqVolumeSlider.setBounds (bar.removeFromLeft (90).reduced (0, 3));
        bar.removeFromLeft (8);
        sigBox.setBounds (bar.removeFromLeft (66).reduced (1, 0));
    }

    // ---- Row 2: pattern + song ----
    {
        auto bar = area.removeFromTop (kToolbarH);
        snapBox.setBounds (bar.removeFromLeft (76).reduced (1, 0));
        bar.removeFromLeft (4);
        barsBox.setBounds (bar.removeFromLeft (56).reduced (1, 0));
        bar.removeFromLeft (4);
        tapButton.setBounds (bar.removeFromLeft (34).reduced (1));
        bar.removeFromLeft (3);
        bpmLabel.setBounds (bar.removeFromLeft (60));
        bar.removeFromLeft (4);
        tempoCombo.setBounds (bar.removeFromLeft (96).reduced (1, 0));
        bar.removeFromLeft (3);
        songMenuButton.setBounds (bar.removeFromLeft (22).reduced (1));
        bar.removeFromLeft (8);
        modeButton.setBounds (bar.removeFromLeft (58).reduced (1));
        bar.removeFromLeft (4);
        lockButton.setBounds (bar.removeFromLeft (40).reduced (1));
    }

    auto rulerRow = area.removeFromTop (kRulerH);
    ruler.setBounds (rulerRow.withTrimmedLeft (kHeaderW));

    headerCanvas.setBounds (area.removeFromLeft (kHeaderW));
    gridViewport.setBounds (area);

    if (pattern == nullptr)
    {
        gridCanvas.setSize (1, 1);
        return;
    }

    // Fit all 16 tracks into the height available: no vertical scrolling, and the
    // rows grow when the window does.  Reserve the horizontal scrollbar's height
    // when the pattern is wider than the viewport, so its arrival cannot push a
    // vertical scrollbar in as well.
    const int contentW = juce::jmax (1, gridCanvas.visibleSteps()) * kStepW;
    const int sbH      = gridViewport.getScrollBarThickness();
    const int availH   = juce::jmax (1, area.getHeight())
                       - SequencerLayout::scrollbarReserve (contentW, area.getWidth(), sbH);

    rowH = SequencerLayout::rowHeightFor (availH, SeqPattern::kTracks, kMinRowH);

    gridCanvas.rowH   = rowH;
    headerCanvas.rowH = rowH;

    gridCanvas.setSize (contentW, SeqPattern::kTracks * rowH);
    headerCanvas.repaint();
    gridCanvas.repaint();
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
    const int r = y / juce::jmax (1, rowH);
    return (r >= 0 && r < SeqPattern::kTracks) ? r : -1;
}

inline void SeqControlPanel::GridCanvas::paint (juce::Graphics& g)
{
    if (pattern == nullptr) return;

    const int steps  = pattern->totalSteps();
    const int shown  = juce::jmax (steps, visibleSteps());
    const int tps    = juce::jmax (1, pattern->ticksPerStep());
    const int spb    = juce::jmax (1, pattern->stepsPerBar());
    const int width  = shown * kStepW;
    const int loopW  = steps * kStepW;
    const int height = SeqPattern::kTracks * rowH;

    g.fillAll (juce::Colour (0xFF1B1B1B));

    // Row backgrounds (alternating) + hit cells.
    for (int r = 0; r < SeqPattern::kTracks; ++r)
    {
        const int y = r * rowH;
        g.setColour ((r % 2) ? juce::Colour (0xFF202020) : juce::Colour (0xFF252525));
        g.fillRect (0, y, loopW, rowH);

        // Dimmed overhang: hits kept here are stored but outside the loop.
        if (shown > steps)
        {
            g.setColour (juce::Colour (0xFF141414));
            g.fillRect (loopW, y, width - loopW, rowH);
        }

        const auto& tr = pattern->tracks[r];
        for (const auto& h : tr.hits)
        {
            if (h.tick < 0) continue;
            const int s = h.tick / tps;
            if (s < 0 || s >= shown) continue;

            const float v = juce::jlimit (0.0f, 1.0f, h.velocity);
            const float bright = 0.35f + 0.65f * v;
            auto col = juce::Colour (0xFF00CF7F).withMultipliedBrightness (bright);
            if (tr.mute) col = col.withSaturation (0.15f);

            const auto cell = juce::Rectangle<float> (
                (float) (s * kStepW + 1), (float) (y + 1),
                (float) (kStepW - 2), (float) (rowH - 2));

            if (s < steps)
            {
                g.setColour (col);
                g.fillRoundedRectangle (cell, 2.0f);
            }
            else
            {
                // Outside the loop: hollow and dim, so it cannot be mistaken for a
                // note that is playing.  Switching the signature back revives it.
                g.setColour (col.withAlpha (0.28f));
                g.fillRoundedRectangle (cell, 2.0f);
                g.setColour (col.withAlpha (0.70f));
                g.drawRoundedRectangle (cell, 2.0f, 1.0f);
            }
        }
    }

    // Vertical lines: bar (bright), beat (medium).
    for (int s = 0; s <= shown; ++s)
    {
        const int x = s * kStepW;
        const bool isBar  = (s % spb) == 0;
        const bool isBeat = (s % juce::jmax (1, spb / 4)) == 0;

        if (s > steps)        g.setColour (juce::Colour (0xFF232323));
        else if (isBar)       g.setColour (juce::Colour (0xFF5A5A5A));
        else if (isBeat)      g.setColour (juce::Colour (0xFF3E3E3E));
        else                  g.setColour (juce::Colour (0xFF2E2E2E));

        g.drawVerticalLine (x, 0.0f, (float) height);
    }

    // The loop end — the boundary those dimmed notes sit beyond.
    if (shown > steps)
    {
        g.setColour (juce::Colour (0xFFB87800));
        g.fillRect (loopW - 1, 0, 2, height);
    }

    // Horizontal row separators.
    g.setColour (juce::Colour (0xFF2E2E2E));
    for (int r = 1; r < SeqPattern::kTracks; ++r)
        g.drawHorizontalLine (r * rowH, 0.0f, (float) width);

    // Playhead column (drawn last so it reads over the cells).
    if (playheadColumn >= 0 && playheadColumn < steps)
    {
        const int x = playheadColumn * kStepW;
        g.setColour (juce::Colour (0x33FFFFFF));
        g.fillRect (x, 0, kStepW, height);
        g.setColour (juce::Colour (0xAAFFFFFF));
        g.drawVerticalLine (x, 0.0f, (float) height);
    }
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

    if (! editable) return;   // Live mode, or Locked: the pattern is read-only

    if (e.mods.isPopupMenu())
    {
        // Right-click deletes outright -- no deferral, since a double right-click
        // means nothing here.
        if (it != hits.end())
            hits.erase (it);
        repaint();
        if (onEdited) onEdited();
        return;
    }

    if (it != hits.end())
    {
        // Pressing an existing hit arms a velocity drag.  It is only erased if the
        // press turns out to be a plain click (see mouseUp).
        pressRow    = row;
        pressTick   = it->tick;
        pressVel    = it->velocity;
        pressY      = e.getPosition().y;
        velDragging = false;
        return;
    }

    hits.push_back ({ tick0, SequencerVelocity::kDefault });
    std::sort (hits.begin(), hits.end(),
               [] (const SeqHit& a, const SeqHit& b) { return a.tick < b.tick; });

    repaint();
    if (onEdited) onEdited();
}

//==============================================================================
inline std::vector<SeqHit>::iterator SeqControlPanel::GridCanvas::findHit (int row, int tick)
{
    auto& hits = pattern->tracks[row].hits;
    return std::find_if (hits.begin(), hits.end(),
                         [tick] (const SeqHit& h) { return h.tick == tick; });
}

inline void SeqControlPanel::GridCanvas::mouseDrag (const juce::MouseEvent& e)
{
    if (pattern == nullptr || pressRow < 0 || ! editable) return;

    const int dy = pressY - e.getPosition().y;

    // A few pixels of slack, so the wobble inside a double-click is not a drag.
    if (! velDragging && std::abs (dy) < 3) return;
    velDragging = true;

    // Cancel any pending erase: this press is an edit, not a click.
    pendingRemoveRow = -1;

    auto it = findHit (pressRow, pressTick);
    if (it == pattern->tracks[pressRow].hits.end()) return;

    it->velocity = SequencerVelocity::fromDrag (pressVel, dy, e.mods.isShiftDown());

    repaint();
    if (onEdited) onEdited();
}

inline void SeqControlPanel::GridCanvas::mouseUp (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);

    if (pattern == nullptr || pressRow < 0)
    {
        pressRow = -1;
        return;
    }

    if (! velDragging)
    {
        // Plain click on a hit: erase it, but only after the double-click window
        // has passed, so a double-click can still reset the velocity instead.
        pendingRemoveRow  = pressRow;
        pendingRemoveTick = pressTick;

        startTimer (juce::jmax (120, juce::MouseEvent::getDoubleClickTimeout()));
    }

    pressRow    = -1;
    velDragging = false;
}

inline void SeqControlPanel::GridCanvas::timerCallback()
{
    stopTimer();

    if (pattern == nullptr || pendingRemoveRow < 0) return;

    auto it = findHit (pendingRemoveRow, pendingRemoveTick);
    if (it != pattern->tracks[pendingRemoveRow].hits.end())
    {
        pattern->tracks[pendingRemoveRow].hits.erase (it);
        repaint();
        if (onEdited) onEdited();
    }

    pendingRemoveRow  = -1;
    pendingRemoveTick = -1;
}

inline void SeqControlPanel::GridCanvas::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (pattern == nullptr || ! editable) return;

    // The first click queued an erase; this press was a double-click, so cancel it
    // and treat the gesture as "reset this hit's velocity" instead.
    stopTimer();
    pendingRemoveRow  = -1;
    pendingRemoveTick = -1;

    const int row  = rowAt (e.y);
    const int step = stepAt (e.x);
    if (row < 0 || step < 0) return;

    const int tps   = juce::jmax (1, pattern->ticksPerStep());
    const int tick0 = step * tps;

    auto& hits = pattern->tracks[row].hits;
    auto it = std::find_if (hits.begin(), hits.end(), [tick0, tps] (const SeqHit& h)
                            { return h.tick >= tick0 && h.tick < tick0 + tps; });
    if (it == hits.end()) return;

    it->velocity = SequencerVelocity::kDefault;

    repaint();
    if (onEdited) onEdited();
}

//==============================================================================
// HeaderCanvas
//==============================================================================
inline int SeqControlPanel::HeaderCanvas::rowAt (int y) const
{
    const int r = (y + scrollY) / rowH;
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
        const int y = r * rowH - scrollY;
        if (y + rowH < 0 || y > getHeight()) continue;

        const auto& tr = pattern->tracks[r];
        const bool selectedRow = false;

        g.setColour ((r % 2) ? juce::Colour (0xFF1C1C1C) : juce::Colour (0xFF212121));
        g.fillRect (0, y, w, rowH);

        // Number
        g.setColour (juce::Colour (0xFF9A9A9A));
        g.setFont (numFont);
        g.drawText (juce::String (r + 1),
                    juce::Rectangle<int> (0, y, kNumW, rowH),
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
                    juce::Rectangle<int> (kNumW, y, nameW - 2, rowH),
                    juce::Justification::centredLeft, true);

        // Volume (drag to adjust)
        const int volX = w - kVolW - kMuteW;
        g.setColour (juce::Colour (0xFF2E2E2E));
        g.fillRect (volX + 1, y + 2, kVolW - 4, rowH - 4);
        g.setColour (tr.volume >= 0.999f ? juce::Colour (0xFF8A8A8A) : juce::Colour (0xFF00CF7F));
        g.setFont (nameFont);
        g.drawText (juce::String (juce::roundToInt (tr.volume * 100.0f)) + "%",
                    juce::Rectangle<int> (volX, y, kVolW, rowH),
                    juce::Justification::centred, false);

        // Mute
        const int muteX = w - kMuteW;
        g.setColour (tr.mute ? juce::Colour (0xFFCC4444) : juce::Colour (0xFF303030));
        g.fillRect (muteX + 1, y + 2, kMuteW - 3, rowH - 4);
        g.setColour (juce::Colour (0xFFDDDDDD));
        g.setFont (nameFont);
        g.drawText ("M", juce::Rectangle<int> (muteX, y, kMuteW, rowH),
                    juce::Justification::centred, false);

        g.setColour (juce::Colour (0xFF2E2E2E));
        g.drawHorizontalLine (y + rowH - 1, 0.0f, (float) w);
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

inline void SeqControlPanel::HeaderCanvas::mouseMove (const juce::MouseEvent& e)
{
    const int w = getWidth();

    if (e.x >= w - kMuteW)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else if (e.x >= w - kVolW - kMuteW)
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

inline void SeqControlPanel::HeaderCanvas::mouseDrag (const juce::MouseEvent& e)
{
    if (pattern == nullptr || dragRow < 0) return;

    const int dy = dragStartY - e.getPosition().y;

    // Ignore the first pixel or two, so the wobble inside a double-click cannot
    // drag the level before the reset lands.
    if (! dragging && std::abs (dy) < 3) return;
    dragging = true;

    // Vertical drag: up = louder.  200 px of travel spans the full range.
    const float delta = (float) dy / 200.0f;
    pattern->tracks[dragRow].volume = juce::jlimit (0.0f, 1.0f, dragStartVol + delta);
    repaint();
    if (onEdited) onEdited();
}

inline void SeqControlPanel::HeaderCanvas::mouseUp (const juce::MouseEvent&)
{
    dragRow  = -1;
    dragging = false;
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
    const int shown = juce::jmax (steps, juce::jmin (pattern->displaySteps(),
                                                     steps + kMaxGhostSteps));
    const int spb   = juce::jmax (1, pattern->stepsPerBar());
    const int spBeat = juce::jmax (1, spb / 4);
    const auto font = juce::Font (juce::FontOptions (9.0f, juce::Font::bold));

    // Overhang: stored notes that sit beyond the loop after a signature change.
    if (shown > steps)
    {
        const int x0 = steps * kStepW - scrollX;
        const int w  = (shown - steps) * kStepW;

        g.setColour (juce::Colour (0xFF221703));
        g.fillRect (x0, 0, w, kRulerH);

        g.setColour (juce::Colour (0xFFB87800));
        g.fillRect (x0 - 1, 0, 2, kRulerH);

        g.setColour (juce::Colour (0xFF8A6A2A));
        g.drawText ("outside loop (" + juce::String (pattern->hitsPastLoop()) + ")",
                    juce::Rectangle<int> (x0 + 4, 0, juce::jmax (1, w - 4), kRulerH),
                    juce::Justification::centredLeft, false);
    }

    for (int s = 0; s < shown; ++s)
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
