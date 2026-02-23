#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

class SampleCard : public juce::Component,
                   private juce::ChangeListener
{
public:
    SampleCard(juce::AudioFormatManager& formatManager)
        : formatManager(formatManager),
          thumbnailCache(5),  // Cache 5 thumbnails
          thumbnail(512, formatManager, thumbnailCache)
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
        
        // Configure waveform area - use a custom component for better control
        waveformComponent = std::make_unique<WaveformComponent>(thumbnail);
        addAndMakeVisible(waveformComponent.get());
        
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
        
        // Set up thumbnail listener
        thumbnail.addChangeListener(this);
    }
    
    ~SampleCard() override
    {
        thumbnail.removeChangeListener(this);
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
        
        // Add 5px margin between buttons and waveform
        area.removeFromTop(5);
        
        // Calculate waveform height based on 4cm at 96 DPI (fixed height)
        const int waveformHeight = static_cast<int>(4 * 37.8); // ~151px
        
        // Waveform area takes full width with margins
        auto waveformRect = area.removeFromTop(waveformHeight);
        waveformRect.reduce(10, 0);
        
        if (waveformComponent != nullptr)
            waveformComponent->setBounds(waveformRect);
        
        // Add 5px margin between waveform and bottom row
        area.removeFromTop(5);
        
        // Bottom row: sample name and duration
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
    }
    
    // Public methods
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
    
    void setWaveform(const juce::File& audioFile)
    {
        // Clear existing thumbnail
        thumbnail.clear();
        
        // Create new thumbnail from file
        if (audioFile.existsAsFile())
        {
            // Create a reader for the file
            std::unique_ptr<juce::AudioFormatReader> reader(
                formatManager.createReaderFor(audioFile));
            
            if (reader != nullptr)
            {
                // Set the thumbnail source
                thumbnail.setSource(new juce::FileInputSource(audioFile));
                
                // Force a repaint
                repaint();
                if (waveformComponent != nullptr)
                    waveformComponent->repaint();
                    
                printf("Waveform set for: %s\n", audioFile.getFileName().toRawUTF8());
            }
        }
    }
    
    void clearWaveform()
    {
        thumbnail.clear();
        repaint();
        if (waveformComponent != nullptr)
            waveformComponent->repaint();
    }
    
    juce::Rectangle<int> getWaveformArea() const 
    { 
        if (waveformComponent != nullptr)
            return waveformComponent->getBounds();
        return juce::Rectangle<int>();
    }

private:
    // Custom component to handle waveform drawing
    class WaveformComponent : public juce::Component
    {
    public:
        WaveformComponent(juce::AudioThumbnail& thumb) : thumbnail(thumb)
        {
            setOpaque(true);
        }
        
        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds();
            
            // Fill background
            g.setColour(juce::Colour(0xFF3A3A3A));
            g.fillRect(bounds);
            
            // Draw border
            g.setColour(juce::Colours::lightgrey);
            g.drawRect(bounds, 2);
            
            // Draw waveform if loaded
            if (thumbnail.getTotalLength() > 0.0)
            {
                // Draw the waveform
                g.setColour(juce::Colours::cyan);  // Bright color for visibility
                thumbnail.drawChannels(g, bounds.reduced(2), 0.0, thumbnail.getTotalLength(), 1.0f);
                
                // Debug text
                g.setColour(juce::Colours::white);
                g.setFont(12.0f);
                g.drawText("Waveform", bounds, juce::Justification::topLeft, true);
            }
            else
            {
                // Draw placeholder
                g.setColour(juce::Colours::darkgrey);
                g.setFont(juce::Font(14.0f, juce::Font::italic));
                g.drawText("No waveform", bounds, juce::Justification::centred, true);
            }
        }
        
    private:
        juce::AudioThumbnail& thumbnail;
    };
    
    // AudioThumbnail listener implementation
    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        if (source == &thumbnail)
        {
            // Thumbnail has been updated, repaint
            repaint();
            if (waveformComponent != nullptr)
                waveformComponent->repaint();
                
            printf("Thumbnail updated - length: %.2f seconds\n", thumbnail.getTotalLength());
        }
    }
    
    // UI Components
    juce::TextButton addButton{"+"};
    juce::TextButton prevButton{"Prev"};
    juce::TextButton nextButton{"Next"};
    std::unique_ptr<WaveformComponent> waveformComponent;
    juce::Label sampleNameLabel;
    juce::Label durationLabel;
    
    // Audio components
    juce::AudioFormatManager& formatManager;
    
    // Waveform components
    juce::AudioThumbnailCache thumbnailCache;
    juce::AudioThumbnail thumbnail;
};