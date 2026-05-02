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
            // Dot color priority: triggered (fuchsia) > MNFreeze active (ice blue) > normal (gray)
            juce::Colour dotColour;
            float dotR = 3.0f;
            if (currentState == State::Triggered)
            {
                dotColour = juce::Colour (0xFFFF0B8F);  // fuchsia — MIDI hit
            }
            else if (midiFreezeLocked)
            {
                dotColour = juce::Colour (0xFF00CFFF);  // ice blue — MNFreeze loop active
                dotR = 4.5f;  // slightly larger to be clearly visible
            }
            else
            {
                dotColour = juce::Colour (0xFF888888);  // gray — loaded/selected, idle
            }
            g.setColour (dotColour);
            g.fillEllipse (cx - dotR, cy - dotR, dotR * 2.0f, dotR * 2.0f);
        }

                // Draw pad number on the narrow base (opposite side from filename)
                                {
                                    auto b = getLocalBounds().toFloat();
                    
                                    // Fixed proportion from your sketch: narrow / wide = 13.555 / 44.694
                                    constexpr float narrowToWideRatio = 13.555f / 44.694f;  // ≈ 0.3033
                                    float narrowW = b.getWidth() * narrowToWideRatio;
                                    float narrowX = (b.getWidth() - narrowW) * 0.5f;
            
                                    // Check if this is an edge pad
                                    bool isLeftEdge = (padIndex == 0 || padIndex == 8);
                                    bool isRightEdge = (padIndex == 7 || padIndex == 15);
            
                                    g.saveState();
                                    g.reduceClipRegion (cachedPath);
                    
                                    // Draw pad number in yellow on narrow side
                                    g.setColour (juce::Colour (0xFFFFDD44));
                                    g.setFont (juce::Font (12.0f, juce::Font::bold));
                    
                                                                        // Calculate number position on narrow base
                                    int numY, numWidth = 30;
                                                                        // 1-character margin offset (approx 9px for 12pt bold font)
                                    constexpr int charMargin = 9;
                                    // Pad groups for margin direction
                                    // LEFT margin  (shift num LEFT by 9px):  0, 1, 3, 5, 10, 12, 14
                                    // RIGHT margin (shift num RIGHT by 9px): 2, 4, 6, 9, 11, 13, 15
                                    bool marginLeft  = (padIndex == 0 || padIndex == 1 || padIndex == 3 || padIndex == 5 ||
                                                        padIndex == 10 || padIndex == 12 || padIndex == 14);
                                    bool marginRight = (padIndex == 2 || padIndex == 4 || padIndex == 6 || padIndex == 9 ||
                                                        padIndex == 11 || padIndex == 13 || padIndex == 15);
                    
                                    if (direction == Direction::Down)
                                    {
                                        // Top row (Down trapezoids): narrow base at BOTTOM
                                        numY = (int)(b.getBottom() - 16);
                        
                                        if (isLeftEdge)
                                        {
                                            // Pad 0: half trapezoid, narrow on right side
                                            int drawX = (int)(narrowX + narrowW - numWidth/2);
                                            if (marginRight) drawX += charMargin;
                                            else if (marginLeft) drawX -= charMargin;
                                            g.drawText (juce::String (padIndex + 1),
                                                        drawX,
                                                        numY, numWidth, 14,
                                                        juce::Justification::centred, true);
                                        }
                                        else if (isRightEdge)
                                        {
                                            // Pad 7: half trapezoid, narrow on left side
                                            int drawX = (int)(narrowX - numWidth/2);
                                            if (marginRight) drawX += charMargin;
                                            else if (marginLeft) drawX -= charMargin;
                                            g.drawText (juce::String (padIndex + 1),
                                                        drawX,
                                                        numY, numWidth, 14,
                                                        juce::Justification::centred, true);
                                        }
                                        else
                                        {
                                            // Regular trapezoid: narrow side at bottom left
                                            int drawX = (int)(narrowX - numWidth/2);
                                            if (marginRight) drawX += charMargin;
                                            else if (marginLeft) drawX -= charMargin;
                                            g.drawText (juce::String (padIndex + 1),
                                                        drawX,
                                                        numY, numWidth, 14,
                                                        juce::Justification::centred, true);
                                        }
                                    }
                                    else // Up direction
                                    {
                                        // Bottom row (Up trapezoids): narrow base at TOP
                                        numY = 2;
                        
                                        if (isLeftEdge)
                                        {
                                            // Pad 8: half trapezoid, narrow on left side
                                            int drawX = (int)(narrowX - numWidth/2);
                                            if (marginRight) drawX += charMargin;
                                            else if (marginLeft) drawX -= charMargin;
                                            g.drawText (juce::String (padIndex + 1),
                                                        drawX,
                                                        numY, numWidth, 14,
                                                        juce::Justification::centred, true);
                                        }
                                        else if (isRightEdge)
                                        {
                                            // Pad 15: half trapezoid, narrow on right side
                                            int drawX = (int)(narrowX + narrowW - numWidth/2);
                                            if (marginRight) drawX += charMargin;
                                            else if (marginLeft) drawX -= charMargin;
                                            g.drawText (juce::String (padIndex + 1),
                                                        drawX,
                                                        numY, numWidth, 14,
                                                        juce::Justification::centred, true);
                                        }
                                        else
                                        {
                                            // Regular trapezoid: narrow side at top right
                                            int drawX = (int)(narrowX + narrowW - numWidth/2);
                                            if (marginRight) drawX += charMargin;
                                            else if (marginLeft) drawX -= charMargin;
                                            g.drawText (juce::String (padIndex + 1),
                                                        drawX,
                                                        numY, numWidth, 14,
                                                        juce::Justification::centred, true);
                                        }
                                    }
                                    g.restoreState();
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
            bool isDown = firstPadIsDown ? (i % 2 == 0) : (i % 2 != 0);
            auto dir    = isDown ? TrianglePad::Direction::Down
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
        bankLabel.setFont (juce::Font (11.0f, juce::Font::bold));
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
        auto left = area.removeFromLeft (144);
        bankDownBtn.setBounds (left.removeFromLeft (36).withSizeKeepingCentre (32, 20));
        left.removeFromLeft (4);
        bankLabel.setBounds   (left.removeFromLeft (60).withSizeKeepingCentre (56, 20));
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

    // Override the label text (e.g. "GJM 3/16" or "No GJM").
    void setBankLabelText (const juce::String& text)
    {
        bankLabel.setText (text, juce::dontSendNotification);
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
    int  currentBank = 1;
    int  maxBank     = 1;   // set to GjmManager::kNumBanks when a GJM is loaded

    void updateBank()
    {
        bankLabel.setText ("Bank " + juce::String (currentBank),
                           juce::dontSendNotification);
        bankDownBtn.setEnabled (currentBank > 1);
        bankUpBtn.setEnabled   (currentBank < maxBank);
    }
};