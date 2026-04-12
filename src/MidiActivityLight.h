#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>

class MidiActivityLight : public juce::Component,
                          private juce::Timer
{
public:
    MidiActivityLight()
    {
        setSize(16, 16);
        startTimer(50); // Check every 50ms for activity timeout
        lastActivityTime = 0;
    }
    
    ~MidiActivityLight()
    {
        stopTimer();
    }
    
    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(2);
        
        // Draw outer circle (always visible)
        g.setColour(juce::Colour(0xFF2D2D2D));
        g.fillEllipse(bounds);
        
        // Draw inner circle based on activity
        if (isActive)
        {
            // Bright yellow for note activity
            g.setColour(juce::Colour(0xFF9DC95C));
            g.fillEllipse(bounds.reduced(2));
        }
        else
        {
            // Dark/black when off
            g.setColour(juce::Colour(0xFF1A1A1A));
            g.fillEllipse(bounds.reduced(2));

            // Add a subtle outline to show the light is present but off
            g.setColour(juce::Colour(0xFF3A3A3A));
            g.drawEllipse(bounds.reduced(2), 0.5f);
        }
    }
    
    void noteOn()
    {
        // Always turn on for note-on events
        triggerActivity();
    }
    
    void noteOff()
    {
        // For note-off events, we still want visual feedback
        // but it's often less intense - we'll trigger a brief flash
        triggerActivity();
    }
    
    void triggerActivity()
    {
        // isActive is std::atomic — safe to write from any thread (MIDI callback thread).
        isActive.store(true);
        lastActivityTime.store(juce::Time::getMillisecondCounter());
        juce::MessageManager::callAsync([this] { repaint(); });
    }
    
private:
    void timerCallback() override
    {
        auto now = juce::Time::getMillisecondCounter();
        if (isActive.load() && (now - lastActivityTime.load()) > 50)
        {
            isActive.store(false);
            juce::MessageManager::callAsync([this] { repaint(); });
        }
    }

    // Both written from MIDI callback thread, read from message thread timer — must be atomic.
    std::atomic<bool>          isActive        { false };
    std::atomic<juce::uint64>  lastActivityTime { 0 };
};

