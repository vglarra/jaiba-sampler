#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class SampleCard : public juce::Component
{
public:
    SampleCard()
    {
        // Configure + button
        addButton.setButtonText("+");
        addButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        addButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible(addButton);
        
        // Configure Prev button
        prevButton.setButtonText("Prev");
        prevButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        prevButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible(prevButton);
        
        // Configure Next button
        nextButton.setButtonText("Next");
        nextButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        nextButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible(nextButton);
        
        // Configure sample view area (this will be the waveform display area)
        sampleViewArea.setOpaque(true);
        sampleViewArea.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF3A3A3A));
        addAndMakeVisible(sampleViewArea);
        
        // Configure sample name label
        sampleNameLabel.setJustificationType(juce::Justification::centredLeft);
        sampleNameLabel.setFont(juce::Font(14.0f, juce::Font::bold));
        sampleNameLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
        addAndMakeVisible(sampleNameLabel);
        
        // Configure duration label
        durationLabel.setJustificationType(juce::Justification::centredRight);
        durationLabel.setFont(juce::Font(12.0f));
        durationLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(durationLabel);
    }
    
    void resized() override
    {
        auto area = getLocalBounds();
        
        // Add margin around the entire card content (10px on each side)
        area.reduce(10, 10);
        
        // Top row: + button and Prev/Next buttons
        auto topRow = area.removeFromTop(30);
        
        // + button on left
        addButton.setBounds(topRow.removeFromLeft(40).reduced(2));
        
        // Prev/Next buttons on right
        auto navArea = topRow.removeFromRight(140);
        prevButton.setBounds(navArea.removeFromLeft(60).reduced(2));
        nextButton.setBounds(navArea.removeFromLeft(60).reduced(2));
        
        // Add 5px margin between buttons and sample view
        area.removeFromTop(5);
        
        // Calculate sample view height based on 4cm at 96 DPI (fixed height)
        const int sampleViewHeight = static_cast<int>(4 * 37.8); // ~151px
        
        // Sample view area width = card width minus margins (already reduced)
        // Use the full width of the remaining area with 10px margin on each side
        auto sampleViewRect = area.removeFromTop(sampleViewHeight);
        
        // Add a small margin on the sides (10px) for the sample view
        sampleViewRect.reduce(10, 0);
        sampleViewArea.setBounds(sampleViewRect);
        
        // Add 5px margin between sample view and bottom row
        area.removeFromTop(5);
        
        // Bottom row: sample name and duration (takes remaining space)
        auto bottomRow = area.removeFromBottom(25);
        
        // Sample name on left
        sampleNameLabel.setBounds(bottomRow.removeFromLeft(bottomRow.getWidth() / 2).reduced(5, 0));
        
        // Duration on right
        durationLabel.setBounds(bottomRow.reduced(5, 0));
    }
    
    void paint(juce::Graphics& g) override
    {
        // Draw card background
        g.setColour(juce::Colour(0xFF2A2A2A));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        
        // Draw card border
        g.setColour(juce::Colour(0xFF4A4A4A));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 8.0f, 1.5f);
        
        // Draw sample view area border
        g.setColour(juce::Colours::lightgrey);
        g.drawRect(sampleViewArea.getBounds(), 2);
        
        // Draw inner margin guides (optional - for debugging, can remove later)
        g.setColour(juce::Colours::darkgrey.withAlpha(0.3f));
        g.drawRect(getLocalBounds().reduced(10), 1);
        
        // Draw "Sample view" placeholder text if no sample loaded
        if (sampleNameLabel.getText().isEmpty() || sampleNameLabel.getText() == "No sample loaded")
        {
            g.setColour(juce::Colours::darkgrey);
            g.setFont(juce::Font(14.0f, juce::Font::italic));
            g.drawText("Sample view", sampleViewArea.getBounds(), 
                      juce::Justification::centred, true);
        }
    }
    
    // Public methods to access buttons and update content
    juce::TextButton& getAddButton() { return addButton; }
    juce::TextButton& getPrevButton() { return prevButton; }
    juce::TextButton& getNextButton() { return nextButton; }
    
    void setSampleName(const juce::String& name)
    {
        sampleNameLabel.setText(name, juce::dontSendNotification);
        repaint();
    }
    
    void setDuration(double seconds)
    {
        durationLabel.setText(juce::String(seconds, 2) + " s", juce::dontSendNotification);
    }
    
    juce::Rectangle<int> getSampleViewArea() const { return sampleViewArea.getBounds(); }

private:
    juce::TextButton addButton{"+"};
    juce::TextButton prevButton{"Prev"};
    juce::TextButton nextButton{"Next"};
    juce::Label sampleViewArea;
    juce::Label sampleNameLabel;
    juce::Label durationLabel;
};
