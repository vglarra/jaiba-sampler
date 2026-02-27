#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class MidiActivityLight : public juce::Component,
                          private juce::Timer
{
public:
    MidiActivityLight()
    {
        // Set initial size, but will be resized by parent component
        startTimer(50); // Check every 50ms for activity timeout
        lastActivityTime = 0;
        isActive = false;
    }
    
    ~MidiActivityLight()
    {
        stopTimer();
    }
    
    void paint(juce::Graphics& g) override
    {
        // Draw button-like background (same as Test tone button)
        auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xFF4A4A4A));
        g.fillRoundedRectangle(bounds, 4.0f);
        
        // Draw outer circle (always visible) - centered in the button
        auto circleBounds = bounds.reduced(bounds.getWidth() / 2 - 8, bounds.getHeight() / 2 - 8).withSize(16.0f, 16.0f);
        g.setColour(juce::Colours::darkgrey);
        g.fillEllipse(circleBounds);
        
        // Draw inner circle based on activity
        if (isActive)
        {
            // Bright yellow for MIDI activity
            g.setColour(juce::Colours::yellow);
            g.fillEllipse(circleBounds.reduced(2));
        }
        else
        {
            // Dark/black when off
            g.setColour(juce::Colour(0xFF222222));
            g.fillEllipse(circleBounds.reduced(2));
            
            // Add a subtle outline to show the light is present but off
            g.setColour(juce::Colours::darkgrey.brighter(0.3f));
            g.drawEllipse(circleBounds.reduced(2), 0.5f);
        }
    }
    
    void triggerActivity()
    {
        // Always turn on for note-on events
        isActive = true;
        lastActivityTime = juce::Time::getMillisecondCounter();
        
        // Always repaint on the message thread
        juce::MessageManager::callAsync([this] { repaint(); });
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
    
private:
    void timerCallback() override
    {
        // Check if we've been inactive for more than 50ms
        auto now = juce::Time::getMillisecondCounter();
        auto timeSinceLastActivity = now - lastActivityTime;
        
        // Turn off if inactive and currently on
        if (isActive && timeSinceLastActivity > 50)
        {
            isActive = false;
            juce::MessageManager::callAsync([this] { 
                repaint();
                printf("MIDI Light turned off\n");
            });
        }
    }
    
    bool isActive = false;
    juce::uint64 lastActivityTime = 0;
};
