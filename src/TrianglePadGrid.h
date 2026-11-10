#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include <vector>
#include <memory>
#include <functional>
#include <cstdio>

// ================================================================================
// TrianglePad – one trapezoid pad (geometry derived from its bounds)
// ================================================================================
class TrianglePad : public juce::Component
{
public:
    enum class State     { Empty, Loaded, Selected, Triggered };
    enum class Direction { Up, Down };

    TrianglePad (int index, Direction dir)
        : padIndex (index), direction (dir)
    {
        flashTimer.owner = this;
    }

    void setSampleName (const juce::String& name)
    {
        sampleName   = name;
        if (currentState != State::Selected)  // preserve selection
            currentState = name.isNotEmpty() ? State::Loaded : State::Empty;
        repaint();
    }

    void setState (State s) { currentState = s; repaint(); }
    State getState()        const { return currentState; }
    int   getPadIndex()     const { return padIndex; }
    const juce::String& getSampleName() const { return sampleName; }

    // MNFreeze dot — lights up ice blue when this pad has an active MIDI-Note-Freeze loop.
    void setMidiFreezeLocked (bool locked) { midiFreezeLocked = locked; repaint(); }
    bool isMidiFreezeLocked() const        { return midiFreezeLocked; }

    std::function<void(int)> onClicked;

    // Go fuchsia immediately with no auto-restore timer.
    // Use for MIDI note-on and mouse-down — colour lasts until triggerEnd() is called.
    void triggerStart()
    {
        flashTimer.stopTimer();
        if (currentState != State::Triggered)
            preFlashState = currentState;
        currentState = State::Triggered;
        repaint();
    }

    // Restore from fuchsia back to preFlashState (Selected / Loaded / Empty).
    // minHoldMs > 0 ensures the fuchsia is visible even for very short notes.
    void triggerEnd (int minHoldMs = 80)
    {
        if (currentState != State::Triggered)
            return;
        flashTimer.stopTimer();
        if (minHoldMs > 0)
            flashTimer.startTimer (minHoldMs);
        else
            restoreFromFlash();
    }

    // Legacy wrapper kept for code paths that have no paired noteOff (e.g. one-shot test tones).
    // Always restores after a fixed 150ms so fuchsia never gets stuck.
    void triggerFlash()
    {
        triggerStart();
        flashTimer.startTimer (150);
    }

private:
    void restoreFromFlash()
    {
        flashTimer.stopTimer();
        if (currentState == State::Triggered)
        {
            currentState = preFlashState;
            repaint();
        }
    }

    struct FlashTimer : public juce::Timer
    {
        TrianglePad* owner = nullptr;
        void timerCallback() override { owner->restoreFromFlash(); }
    } flashTimer;

public:
    std::function<void(int)> onReleased;  // called on mouseUp — use to stop note early

    void mouseEnter (const juce::MouseEvent&) override { isHovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { isHovered = false; repaint(); }
    void mouseDown  (const juce::MouseEvent& e) override
    {
        if (cachedPath.contains (e.position) && onClicked)
            onClicked (padIndex);
        // triggerStart() is called by the grid's handlePadClicked after onClicked.
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Restore immediately on mouse release — fuchsia lasts exactly the click duration.
        triggerEnd (0);
        // Always fire onReleased regardless of cursor position — prevents stuck notes
        // when the user drags the mouse outside the pad before releasing.
        if (onReleased) onReleased (padIndex);
        (void)e;
    }

    void paint (juce::Graphics& g) override
    {
        juce::Colour fill;
        switch (currentState)
        {
            case State::Empty:     fill = juce::Colour (0xFF3A3A3A); break;
            case State::Loaded:    fill = juce::Colour (0xFF3A3A3A); break;
            case State::Selected:  fill = juce::Colour (0xB3BE72FF); break;
            case State::Triggered: fill = juce::Colour (0xCCFF0B8F); break;
        }
        if (isHovered && currentState != State::Triggered)
            fill = fill.brighter (0.12f);

        g.setColour (fill);
        g.fillPath (cachedPath);
        g.setColour (juce::Colour (0xFFA7FFD9));
        g.strokePath (cachedPath, juce::PathStrokeType (1.0f));

        auto b = getLocalBounds().toFloat();

        // Activity dot in the middle of the pad.
        if (currentState != State::Empty)
        {
            juce::Colour dotColour;
            float dotR = 3.0f;
            if (currentState == State::Triggered)
            {
                dotColour = juce::Colour (0xFFFF0B8F);
            }
            else if (midiFreezeLocked)
            {
                dotColour = juce::Colour (0xFF00CFFF);
                dotR = 4.5f;
            }
            else
            {
                dotColour = juce::Colour (0xFF888888);
            }
            g.setColour (dotColour);
            g.fillEllipse (b.getCentreX() - dotR, b.getCentreY() - dotR, dotR * 2.0f, dotR * 2.0f);
        }

        // Pad number: bottom-left on the top row, top-left on the bottom row.
        g.setColour (juce::Colour (0xFFFFDD44));
        g.setFont (juce::Font (12.0f, juce::Font::bold));
        const int numY = (direction == Direction::Down) ? (int) b.getBottom() - 17
                                                        : (int) b.getY() + 2;
        g.drawText (juce::String (padIndex + 1), (int) b.getX() + 6, numY, 30, 14,
                    juce::Justification::left, false);

        // Sample name on the opposite side of the number.
        if (currentState != State::Empty && sampleName.isNotEmpty())
        {
            auto dn = sampleName.upToLastOccurrenceOf (".", false, false);
            if (dn.isEmpty()) dn = sampleName;
            if (dn.length() > 18) dn = dn.substring (0, 15) + "...";

            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (9.0f));

            const int textY = (direction == Direction::Down) ? (int) b.getY() + 2
                                                             : (int) b.getBottom() - 17;
            auto textArea = juce::Rectangle<int> ((int) b.getX() + 6, textY,
                                                  (int) b.getWidth() - 12, 14);
            g.drawFittedText (dn, textArea, juce::Justification::left, 1);
        }
    }

    bool hitTest (int x, int y) override
    {
        return cachedPath.contains ((float)x, (float)y);
    }

    void resized() override
    {
        updatePath();
    }

private:
    void updatePath()
    {
        auto b = getLocalBounds().toFloat();
        if (b.getWidth() <= 0.0f || b.getHeight() <= 0.0f) return;

        juce::Path p;
        p.addRoundedRectangle (b, juce::jmin (8.0f, b.getHeight() * 0.12f));
        cachedPath = p;
    }

    int          padIndex;
    Direction    direction;
    State        currentState    = State::Empty;
    State        preFlashState   = State::Empty;
    juce::String sampleName;
    bool         isHovered       = false;
    bool         midiFreezeLocked = false;  // ice-blue dot when MNFreeze loop is active

    juce::Path cachedPath;
};

// ================================================================================
// TrianglePadRow – pads fill the row vertically, with 1px gaps, preserving shape
// ================================================================================
class TrianglePadRow : public juce::Component
{
public:
    explicit TrianglePadRow (int startPadIndex, bool firstPadIsDown)
    {
        for (int i = 0; i < 8; ++i)
        {
            // Rectangular pads: one orientation per row so numbers and sample
            // names line up instead of alternating top/bottom like the trapezoids.
            auto dir = firstPadIsDown ? TrianglePad::Direction::Down
                                      : TrianglePad::Direction::Up;
            auto p = std::make_unique<TrianglePad> (startPadIndex + i, dir);
            p->onClicked  = [this] (int idx) { if (onPadClicked)  onPadClicked  (idx); };
            p->onReleased = [this] (int idx) { if (onPadReleased) onPadReleased (idx); };
            addAndMakeVisible (*p);
            pads.push_back (std::move (p));
        }
    }

    TrianglePad* getPad (int i)
    {
        if (i >= 0 && i < (int)pads.size()) return pads[(size_t)i].get();
        return nullptr;
    }

    void triggerStart (int i) { if (auto* p = getPad (i)) p->triggerStart(); }
    void triggerEnd   (int i, int minHoldMs = 80) { if (auto* p = getPad (i)) p->triggerEnd (minHoldMs); }

    std::function<void(int)> onPadClicked;
    std::function<void(int)> onPadReleased;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::padBackground());  // dark background between rectangular pads
    }

void resized() override
{
    if (pads.empty() || getWidth() <= 0 || getHeight() <= 0) return;

    const int   numPads = 8;
    const float rowW    = (float)getWidth();
    const float rowH    = (float)getHeight();

    // Rectangular pads: eight equal slots with a small gap between them.
    constexpr float gap = 4.0f;
    const float slotW = rowW / numPads;
    const float padW  = juce::jmax (8.0f, slotW - gap);

    float x = gap * 0.5f;
    for (int i = 0; i < numPads; ++i)
    {
        auto* pad = pads[i].get();
        pad->setBounds (juce::Rectangle<float> (x, 0.0f, padW, rowH).toNearestInt());
        x += slotW;
    }
}
private:
    std::vector<std::unique_ptr<TrianglePad>> pads;
};

// ================================================================================
// TrianglePadGrid – top and bottom rows
// ================================================================================
class TrianglePadGrid : public juce::Component
{
public:
    explicit TrianglePadGrid (int rowH = 85)
        : rowHeight (rowH),
          topRow    (0, true),
          bottomRow (8, false)
    {
        addAndMakeVisible (topRow);
        addAndMakeVisible (bottomRow);
        topRow.onPadClicked     = [this] (int idx) { handlePadClicked  (idx); };
        bottomRow.onPadClicked  = [this] (int idx) { handlePadClicked  (idx); };
        topRow.onPadReleased    = [this] (int idx) { handlePadReleased (idx); };
        bottomRow.onPadReleased = [this] (int idx) { handlePadReleased (idx); };
    }

    void setRowHeight (int h) { rowHeight = h; resized(); }

    void resized() override
    {
        const int W = getWidth(), H = getHeight();
        if (W <= 0 || H <= 0 || rowHeight <= 0) return;

        // Clamp row heights to avoid negative positioning
        int topH = juce::jmin (rowHeight, H / 2);
        int bottomH = juce::jmin (rowHeight, H - topH);
        topRow.setBounds    (0, 0,             W, topH);
        bottomRow.setBounds (0, H - bottomH,   W, bottomH);
    }

    bool hitTest (int, int y) override
    {
        return (y < rowHeight) || (y >= getHeight() - rowHeight);
    }

    void setPadSampleName (int idx, const juce::String& name)
    {
        if (auto* p = padAt (idx)) p->setSampleName (name);
    }

    void selectPad (int idx)
    {
        if (selectedIndex >= 0 && selectedIndex != idx)
            if (auto* prev = padAt (selectedIndex))
                prev->setState (prev->getSampleName().isNotEmpty()
                                ? TrianglePad::State::Loaded
                                : TrianglePad::State::Empty);
        selectedIndex = idx;
        if (auto* p = padAt (idx)) p->setState (TrianglePad::State::Selected);
        printf ("[MOUSE-SELECT] Pad %d selected by mouse — state=Selected\n", idx + 1);
        if (onPadSelected) onPadSelected (idx);
    }

    // Visual-only pad selection — updates the grid highlight without firing onPadSelected.
    // Use when the caller is about to manage the pad switch logic itself.
    void selectPadQuiet (int idx)
    {
        if (selectedIndex >= 0 && selectedIndex != idx)
            if (auto* prev = padAt (selectedIndex))
                prev->setState (prev->getSampleName().isNotEmpty()
                                ? TrianglePad::State::Loaded
                                : TrianglePad::State::Empty);
        selectedIndex = idx;
        if (auto* p = padAt (idx)) p->setState (TrianglePad::State::Selected);
    }

    // Restore the currently selected pad to Loaded/Empty without selecting a new one.
    // Use when an external selection source (e.g. GlobalLoopColumn) takes focus.
    void clearSelection()
    {
        if (selectedIndex >= 0)
            if (auto* prev = padAt (selectedIndex))
                prev->setState (prev->getSampleName().isNotEmpty()
                                ? TrianglePad::State::Loaded
                                : TrianglePad::State::Empty);
        selectedIndex = -1;
    }

    // Start fuchsia with no auto-timer — must be paired with triggerEnd().
    // Used for mouse-down and MIDI note-on.
    void triggerStart (int idx)
    {
        if (auto* p = padAt (idx)) p->triggerStart();
    }

    // Restore from fuchsia. minHoldMs=0 restores immediately (mouse/MIDI use case).
    void triggerEnd (int idx, int minHoldMs = 0)
    {
        if (auto* p = padAt (idx)) p->triggerEnd (minHoldMs);
    }

    // Legacy: fixed 150ms flash for code paths with no paired end event.
    void triggerFlash (int idx)
    {
        if (auto* p = padAt (idx)) p->triggerFlash();
    }

    // MNFreeze dot — call with locked=true when freeze loop is active, false when released.
    void setMidiFreezeLocked (int idx, bool locked)
    {
        if (auto* p = padAt (idx)) p->setMidiFreezeLocked (locked);
    }

    int getSelectedPadIndex() const { return selectedIndex; }

    std::function<void(int)> onPadSelected;
    std::function<void(int)> onPadTriggered;   // mouseDown on pad — fire noteOn
    std::function<void(int)> onPadReleased;    // mouseUp  on pad — fire noteOff

    void paint (juce::Graphics&) override {}

private:
    int            rowHeight;
    TrianglePadRow topRow, bottomRow;
    int            selectedIndex = -1;

    TrianglePad* padAt (int idx)
    {
        if (idx >= 0 && idx < 8)  return topRow.getPad (idx);
        if (idx >= 8 && idx < 16) return bottomRow.getPad (idx - 8);
        return nullptr;
    }

    void handlePadClicked (int idx)
    {
        DBG ("[PAD-UI] Pad " << idx + 1 << " clicked");

        if (selectedIndex != idx)
        {
            // First click: select the pad (turns purple).
            // No fuchsia flash — the purple selection IS the visual feedback.
            selectPad (idx);
        }
        else
        {
            // Re-click on already-selected pad: show fuchsia for the duration of the press.
            triggerStart (idx);
        }

        // noteOn for both cases — duration is determined by the matching mouseUp.
        if (onPadTriggered) onPadTriggered (idx);
    }

    void handlePadReleased (int idx)
    {
        // noteOff — ends the note started in handlePadClicked.
        if (onPadReleased) onPadReleased (idx);
    }
};

// ================================================================================
// GlobalControlsBar — bank navigation (Up/Down) wired to GJM bank switching
// ================================================================================
class GlobalControlsBar : public juce::Component
{
public:
    GlobalControlsBar()
    {
        bankUpBtn.setButtonText ("UP");
        bankUpBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xFF4A4A4A));
        bankUpBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFCECECE));
        addAndMakeVisible (bankUpBtn);

        bankLabel.setJustificationType (juce::Justification::centred);
        bankLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xFF1E1E1E));
        bankLabel.setColour (juce::Label::textColourId,       juce::Colour (0xFF4BFF7A));
        bankLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        // Long kit names shrink to fit instead of being clipped mid-word.
        bankLabel.setMinimumHorizontalScale (0.6f);
        bankLabel.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (bankLabel);

        bankDownBtn.setButtonText ("Down");
        bankDownBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xFF4A4A4A));
        bankDownBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFCECECE));
        bankDownBtn.setEnabled (false);
        addAndMakeVisible (bankDownBtn);

        bankUpBtn.onClick   = [this]
        {
            if (currentBank < maxBank) { ++currentBank; updateBank(); if (onBankChanged) onBankChanged (currentBank); }
        };
        bankDownBtn.onClick = [this]
        {
            if (currentBank > 1) { --currentBank; updateBank(); if (onBankChanged) onBankChanged (currentBank); }
        };

        updateBank();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF1E1E1E));
        g.setColour (juce::Colour (0xFF3A3A3A));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float)getWidth());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 0);
        // Keep this width in sync with MainComponent::resized(), which skips the
        // same span before laying out the kit/session controls on this row.
        auto left = area.removeFromLeft (260);
        bankDownBtn.setBounds (left.removeFromLeft (36).withSizeKeepingCentre (32, 20));
        left.removeFromLeft (4);
        bankLabel.setBounds   (left.removeFromLeft (180).withSizeKeepingCentre (176, 20));
        left.removeFromLeft (4);
        bankUpBtn.setBounds   (left.removeFromLeft (36).withSizeKeepingCentre (32, 20));
        // Right area left for gjmStatusLabel (MainComponent child, overlaid on this row)
    }

    // Fired with the new 1-based bank index whenever the user presses Up or Down.
    std::function<void(int)> onBankChanged;

    // Called by MainComponent when a GJM is loaded to set the bank ceiling.
    void setMaxBank (int newMax)
    {
        maxBank = juce::jmax (1, newMax);
        currentBank = juce::jlimit (1, maxBank, currentBank);
        updateBank();
    }

    // Programmatically move to a 1-based bank index without firing onBankChanged.
    void setBankIndex (int bank1Based)
    {
        currentBank = juce::jlimit (1, maxBank, bank1Based);
        updateBank();
    }

    // Kit name shown after the bank number, e.g. "Bank 3 : empty",
    // "Bank 1 : no_name" or "Bank 2 : MyKit". Empty suffix = just "Bank N".
    void setBankSuffix (const juce::String& suffix)
    {
        bankSuffix = suffix;
        updateBank();
    }

    // Enable/disable both nav buttons (used while GJM is being parsed in background).
    void setNavEnabled (bool enabled)
    {
        bankUpBtn.setEnabled   (enabled && currentBank < maxBank);
        bankDownBtn.setEnabled (enabled && currentBank > 1);
    }

    int getCurrentBank() const { return currentBank; }

private:
    juce::TextButton bankUpBtn, bankDownBtn;
    juce::Label      bankLabel;
    juce::String     bankSuffix;   // kit name / "no_name" / "empty" for the current bank
    int  currentBank = 1;
    int  maxBank     = 1;   // set to GjmManager::kNumBanks when a GJM is loaded

    void updateBank()
    {
        juce::String text = "Bank " + juce::String (currentBank);
        if (bankSuffix.isNotEmpty())
            text += " : " + bankSuffix;

        bankLabel.setText (text, juce::dontSendNotification);
        bankDownBtn.setEnabled (currentBank > 1);
        bankUpBtn.setEnabled   (currentBank < maxBank);
    }
};