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
    }
    
    ~MidiActivityLight()
    {
        stopTimer();
    }
    
    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(2);
        
        // Draw outer circle
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
            // Completely black/dark when off
            g.setColour(juce::Colour(0xFF111111));
            g.fillEllipse(bounds.reduced(2));
        }
    }
    
    void noteOn()
    {
        isActive = true;
        lastActivityTime = juce::Time::getMillisecondCounter();
        juce::MessageManager::callAsync([this] { repaint(); });
    }
    
    void noteOff()
    {
        // Note: We don't turn off immediately on note off
        // The timer will handle turning off after a short delay
        lastActivityTime = juce::Time::getMillisecondCounter();
    }
    
private:
    void timerCallback() override
    {
        // Turn off after 50ms of no activity
        if (isActive && (juce::Time::getMillisecondCounter() - lastActivityTime) > 50)
        {
            isActive = false;
            juce::MessageManager::callAsync([this] { repaint(); });
        }
    }
    
    bool isActive = false;
    juce::uint64 lastActivityTime = 0;
};
