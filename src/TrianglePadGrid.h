#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
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
        // Restore fuchsia after button release, with 80ms minimum visibility.
        triggerEnd (80);
        if (cachedPath.contains (e.position) && onReleased)
            onReleased (padIndex);
    }

    void paint (juce::Graphics& g) override
    {
        juce::Colour fill;
        float strokeW = 2.0f;
        switch (currentState)
        {
            case State::Empty:     fill = juce::Colour (0xFF3A3A3A); break;
            case State::Loaded:    fill = juce::Colour (0xFF3A3A3A); break;
            case State::Selected:  fill = juce::Colour (0xB3BE72FF); strokeW = 3.0f; break;
            case State::Triggered: fill = juce::Colour (0xCCFF0B8F); break;
        }
        if (isHovered && currentState != State::Triggered)
            fill = fill.brighter (0.12f);

        g.setColour (fill);
        g.fillPath (cachedPath);
        g.setColour (juce::Colour (0xFFA7FFD9));
        // g.strokePath (cachedPath, juce::PathStrokeType (strokeW));
        g.strokePath (cachedPath, juce::PathStrokeType (1.0f));

        if (currentState != State::Empty)
        {
            auto b = getLocalBounds().toFloat();
            auto [cx, cy] = std::make_pair (b.getCentreX(), b.getCentreY());
            g.setColour (currentState == State::Triggered
                         ? juce::Colour (0xFFFF0B8F)
                         : juce::Colour (0xFF888888));
            g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
        }

                if (currentState != State::Empty && sampleName.isNotEmpty())
                {
                    auto dn = sampleName.upToLastOccurrenceOf (".", false, false);
                    if (dn.isEmpty()) dn = sampleName;
                    // Increased from 12 to 18 characters before truncation
                    if (dn.length() > 18) dn = dn.substring (0, 15) + "...";
                    g.saveState();
                    g.reduceClipRegion (cachedPath);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (9.0f));
                    auto b = getLocalBounds().toFloat();
            
                    // Calculate the visible left edge of the trapezoid
                    float visibleLeftEdge = 0.0f;
                    float visibleRightEdge = b.getWidth();
            
                    // Fixed proportion from your sketch: narrow / wide = 13.555 / 44.694
                    constexpr float narrowToWideRatio = 13.555f / 44.694f;  // ≈ 0.3033
                    float narrowW = b.getWidth() * narrowToWideRatio;
                    float narrowX = (b.getWidth() - narrowW) * 0.5f;
            
                    // Check if this is an edge pad
                    bool isLeftEdge = (padIndex == 0 || padIndex == 8);
                    bool isRightEdge = (padIndex == 7 || padIndex == 15);
            
                    if (isLeftEdge)
                    {
                        // Left edge pad: visible area starts at 0 (trapezoid starts at component edge)
                        visibleLeftEdge = 0.0f;
                        // For left edge pads, the narrow side is at 0, not narrowX
                    }
                    else if (isRightEdge)
                    {
                        // Right edge pad: visible area ends at narrowX + narrowW
                        visibleRightEdge = narrowX + narrowW;
                    }
                    // For interior pads, visible area is full width (0 to width)
            
                    // Calculate text bounds based on visible area
                    // Add 2 character margin (18px) to all pads for better visual spacing
                    // Left edge pads: 56px + 18px = 74px
                    // Other pads: 4px + 18px = 22px
                    float marginFromVisibleEdge = isLeftEdge ? 74.0f : 22.0f;
                    float textX = visibleLeftEdge + marginFromVisibleEdge;
                    float textWidth = visibleRightEdge - visibleLeftEdge - (marginFromVisibleEdge * 2.0f);
            
                    // Ensure text width is positive
                    textWidth = juce::jmax(1.0f, textWidth);
            
                    // Position text based on trapezoid direction
                    if (direction == Direction::Down)
                    {
                        // Down-pointing trapezoid: wider base at TOP, text at top
                        g.drawText (dn, (int)textX, (int)b.getY() + 2,
                                    (int)textWidth, 14,
                                    juce::Justification::topLeft, true);
                    }
                    else
                    {
                        // Up-pointing trapezoid: wider base at BOTTOM, text at bottom
                        g.drawText (dn, (int)textX, (int)b.getBottom() - 16,
                                    (int)textWidth, 14,
                                    juce::Justification::bottomLeft, true);
                    }
                    g.restoreState();
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
            float w = b.getWidth();
            float h = b.getHeight();
            if (w <= 0.0f || h <= 0.0f) return;

            // Fixed proportion from your sketch: narrow / wide = 13.555 / 44.694
            constexpr float narrowToWideRatio = 13.555f / 44.694f;  // ≈ 0.3033
            float narrowW = w * narrowToWideRatio;
            float narrowX = (w - narrowW) * 0.5f;

            juce::Path p;
        
            // Check if this is an edge pad that needs to be a half-trapezoid
            bool isLeftEdge = (padIndex == 0 || padIndex == 8);     // Pad 1 or 9
            bool isRightEdge = (padIndex == 7 || padIndex == 15);   // Pad 8 or 16
        
            if (isLeftEdge)
            {
                // Left edge pad - half trapezoid flush against left edge
                if (direction == Direction::Down)
                {
                    // Pad 1: Down direction, narrow at bottom, wide at top
                    p.startNewSubPath (0.0f, 0.0f);
                    p.lineTo         (w,    0.0f);
                    p.lineTo         (narrowX + narrowW, h);
                    p.lineTo         (0.0f, h);  // Changed from narrowX to 0.0f for flush left edge
                    p.closeSubPath();
                }
                else // Up direction (Pad 9)
                {
                    // Pad 9: Up direction, narrow at top, wide at bottom
                    p.startNewSubPath (0.0f, 0.0f);  // Changed from narrowX to 0.0f for flush left edge
                    p.lineTo         (narrowX + narrowW, 0.0f);
                    p.lineTo         (w,    h);
                    p.lineTo         (0.0f, h);
                    p.closeSubPath();
                }
            }
            else if (isRightEdge)
            {
                // Right edge pad - half trapezoid flush against right edge
                if (direction == Direction::Up)
                {
                    // Pad 8: Up direction, narrow at top, wide at bottom
                    p.startNewSubPath (narrowX, 0.0f);
                    p.lineTo         (w, 0.0f);  // Changed from narrowX + narrowW to w for flush right edge
                    p.lineTo         (w,    h);
                    p.lineTo         (0.0f, h);
                    p.closeSubPath();
                }
                else // Down direction (Pad 16)
                {
                    // Pad 16: Down direction, narrow at bottom, wide at top
                    p.startNewSubPath (0.0f, 0.0f);
                    p.lineTo         (w,    0.0f);
                    p.lineTo         (w, h);  // Changed from narrowX + narrowW to w for flush right edge
                    p.lineTo         (narrowX, h);
                    p.closeSubPath();
                }
            }
            else
            {
                // Regular interior pad - full trapezoid
                if (direction == Direction::Down)
                {
                    p.startNewSubPath (0.0f, 0.0f);
                    p.lineTo         (w,    0.0f);
                    p.lineTo         (narrowX + narrowW, h);
                    p.lineTo         (narrowX, h);
                    p.closeSubPath();
                }
                else // Up
                {
                    p.startNewSubPath (narrowX, 0.0f);
                    p.lineTo         (narrowX + narrowW, 0.0f);
                    p.lineTo         (w,    h);
                    p.lineTo         (0.0f, h);
                    p.closeSubPath();
                }
            }
            cachedPath = p;
        }

    int          padIndex;
    Direction    direction;
    State        currentState  = State::Empty;
    State        preFlashState = State::Empty;
    juce::String sampleName;
    bool         isHovered     = false;

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
            bool isDown = firstPadIsDown ? (i % 2 == 0) : (i % 2 != 0);
            auto dir    = isDown ? TrianglePad::Direction::Down
                                 : TrianglePad::Direction::Up;
            auto p = std::make_unique<TrianglePad> (startPadIndex + i, dir);
            p->onClicked = [this] (int idx) { if (onPadClicked) onPadClicked (idx); };
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
        g.fillAll (juce::Colour (0xFFA7FFD9));  // green background (minimised)
    }

void resized() override
{
    if (pads.empty() || getWidth() <= 0 || getHeight() <= 0) return;

    const int   numPads = 8;
    const float rowW    = (float)getWidth();
    const float rowH    = (float)getHeight();

    // Make pads wider than their slot so they overlap
    // This creates the visual effect of smaller gaps while preserving trapezoid angles
    float padWidth  = rowW / numPads * 1.55f;  // 15% wider than slot (adjust as needed)
    float padHeight = rowH;
    
    // Position pads with overlap instead of gaps
    float stepX = rowW / numPads;  // Each pad starts this far from the previous
    float x = -((padWidth - stepX) * 0.5f);  // Center the overlap
    float y = 0.0f;

    for (int i = 0; i < numPads; ++i)
    {
        auto* pad = pads[i].get();
        pad->setBounds (juce::Rectangle<float> (x, y, padWidth, padHeight).toNearestInt());
        x += stepX;  // Move by slot width (not pad width), creating overlap
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
        topRow.onPadClicked    = [this] (int idx) { handlePadClicked (idx); };
        bottomRow.onPadClicked = [this] (int idx) { handlePadClicked (idx); };
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

    void triggerFlash (int idx)
    {
        if (auto* p = padAt (idx)) p->triggerFlash();
    }

    int getSelectedPadIndex() const { return selectedIndex; }

    std::function<void(int)> onPadSelected;
    std::function<void(int)> onPadTriggered;

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

        // Select first so preFlashState captures Selected, not Empty/Loaded
        if (selectedIndex != idx)
            selectPad (idx);

        // Always flash regardless of prior selection state
        triggerFlash (idx);

        if (onPadTriggered) onPadTriggered (idx);
    }
};

// ================================================================================
// GlobalControlsBar – unchanged from your original (but included for completeness)
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

        bankLabel.setText ("Bank 1", juce::dontSendNotification);
        bankLabel.setJustificationType (juce::Justification::centred);
        bankLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        bankLabel.setFont (juce::Font (12.0f));
        addAndMakeVisible (bankLabel);

        bankDownBtn.setButtonText ("Down");
        bankDownBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xFF4A4A4A));
        bankDownBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFCECECE));
        bankDownBtn.setEnabled (false);
        addAndMakeVisible (bankDownBtn);

        globalLabel.setText ("Global Controls", juce::dontSendNotification);
        globalLabel.setJustificationType (juce::Justification::centredRight);
        globalLabel.setColour (juce::Label::textColourId, juce::Colour (0xFF888888));
        globalLabel.setFont (juce::Font (12.0f));
        addAndMakeVisible (globalLabel);

        bankUpBtn.onClick   = [this] { ++currentBank; updateBank(); if (onBankChanged) onBankChanged(currentBank); };
        bankDownBtn.onClick = [this] { if (currentBank > 1) { --currentBank; updateBank(); if (onBankChanged) onBankChanged(currentBank); } };
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
        auto left = area.removeFromLeft (144);
        bankUpBtn.setBounds   (left.removeFromLeft (36).withSizeKeepingCentre (32, 20));
        left.removeFromLeft (4);
        bankLabel.setBounds   (left.removeFromLeft (60).withSizeKeepingCentre (56, 20));
        left.removeFromLeft (4);
        bankDownBtn.setBounds (left.removeFromLeft (36).withSizeKeepingCentre (32, 20));
        globalLabel.setBounds (area);
    }

    std::function<void(int)> onBankChanged;

private:
    juce::TextButton bankUpBtn, bankDownBtn;
    juce::Label      bankLabel, globalLabel;
    int              currentBank = 1;

    void updateBank()
    {
        bankLabel.setText ("Bank " + juce::String (currentBank),
                           juce::dontSendNotification);
        bankDownBtn.setEnabled (currentBank > 1);
    }
};