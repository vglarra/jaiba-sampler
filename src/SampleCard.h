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
          thumbnail(512, formatManager, thumbnailCache),
          resampledThumbnail(512, formatManager, thumbnailCache)  // Add second thumbnail for resampled view
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
        
        // Configure waveform area with viewport
        waveformComponent = std::make_unique<WaveformComponent>(thumbnail, resampledThumbnail, pitchOffset);
        
        // Create a container for the waveform that can be larger than the viewport
        waveformContainer = std::make_unique<juce::Component>();
        waveformContainer->addAndMakeVisible(waveformComponent.get());
        
        // Set up the viewport
        waveformViewport.setViewedComponent(waveformContainer.get(), false);
        waveformViewport.setScrollBarsShown(true, false); // Show vertical scroll bar? false, show horizontal? true
        waveformViewport.setScrollOnDragEnabled(true);
        addAndMakeVisible(waveformViewport);
        
        // Set up fixed info labels
        topInfoLabel.setJustificationType(juce::Justification::centred);
        topInfoLabel.setFont(juce::Font(10.0f, juce::Font::bold));
        topInfoLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        topInfoLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(topInfoLabel);
        
        bottomInfoLabel.setJustificationType(juce::Justification::centred);
        bottomInfoLabel.setFont(juce::Font(12.0f, juce::Font::bold));
        bottomInfoLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
        bottomInfoLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(bottomInfoLabel);
        
    // Configure MIDI Note display (no +/- buttons, will add Learn button)
    midiNoteLabel.setJustificationType(juce::Justification::centred);
    midiNoteLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    midiNoteLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    midiNoteLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF3A3A3A));
    updateMidiNoteDisplay();
    addAndMakeVisible(midiNoteLabel);
        
        // Configure MIDI Channel controls
        channelDownButton.setButtonText("-");
        channelDownButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelDownButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        channelDownButton.onClick = [this] { adjustMidiChannel(-1); };
        channelDownButton.setTooltip("Previous MIDI channel");
        addAndMakeVisible(channelDownButton);
        
        midiChannelLabel.setJustificationType(juce::Justification::centred);
        midiChannelLabel.setFont(juce::Font(16.0f, juce::Font::bold));
        midiChannelLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        midiChannelLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF3A3A3A));
        updateMidiChannelDisplay();
        addAndMakeVisible(midiChannelLabel);
        
        channelUpButton.setButtonText("+");
        channelUpButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelUpButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        channelUpButton.onClick = [this] { adjustMidiChannel(1); };
        channelUpButton.setTooltip("Next MIDI channel");
        addAndMakeVisible(channelUpButton);
        
        // Configure Pitch adjustment controls (similar to channel controls)
        pitchDownButton.setButtonText("Down");
        pitchDownButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        pitchDownButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        pitchDownButton.onClick = [this] { adjustPitchDown(); };
        pitchDownButton.setTooltip("Lower pitch (longer duration)");
        addAndMakeVisible(pitchDownButton);
        
        pitchLabel.setJustificationType(juce::Justification::centred);
        pitchLabel.setFont(juce::Font(16.0f, juce::Font::bold));
        pitchLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
        pitchLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF3A3A3A));
        updatePitchDisplay(pitchOffset);
        addAndMakeVisible(pitchLabel);
        
        pitchUpButton.setButtonText("Up");
        pitchUpButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        pitchUpButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        pitchUpButton.onClick = [this] { adjustPitchUp(); };
        pitchUpButton.setTooltip("Higher pitch (shorter duration)");
        addAndMakeVisible(pitchUpButton);
        
        // Update pitch button labels with tooltips
        updatePitchButtonLabels();
        
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
        
        // Set up thumbnail listeners
        thumbnail.addChangeListener(this);
        resampledThumbnail.addChangeListener(this);
    }
    
    ~SampleCard() override
    {
        thumbnail.removeChangeListener(this);
        resampledThumbnail.removeChangeListener(this);
    }
    
    void resized() override
    {
        auto area = getLocalBounds();
        
        // Add margin around the entire card content (10px on each side)
        area.reduce(10, 10);
        
        // Top row: All controls (height 40px to fit all buttons)
        auto topRow = area.removeFromTop(40);
        
        // + button on left (40px)
        addButton.setBounds(topRow.removeFromLeft(40).reduced(2));
        
        // Leave some space between + button and MIDI controls
        topRow.removeFromLeft(10);
        
        // Learn button (60px)
        learnButton.setBounds(topRow.removeFromLeft(60).reduced(2));
        
        // MIDI Note display (100px)
        midiNoteLabel.setBounds(topRow.removeFromLeft(100).reduced(2));
        
        // Space between note and channel controls
        topRow.removeFromLeft(10);
        
        // MIDI Channel controls (total 120px: 30 + 60 + 30)
        channelDownButton.setBounds(topRow.removeFromLeft(30).reduced(2));
        midiChannelLabel.setBounds(topRow.removeFromLeft(60).reduced(2));
        channelUpButton.setBounds(topRow.removeFromLeft(30).reduced(2));
        
        // Space before Prev/Next buttons
        topRow.removeFromLeft(10);
        
        // Prev/Next buttons on right (total 120px: 60 + 60)
        prevButton.setBounds(topRow.removeFromRight(60).reduced(2));
        nextButton.setBounds(topRow.removeFromRight(60).reduced(2));
        
        // Add 5px margin between top row and waveform
        area.removeFromTop(5);
        
        // Calculate waveform height based on 4cm at 96 DPI (fixed height)
        const int waveformHeight = static_cast<int>(4 * 37.8); // ~151px
        
        // Waveform area with viewport
        auto waveformRect = area.removeFromTop(waveformHeight);
        waveformViewport.setBounds(waveformRect);
        
        // Position fixed info labels within the viewport area
        auto labelArea = waveformRect;
        topInfoLabel.setBounds(labelArea.removeFromTop(20).reduced(2));
        bottomInfoLabel.setBounds(labelArea.removeFromBottom(25).reduced(2));
        
        // Update the waveform container and component size
        updateWaveformSize();
        
        // Add 5px margin between waveform and pitch controls
        area.removeFromTop(5);
        
        // Pitch adjustment controls row (below waveform, left corner)
        auto pitchRow = area.removeFromTop(30);
        
        // Position pitch controls at left corner (similar to channel controls layout)
        const int pitchControlWidth = 180;  // 60 + 60 + 60 (matching Learn button width)
        auto pitchControlArea = pitchRow.withWidth(pitchControlWidth);
        
        pitchDownButton.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
        pitchLabel.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
        pitchUpButton.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
        
        // Add margin before bottom row
        area.removeFromTop(10);
        
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
    int getPitchOffset() const { return pitchOffset; }

    void setPitchOffset(int offset)
    {
        // Constrain to reasonable range: ±48 semitones (4 octaves)
        if (offset >= -48 && offset <= 48 && offset != pitchOffset)
        {
            pitchOffset = offset;
            updatePitchDisplay(pitchOffset);
            
            printf("Pitch offset set to: %+d semitones\n", pitchOffset);
        }
    }

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
        originalDuration = seconds;
        // Apply current pitch factor to the displayed duration
        double adjustedDuration = seconds / currentPitchFactor;
        durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
    }
    
    void setPitchFactor(double semitones)
    {
        // Convert semitones to pitch factor (2^(semitones/12))
        currentPitchFactor = std::pow(2.0, semitones / 12.0);
        
        // Update duration display based on pitch factor
        // Pitch up = shorter duration, Pitch down = longer duration
        if (originalDuration > 0.0)
        {
            double adjustedDuration = originalDuration / currentPitchFactor;
            durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
        }
        
        // Trigger a repaint to show the "stretched" waveform visually
        // Note: We can't actually stretch the thumbnail, but we can indicate pitch change
        // by changing the waveform color or adding an overlay
        repaint();
        if (waveformComponent != nullptr)
            waveformComponent->repaint();
    }
    
    void updatePitchDisplay(int semitones)
    {
        // Update the pitch label
        juce::String displayText;
        if (semitones == 0)
            displayText = "0";
        else if (semitones > 0)
            displayText = "+" + juce::String(semitones);
        else
            displayText = juce::String(semitones);
        
        pitchLabel.setText(displayText + " st", juce::dontSendNotification);
        
        // Calculate pitch factor correctly:
        // Positive semitones = pitch up = shorter duration = divide by factor
        // Negative semitones = pitch down = longer duration = divide by factor
        double pitchFactor = std::pow(2.0, semitones / 12.0);
        
        // Store the current pitch factor for duration calculations
        currentPitchFactor = pitchFactor;
        
        if (waveformComponent != nullptr)
            waveformComponent->setPitchFactor(pitchFactor, semitones);
        
        // Update duration display - CORRECTED FORMULA
        if (originalDuration > 0)
        {
            // When pitch goes UP (positive semitones), duration gets SHORTER
            // So we divide by pitchFactor (which is > 1 for positive semitones)
            double adjustedDuration = originalDuration / pitchFactor;
            durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
            
            printf("Pitch: %+d, Factor: %.3f, Original: %.2f, Adjusted: %.2f\n", 
                   semitones, pitchFactor, originalDuration, adjustedDuration);
        }
        
        // Update the fixed info labels
        if (semitones > 0)
        {
            double compressionFactor = 1.0 / pitchFactor;
            topInfoLabel.setText(juce::String("Compressed: ") + juce::String(compressionFactor * 100, 1) + "%", 
                                juce::dontSendNotification);
            bottomInfoLabel.setText("PITCH: +" + juce::String(semitones) + " (↑ " + 
                                   juce::String(pitchFactor, 2) + "x)", juce::dontSendNotification);
        }
        else if (semitones < 0)
        {
            double stretchFactor = 1.0 / pitchFactor;
            topInfoLabel.setText(juce::String("Stretched: ") + juce::String(stretchFactor * 100, 1) + "%", 
                                juce::dontSendNotification);
            bottomInfoLabel.setText("PITCH: " + juce::String(semitones) + " (↓ " + 
                                   juce::String(1.0/pitchFactor, 2) + "x)", juce::dontSendNotification);
        }
        else
        {
            topInfoLabel.setText("", juce::dontSendNotification);
            bottomInfoLabel.setText("", juce::dontSendNotification);
        }
        
        // Update waveform container size after pitch change
        updateWaveformSize();
    }
    
    void setWaveform(const juce::File& audioFile)
    {
        // Clear existing thumbnails
        thumbnail.clear();
        resampledThumbnail.clear();
        
        // Store the original file for resampling
        currentAudioFile = audioFile;
        
        // Create new thumbnail from file
        if (audioFile.existsAsFile())
        {
            // Create a reader for the file
            std::unique_ptr<juce::AudioFormatReader> reader(
                formatManager.createReaderFor(audioFile));
            
            if (reader != nullptr)
            {
                // Store original length and sample rate for resampling calculations
                originalLengthInSamples = reader->lengthInSamples;
                originalSampleRate = reader->sampleRate;
                
                // Set the thumbnail source
                thumbnail.setSource(new juce::FileInputSource(audioFile));
                
                // Update resampled waveform based on current pitch
                updateResampledWaveform();
                
                // Update waveform container size
                updateWaveformSize();
                
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
        virtual void pitchOffsetChanged(int pitchOffset) = 0;
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
        WaveformComponent(juce::AudioThumbnail& thumb, juce::AudioThumbnail& resampledThumb, int& pitchOffsetRef)
            : thumbnail(thumb), resampledThumbnail(resampledThumb), pitchOffset(pitchOffsetRef)
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
            auto waveformBounds = bounds.reduced(2);
            
            if (pitchOffset == 0)
            {
                // No pitch change - draw normal waveform
                g.setColour(juce::Colours::cyan);
                thumbnail.drawChannels(g, waveformBounds, 0.0, thumbnail.getTotalLength(), 1.0f);
            }
            else
            {
                // Calculate the ACTUAL pitch factor for accurate visual scaling
                double pitchFactor = std::pow(2.0, pitchOffset / 12.0);
                
                // First, draw the ORIGINAL waveform in the background (light color)
                g.setColour(juce::Colours::grey.withAlpha(0.3f));
                thumbnail.drawChannels(g, waveformBounds, 0.0, thumbnail.getTotalLength(), 1.0f);
                
                if (pitchOffset > 0)
                {
                    // HIGHER PITCH (Up button) - waveform should appear COMPRESSED
                    double compressionFactor = 1.0 / pitchFactor;
                    
                    // Save graphics state before transform
                    juce::Graphics::ScopedSaveState saveState(g);
                    
                    // Apply horizontal compression from the LEFT edge
                    g.addTransform(juce::AffineTransform::scale((float)compressionFactor, 1.0f, 
                                                               (float)waveformBounds.getX(), (float)waveformBounds.getY()));
                    
                    // Draw COMPRESSED waveform on top
                    g.setColour(juce::Colours::orange);
                    thumbnail.drawChannels(g, waveformBounds, 0.0, thumbnail.getTotalLength(), 1.0f);
                }
                else // pitchOffset < 0
                {
                    // LOWER PITCH (Down button) - waveform should appear STRETCHED
                    double stretchFactor = 1.0 / pitchFactor;  // This will be > 1
                    double visiblePortion = 1.0 / stretchFactor;
                    
                    // Save graphics state before transform
                    juce::Graphics::ScopedSaveState saveState(g);
                    
                    // Apply horizontal stretching from the LEFT edge
                    g.addTransform(juce::AffineTransform::scale((float)stretchFactor, 1.0f,
                                                               (float)waveformBounds.getX(), (float)waveformBounds.getY()));
                    
                    // Draw STRETCHED waveform on top (only visible portion)
                    g.setColour(juce::Colours::orange);
                    thumbnail.drawChannels(g, waveformBounds, 0.0, 
                                          thumbnail.getTotalLength() * visiblePortion, 1.0f);
                }
            }
        }
        else
        {
            // Draw placeholder
            g.setColour(juce::Colours::darkgrey);
            g.setFont(juce::Font(14.0f, juce::Font::italic));
            g.drawText("No waveform", bounds, juce::Justification::centred, true);
        }
    }
        
        // Update setPitchFactor to also store the semitone value
        void setPitchFactor(double factor, int semitones)
        {
            currentPitchFactor = factor;
            currentSemitones = semitones;
            repaint();
        }
        
        // Add a method to trigger repaint when pitch changes
        void updatePitch()
        {
            repaint();
        }
        
    private:
        juce::AudioThumbnail& thumbnail;
        juce::AudioThumbnail& resampledThumbnail;
        int& pitchOffset;
        double currentPitchFactor = 1.0;
        int currentSemitones = 0;
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
    
    // Helper method to update pitch button tooltips
    void updatePitchButtonLabels()
    {
        // Update button tooltips to reflect the new behavior
        pitchDownButton.setTooltip("Lower pitch (longer duration)");
        pitchUpButton.setTooltip("Higher pitch (shorter duration)");
    }
    
    void adjustPitchUp()
    {
        // Up button = Higher pitch = POSITIVE semitones
        int newOffset = pitchOffset + 1;
        
        // Constrain to reasonable range: ±48 semitones (4 octaves)
        if (newOffset >= -48 && newOffset <= 48)
        {
            pitchOffset = newOffset;
            updatePitchDisplay(pitchOffset);
            
            // Notify listeners of pitch offset change
            listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });
            
            if (pitchOffset > 0)
                printf("Pitch UP: %+d semitones (higher pitch, shorter duration)\n", pitchOffset);
            else if (pitchOffset < 0)
                printf("Pitch UP: %+d semitones (less low pitch, shorter duration)\n", pitchOffset);
            else
                printf("Pitch reset to 0\n");
        }
    }

    void adjustPitchDown()
    {
        // Down button = Lower pitch = NEGATIVE semitones
        int newOffset = pitchOffset - 1;
        
        // Constrain to reasonable range: ±48 semitones (4 octaves)
        if (newOffset >= -48 && newOffset <= 48)
        {
            pitchOffset = newOffset;
            updatePitchDisplay(pitchOffset);
            
            // Notify listeners of pitch offset change
            listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });
            
            if (pitchOffset < 0)
                printf("Pitch DOWN: %+d semitones (lower pitch, longer duration)\n", pitchOffset);
            else if (pitchOffset > 0)
                printf("Pitch DOWN: %+d semitones (less high pitch, longer duration)\n", pitchOffset);
            else
                printf("Pitch reset to 0\n");
        }
    }
    
    void updateResampledWaveform()
    {
        if (!currentAudioFile.existsAsFile())
            return;
            
        // Clear the resampled thumbnail
        resampledThumbnail.clear();
        
        // Create a reader for the file
        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(currentAudioFile));
        
        if (reader != nullptr)
        {
            // Calculate pitch ratio (2^(semitones/12))
            double pitchRatio = std::pow(2.0, pitchOffset / 12.0);
            
            // For now, we'll just update the duration display and trigger a repaint
            // Actual resampling would require creating a resampled AudioFormatReader
            // and setting it as the source for resampledThumbnail
            
            // Update duration display
            double adjustedDuration = (originalLengthInSamples / originalSampleRate) / pitchRatio;
            setDuration(adjustedDuration);
            
            printf("Updated resampled waveform: pitch ratio %.3f, adjusted duration %.3f s\n", 
                   pitchRatio, adjustedDuration);
        }
        
        // Trigger repaint
        repaint();
        if (waveformComponent != nullptr)
            waveformComponent->repaint();
    }
    
    void updateWaveformSize()
    {
        if (waveformComponent == nullptr || waveformContainer == nullptr)
            return;
        
        auto viewportBounds = waveformViewport.getLocalBounds();
        int containerWidth = viewportBounds.getWidth();
        
        // If pitch is applied, make the container wider to show the stretched/compressed waveform
        if (pitchOffset != 0)
        {
            double pitchFactor = std::pow(2.0, pitchOffset / 12.0);
            
            if (pitchOffset > 0)
            {
                // For compressed waveform (higher pitch), we can keep original width
                // or make it slightly smaller - here we keep original
                containerWidth = viewportBounds.getWidth();
            }
            else
            {
                // For stretched waveform (lower pitch), we need more width
                // Stretch factor = 1 / pitchFactor (since pitchFactor < 1)
                double stretchFactor = 1.0 / pitchFactor;
                containerWidth = (int)(viewportBounds.getWidth() * stretchFactor);
            }
        }
        
        // Ensure minimum width
        containerWidth = juce::jmax(containerWidth, viewportBounds.getWidth());
        
        // Set container size
        waveformContainer->setBounds(0, 0, containerWidth, viewportBounds.getHeight());
        
        // Set waveform component to fill the container
        waveformComponent->setBounds(waveformContainer->getLocalBounds().reduced(2));
        
        // Reset scroll position to start when waveform changes significantly
        // Comment this out if you want to maintain scroll position
        waveformViewport.setViewPosition(0, 0);
    }
    
    // UI Components
    juce::TextButton addButton{"+"};
    juce::TextButton prevButton{"Prev"};
    juce::TextButton nextButton{"Next"};
    juce::TextButton learnButton{"Learn"};  // MIDI Learn button
    juce::Viewport waveformViewport;
    std::unique_ptr<juce::Component> waveformContainer;
    std::unique_ptr<WaveformComponent> waveformComponent;
    
    // Fixed info labels for waveform display (don't scroll with viewport)
    juce::Label topInfoLabel;      // For compression/stretching percentage
    juce::Label bottomInfoLabel;   // For pitch information
    
    // MIDI Note controls
    juce::Label midiNoteLabel;
    int currentMidiNote = 60;  // Default to Middle C
    
    // MIDI Channel controls
    juce::TextButton channelDownButton{"-"};
    juce::Label midiChannelLabel;
    juce::TextButton channelUpButton{"+"};
    int currentMidiChannel = 1;  // Default to channel 1
    
    juce::Label sampleNameLabel;
    juce::Label durationLabel;
    
    // Pitch adjustment controls
    juce::TextButton pitchDownButton{"Down"};  // Lower pitch = negative semitones = longer duration
    juce::Label pitchLabel;
    juce::TextButton pitchUpButton{"Up"};      // Higher pitch = positive semitones = shorter duration
    int pitchOffset = 0;  // Pitch offset in semitones
    
    // Pitch factor tracking for visual feedback
    double currentPitchFactor = 1.0;  // 1.0 = no pitch change
    double originalDuration = 0.0;     // Store original duration
    
    // MIDI Learn state
    bool isLearning = false;
    
    // Audio components
    juce::AudioFormatManager& formatManager;
    
    // Waveform components
    juce::AudioThumbnailCache thumbnailCache;
    juce::AudioThumbnail thumbnail;
    juce::AudioThumbnail resampledThumbnail;  // For resampled waveform display
    
    // Audio file info for resampling
    juce::File currentAudioFile;
    juce::int64 originalLengthInSamples = 0;
    double originalSampleRate = 0.0;
    
    // Listener list
    juce::ListenerList<Listener> listeners;
};


