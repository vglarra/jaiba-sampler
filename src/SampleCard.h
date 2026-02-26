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
        // Configure + button (top left)
        addButton.setButtonText("+");
        addButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        addButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible(addButton);
        
        // Configure Prev button (top right)
        prevButton.setButtonText("Prev");
        prevButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        prevButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible(prevButton);
        
        // Configure Next button (top right)
        nextButton.setButtonText("Next");
        nextButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        nextButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible(nextButton);
        
        // Configure MIDI Learn button
        learnButton.setButtonText("Learn");
        learnButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        learnButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        learnButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::orange);
        learnButton.onClick = [this] { toggleLearnMode(); };
        addAndMakeVisible(learnButton);
        
        // Configure waveform area
        waveformComponent = std::make_unique<WaveformComponent>(thumbnail);
        addAndMakeVisible(waveformComponent.get());
        
    // Configure MIDI Note display (no +/- buttons, will add Learn button)
    midiNoteLabel.setJustificationType(juce::Justification::centred);
    midiNoteLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    midiNoteLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    midiNoteLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF3A3A3A));
    updateMidiNoteDisplay();
    addAndMakeVisible(midiNoteLabel);
        
        // Configure MIDI Channel controls
        channelMinusButton.setButtonText("-");
        channelMinusButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelMinusButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        channelMinusButton.onClick = [this] { adjustMidiChannel(-1); };
        addAndMakeVisible(channelMinusButton);
        
        midiChannelLabel.setJustificationType(juce::Justification::centred);
        midiChannelLabel.setFont(juce::Font(16.0f, juce::Font::bold));
        midiChannelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        midiChannelLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF3A3A3A));
        updateMidiChannelDisplay();
        addAndMakeVisible(midiChannelLabel);
        
        channelPlusButton.setButtonText("+");
        channelPlusButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelPlusButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        channelPlusButton.onClick = [this] { adjustMidiChannel(1); };
        addAndMakeVisible(channelPlusButton);
        
        // Configure sample name label (bottom left)
        sampleNameLabel.setJustificationType(juce::Justification::centredLeft);
        sampleNameLabel.setFont(juce::Font(14.0f, juce::Font::bold));
        sampleNameLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
        addAndMakeVisible(sampleNameLabel);
        
        // Configure duration label (bottom right)
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
        
        // Prev/Next buttons on right (no Learn button here anymore)
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
        
        // Add 5px margin between waveform and MIDI controls
        area.removeFromTop(5);
        
        // MIDI Controls row (Note and Channel side by side)
        auto midiRow = area.removeFromTop(30);
        
        // Split the row into two equal halves
        auto leftHalf = midiRow.removeFromLeft(midiRow.getWidth() / 2);
        auto rightHalf = midiRow;
        
        // LEFT HALF: Learn button + MIDI Note display
        const int leftControlWidth = 160;  // Enough for Learn button + note display
        
        auto learnNoteArea = leftHalf.withWidth(leftControlWidth).withCentre(leftHalf.getCentre());
        
        // Learn button on left (60px)
        learnButton.setBounds(learnNoteArea.removeFromLeft(60).reduced(2));
        
        // Note display on right (100px)
        midiNoteLabel.setBounds(learnNoteArea.removeFromLeft(100).reduced(2));
        
        // RIGHT HALF: MIDI Channel controls (-, display, +)
        const int channelControlWidth = 120;
        auto channelControlArea = rightHalf.withWidth(channelControlWidth).withCentre(rightHalf.getCentre());
        
        // - button on left (30px)
        channelMinusButton.setBounds(channelControlArea.removeFromLeft(30).reduced(2));
        
        // Numeric display in middle (60px)
        midiChannelLabel.setBounds(channelControlArea.removeFromLeft(60).reduced(2));
        
        // + button on right (30px)
        channelPlusButton.setBounds(channelControlArea.removeFromLeft(30).reduced(2));
        
        // Add 5px margin between MIDI controls and bottom row
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
    
    int getMidiNote() const { return currentMidiNote; }
    int getMidiChannel() const { return currentMidiChannel; }
    
    void setMidiNote(int note)
    {
        if (note >= 0 && note <= 127 && note != currentMidiNote)
        {
            currentMidiNote = note;
            updateMidiNoteDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            printf("MIDI note set to: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
    }
    
    void setMidiChannel(int channel)
    {
        if (channel >= 0 && channel <= 16 && channel != currentMidiChannel)
        {
            currentMidiChannel = channel;
            updateMidiChannelDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiChannelChanged(currentMidiChannel); });
            
            printf("MIDI channel set to: %s\n", 
                   currentMidiChannel == 0 ? "All Channels" : juce::String(currentMidiChannel).toRawUTF8());
        }
    }
    
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

    // Add listener for MIDI note changes
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void midiNoteChanged(int newNote) = 0;
        virtual void midiChannelChanged(int newChannel) = 0;
        virtual void learningModeChanged(bool isLearning) = 0;
    };
    
    void addListener(Listener* listener)
    {
        listeners.add(listener);
    }
    
    void removeListener(Listener* listener)
    {
        listeners.remove(listener);
    }
    
    // MIDI Learn public methods
    void setLearningMode(bool learning)
    {
        isLearning = learning;
        learnButton.setToggleState(learning, juce::dontSendNotification);
        if (isLearning)
        {
            learnButton.setButtonText("Learning...");
            listeners.call([this](Listener& l) { l.learningModeChanged(true); });
        }
        else
        {
            learnButton.setButtonText("Learn");
            listeners.call([this](Listener& l) { l.learningModeChanged(false); });
        }
    }
    
    bool isInLearningMode() const { return isLearning; }
    
    void setMidiNoteFromLearn(int note)
    {
        if (isLearning && note != currentMidiNote)
        {
            currentMidiNote = note;
            updateMidiNoteDisplay();
            
            // Notify listeners of the new note
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            // Automatically exit learn mode after setting the note
            if (isLearning)
            {
                isLearning = false;
                learnButton.setToggleState(false, juce::dontSendNotification);
                learnButton.setButtonText("Learn");
                listeners.call([this](Listener& l) { l.learningModeChanged(false); });
            }
            
            printf("MIDI note learned: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
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
                g.setColour(juce::Colours::cyan);
                thumbnail.drawChannels(g, bounds.reduced(2), 0.0, thumbnail.getTotalLength(), 1.0f);
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
        }
    }
    
    void adjustMidiNote(int delta)
    {
        int newNote = currentMidiNote + delta;
        
        // Constrain to valid MIDI range (0-127)
        if (newNote >= 0 && newNote <= 127)
        {
            currentMidiNote = newNote;
            updateMidiNoteDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            printf("MIDI note changed to: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
    }
    
    void adjustMidiChannel(int delta)
    {
        int newChannel = currentMidiChannel + delta;
        
        // Allow channel 0 for "All Channels", then 1-16
        if (delta > 0) // Incrementing
        {
            if (currentMidiChannel == 16)
                newChannel = 0; // Wrap to All Channels
            else if (currentMidiChannel == 0)
                newChannel = 1; // From All to channel 1
        }
        else if (delta < 0) // Decrementing
        {
            if (currentMidiChannel == 1)
                newChannel = 0; // Wrap to All Channels
            else if (currentMidiChannel == 0)
                newChannel = 16; // From All to channel 16
        }
        
        // Ensure valid range
        if (newChannel >= 0 && newChannel <= 16)
        {
            currentMidiChannel = newChannel;
            updateMidiChannelDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiChannelChanged(currentMidiChannel); });
            
            printf("MIDI channel changed to: %s\n", 
                   currentMidiChannel == 0 ? "All Channels" : juce::String(currentMidiChannel).toRawUTF8());
        }
    }
    
    void updateMidiNoteDisplay()
    {
        juce::String noteName = juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true);
        midiNoteLabel.setText(juce::String(currentMidiNote) + " (" + noteName + ")", 
                              juce::dontSendNotification);
    }
    
    void updateMidiChannelDisplay()
    {
        if (currentMidiChannel == 0)
            midiChannelLabel.setText("Ch All", juce::dontSendNotification);
        else
            midiChannelLabel.setText("Ch " + juce::String(currentMidiChannel), 
                                     juce::dontSendNotification);
    }
    
    void toggleLearnMode()
    {
        isLearning = !isLearning;
        learnButton.setToggleState(isLearning, juce::dontSendNotification);
        
        if (isLearning)
        {
            learnButton.setButtonText("Learning...");
            // Notify listeners that we're entering learn mode
            listeners.call([this](Listener& l) { l.learningModeChanged(true); });
        }
        else
        {
            learnButton.setButtonText("Learn");
            // Notify listeners that we're exiting learn mode
            listeners.call([this](Listener& l) { l.learningModeChanged(false); });
        }
    }
    
    // UI Components
    juce::TextButton addButton{"+"};
    juce::TextButton prevButton{"Prev"};
    juce::TextButton nextButton{"Next"};
    juce::TextButton learnButton{"Learn"};  // MIDI Learn button
    std::unique_ptr<WaveformComponent> waveformComponent;
    
    // MIDI Note controls
    juce::Label midiNoteLabel;
    int currentMidiNote = 60;  // Default to Middle C
    
    // MIDI Channel controls
    juce::TextButton channelMinusButton{"-"};
    juce::Label midiChannelLabel;
    juce::TextButton channelPlusButton{"+"};
    int currentMidiChannel = 1;  // Default to channel 1
    
    juce::Label sampleNameLabel;
    juce::Label durationLabel;
    
    // MIDI Learn state
    bool isLearning = false;
    
    // Audio components
    juce::AudioFormatManager& formatManager;
    
    // Waveform components
    juce::AudioThumbnailCache thumbnailCache;
    juce::AudioThumbnail thumbnail;
    
    // Listener list
    juce::ListenerList<Listener> listeners;
};