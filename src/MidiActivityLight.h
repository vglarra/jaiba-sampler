#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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
        g.setColour(juce::Colours::darkgrey);
        g.fillEllipse(bounds);
        
        // Draw inner circle based on activity
        if (isActive)
        {
            // Bright yellow for note activity
            g.setColour(juce::Colours::yellow);
            g.fillEllipse(bounds.reduced(2));
        }
        else
        {
            // Dark/black when off
            g.setColour(juce::Colour(0xFF222222));
            g.fillEllipse(bounds.reduced(2));
            
            // Add a subtle outline to show the light is present but off
            g.setColour(juce::Colours::darkgrey.brighter(0.3f));
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
        isActive = true;
        lastActivityTime = juce::Time::getMillisecondCounter();
        
        // Always repaint on the message thread
        juce::MessageManager::callAsync([this] { repaint(); });
    }
    
private:
    void timerCallback() override
    {
        // Check if we've been inactive for more than 50ms
        auto now = juce::Time::getMillisecondCounter();
        auto timeSinceLastActivity = now - lastActivityTime;
        
        // Only turn off if we've been inactive and we're currently on
        if (isActive && timeSinceLastActivity > 50)
        {
            isActive = false;
            juce::MessageManager::callAsync([this] { repaint(); });
        }
    }
    
    bool isActive = false;
    juce::uint64 lastActivityTime = 0;
};

