#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// GlobalLoopColumn — 5 persistent loop-pad slots that survive bank switching.
//
// Callbacks:
//   onPadSelected(slotIdx)        — single click: edit in SampleCard
//   onPadTogglePlayback(slotIdx)  — double click: start / stop loop
//
// Slot states:
//   Empty           — no sample  (#2A2A2A)
//   Loaded          — sample in RAM, stopped  (#4A4A4A)
//   Playing         — looping continuously  (#006622)
//   SelectedIdle    — selected in SampleCard, stopped  (#441166)
//   SelectedPlaying — selected + playing  (#006622, purple border)
//==============================================================================

class GlobalLoopColumn : public juce::Component
{
public:
    static constexpr int kNumSlots = 5;

    enum class SlotState { Empty, Loaded, Playing, SelectedIdle, SelectedPlaying };

    std::function<void(int)> onPadSelected;
    std::function<void(int)> onPadTogglePlayback;

    //==========================================================================
    GlobalLoopColumn()
    {
        for (int i = 0; i < kNumSlots; ++i)
        {
            slotStates[i]  = SlotState::Empty;
            flashActive[i] = false;
        }
    }

    //==========================================================================
    void setSlotName (int idx, const juce::String& name)
    {
        if (idx < 0 || idx >= kNumSlots) return;
        slotNames[idx] = name;
        repaint();
    }

    void setSlotState (int idx, SlotState s)
    {
        if (idx < 0 || idx >= kNumSlots) return;
        slotStates[idx] = s;
        repaint();
    }

    SlotState getSlotState (int idx) const
    {
        if (idx < 0 || idx >= kNumSlots) return SlotState::Empty;
        return slotStates[idx];
    }

    // Mark a slot as playing/stopped while honouring the selected state.
    void setSlotPlaying (int idx, bool playing)
    {
        if (idx < 0 || idx >= kNumSlots) return;
        const bool sel = (idx == selectedSlot);
        if (playing)
            slotStates[idx] = sel ? SlotState::SelectedPlaying : SlotState::Playing;
        else
            slotStates[idx] = sel ? SlotState::SelectedIdle
                                  : (slotNames[idx].isNotEmpty() ? SlotState::Loaded
                                                                  : SlotState::Empty);
        repaint();
    }

    bool isSlotPlaying (int idx) const
    {
        if (idx < 0 || idx >= kNumSlots) return false;
        return slotStates[idx] == SlotState::Playing
            || slotStates[idx] == SlotState::SelectedPlaying;
    }

    // Highlight the selected slot; deselect any previously selected one.
    void selectSlot (int idx)
    {
        // Restore previous selection to its non-selected state
        if (selectedSlot >= 0 && selectedSlot < kNumSlots)
        {
            auto& s = slotStates[selectedSlot];
            if (s == SlotState::SelectedIdle)
                s = slotNames[selectedSlot].isNotEmpty() ? SlotState::Loaded : SlotState::Empty;
            else if (s == SlotState::SelectedPlaying)
                s = SlotState::Playing;
        }

        selectedSlot = idx;

        if (idx >= 0 && idx < kNumSlots)
        {
            auto& s = slotStates[idx];
            if (s == SlotState::Playing)
                s = SlotState::SelectedPlaying;
            else
                s = SlotState::SelectedIdle;
        }
        repaint();
    }

    void clearSelection()
    {
        selectSlot (-1);
    }

    int getSelectedSlot() const { return selectedSlot; }

    // Briefly flash white to show MIDI trigger.
    void triggerFlash (int idx)
    {
        if (idx < 0 || idx >= kNumSlots) return;
        flashActive[idx] = true;
        repaint();
        juce::Component::SafePointer<GlobalLoopColumn> sp (this);
        juce::Timer::callAfterDelay (120, [sp, idx]()
        {
            if (sp != nullptr) { sp->flashActive[idx] = false; sp->repaint(); }
        });
    }

    //==========================================================================
    void resized() override
    {
        auto area    = getLocalBounds().reduced (2, 2);
        const int H  = area.getHeight();
        const int step = H / kNumSlots;
        const int slotH = step - 2;

        for (int i = 0; i < kNumSlots; ++i)
        {
            int y = area.getY() + i * step;
            slotBounds[i] = { area.getX(), y, area.getWidth(), juce::jmax (24, slotH) };
        }
    }

    void paint (juce::Graphics& g) override
    {
        // Column background
        g.setColour (juce::Colour (0xFF1A1A1A));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        for (int i = 0; i < kNumSlots; ++i)
        {
            const auto r = slotBounds[i];
            if (r.isEmpty()) continue;

            // Fill
            juce::Colour bg;
            if (flashActive[i])
            {
                bg = juce::Colour (0xFFFFFFFF);
            }
            else
            {
                switch (slotStates[i])
                {
                    case SlotState::Empty:           bg = juce::Colour (0xFF2A2A2A); break;
                    case SlotState::Loaded:          bg = juce::Colour (0xFF4A4A4A); break;
                    case SlotState::Playing:         bg = juce::Colour (0xFF006622); break;
                    case SlotState::SelectedIdle:    bg = juce::Colour (0xFF441166); break;
                    case SlotState::SelectedPlaying: bg = juce::Colour (0xFF006622); break;
                }
            }

            g.setColour (bg);
            g.fillRoundedRectangle (r.toFloat(), 3.0f);

            // Border — purple when selected, near-black otherwise
            const bool selected = (i == selectedSlot);
            g.setColour (selected ? juce::Colour (0xFFCC44FF)
                                  : juce::Colour (0xFF0A0A0A));
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, selected ? 1.5f : 1.0f);

            if (!flashActive[i])
            {
                // Slot label "G1"…"G5"
                g.setColour (juce::Colour (0xFF888888));
                g.setFont (juce::Font (8.5f, juce::Font::bold));
                g.drawText ("G" + juce::String (i + 1),
                            r.reduced (3, 2).removeFromTop (11),
                            juce::Justification::centredLeft);

                // Sample name — truncated to fit
                if (slotNames[i].isNotEmpty())
                {
                    g.setColour (juce::Colour (0xFFCECECE));
                    g.setFont (juce::Font (7.5f));
                    auto nameR = r.reduced (3, 2);
                    nameR.removeFromTop (12);
                    g.drawText (slotNames[i], nameR, juce::Justification::topLeft, true);
                }

                // Green dot = looping
                if (slotStates[i] == SlotState::Playing
                 || slotStates[i] == SlotState::SelectedPlaying)
                {
                    g.setColour (juce::Colour (0xFF00FF88));
                    g.fillEllipse ((float)(r.getRight() - 8),
                                   (float)(r.getY()    + 3), 5.0f, 5.0f);
                }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < kNumSlots; ++i)
        {
            if (!slotBounds[i].contains (e.getPosition())) continue;

            if (e.getNumberOfClicks() >= 2)
            {
                if (onPadTogglePlayback) onPadTogglePlayback (i);
            }
            else
            {
                if (onPadSelected) onPadSelected (i);
            }
            return;
        }
    }

    bool hitTest (int x, int y) override
    {
        for (auto& r : slotBounds)
            if (r.contains (x, y)) return true;
        return false;
    }

private:
    SlotState    slotStates[kNumSlots];
    juce::String slotNames [kNumSlots];
    juce::Rectangle<int> slotBounds[kNumSlots];
    bool         flashActive[kNumSlots];
    int          selectedSlot = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalLoopColumn)
};
