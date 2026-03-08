#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_basics/juce_audio_basics.h>

class SampleCard : public juce::Component
{
public:
    SampleCard(juce::AudioFormatManager& formatManager)
        : formatManager(formatManager)
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
        waveformComponent = std::make_unique<WaveformComponent>(
            formatManager, currentAudioFile, pitchOffset);
        
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
        
    }
    
    ~SampleCard() override
    {
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
        // Update the pitch label – invert the sign for display
        int displaySemitones = -semitones;
        juce::String displayText;
        if (displaySemitones == 0)
            displayText = "0";
        else if (displaySemitones > 0)
            displayText = "+" + juce::String(displaySemitones);
        else
            displayText = juce::String(displaySemitones);

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
        // Store the original file
        currentAudioFile = audioFile;
        
        if (audioFile.existsAsFile())
        {
            // Create a reader to get audio info
            std::unique_ptr<juce::AudioFormatReader> reader(
                formatManager.createReaderFor(audioFile));
            
            if (reader != nullptr)
            {
                // Store original info for duration calculations
                originalLengthInSamples = reader->lengthInSamples;
                originalSampleRate = reader->sampleRate;
                
                // Update waveform container size
                updateWaveformSize();
                
                // Force a repaint
                repaint();
                if (waveformComponent != nullptr)
                    waveformComponent->repaint();
                    
                printf("Waveform set for: %s (sample rate: %.1f kHz, length: %lld samples)\n", 
                       audioFile.getFileName().toRawUTF8(),
                       originalSampleRate / 1000.0,
                       originalLengthInSamples);
            }
        }
        else
        {
            // Clear audio info if file doesn't exist
            originalLengthInSamples = 0;
            originalSampleRate = 0.0;
            
            // Force a repaint
            repaint();
            if (waveformComponent != nullptr)
                waveformComponent->repaint();
        }
    }
    
    void clearWaveform()
    {
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
        // High-resolution WaveformComponent that renders directly from audio data
        class WaveformComponent : public juce::Component,
                                private juce::Timer
        {
        public:
            WaveformComponent(juce::AudioFormatManager& formatManager,
                            juce::File& currentAudioFile,
                            int& pitchOffsetRef)
                : formatManager(formatManager),
                currentAudioFile(currentAudioFile),
                pitchOffset(pitchOffsetRef)
            {
                setOpaque(true);
                startTimer(100); // Check for file/pitch changes every 100ms
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

                if (!currentAudioFile.existsAsFile())
                {
                    g.setColour(juce::Colours::darkgrey);
                    g.setFont(juce::Font(14.0f, juce::Font::italic));
                    g.drawText("No waveform", bounds, juce::Justification::centred, true);
                    return;
                }

                auto waveformBounds = bounds.reduced(2);
                if (waveformBounds.isEmpty())
                    return;

                std::unique_ptr<juce::AudioFormatReader> reader(
                    formatManager.createReaderFor(currentAudioFile));

                if (reader == nullptr)
                    return;

                int renderWidth = waveformBounds.getWidth();
                int renderHeight = waveformBounds.getHeight();
                int renderTop = waveformBounds.getY();
                int renderBottom = waveformBounds.getBottom();
                int renderCenter = waveformBounds.getCentreY();

                double totalLength = reader->lengthInSamples;
                int numChannels = reader->numChannels;

                 // Calculate pitch factor (2^(semitones/12))
                double pitchFactor = std::pow(2.0, pitchOffset / 12.0);

                // visibleSamples = totalLength / pitchFactor
                double visibleSamples = totalLength / pitchFactor;

                //double pitchFactor = std::pow(2.0, -pitchOffset / 12.0);   // note the minus sign
                //double visibleSamples = totalLength * pitchFactor;         // now using multiplication
                
                visibleSamples = juce::jlimit(1.0, totalLength * 4.0, visibleSamples);

                // Always start from beginning
                double startOffset = 0.0;
                double samplesPerPixel = visibleSamples / renderWidth;
                samplesPerPixel = juce::jmax(1.0, samplesPerPixel);

                // Buffer for reading audio data
                int bufferSize = 4096;
                juce::AudioBuffer<float> tempBuffer(numChannels, bufferSize);

                // DEBUG flags - moved to outer scope so they're available everywhere
                bool foundNegative = false;
                bool foundPositive = false;

                // ===== DRAW BACKGROUND (STATIC ORIGINAL WAVEFORM) =====
                // This should ALWAYS be drawn, regardless of pitch, and should NEVER move/resize
                // Use original samples per pixel (totalLength / renderWidth) for static display
                {
                    double origSamplesPerPixel = totalLength / renderWidth;

                    if (numChannels > 1)
                    {
                        // STEREO - Draw channels in separate vertical spaces
                        juce::Path leftOrigPath, rightOrigPath;
                        bool leftStarted = false, rightStarted = false;
                        
                        // Split the height into two equal parts
                        int halfHeight = renderHeight / 2;
                        int leftTop = renderTop;
                        int leftBottom = renderTop + halfHeight;
                        int rightTop = renderTop + halfHeight;
                        int rightBottom = renderBottom;
                        
                        for (int x = 0; x < renderWidth; ++x)
                        {
                            double startSample = x * origSamplesPerPixel;  // Use origSamplesPerPixel for background
                            double endSample = (x + 1) * origSamplesPerPixel;
                            
                            if (startSample >= totalLength) break;
                            
                            endSample = std::min(endSample, totalLength);
                            int numSamples = static_cast<int>(endSample - startSample);
                            if (numSamples <= 0) continue;
                            
                            reader->read(&tempBuffer, 0, numSamples, static_cast<juce::int64>(startSample), true, true);
                            
                            float leftMin = 1.0f, leftMax = -1.0f;
                            float rightMin = 1.0f, rightMax = -1.0f;
                            
                            for (int s = 0; s < numSamples; ++s)
                            {
                                float leftVal = tempBuffer.getSample(0, s);
                                leftMin = std::min(leftMin, leftVal);
                                leftMax = std::max(leftMax, leftVal);
                                
                                if (leftVal < 0) foundNegative = true;
                                if (leftVal > 0) foundPositive = true;
                                
                                float rightVal = tempBuffer.getSample(1, s);
                                rightMin = std::min(rightMin, rightVal);
                                rightMax = std::max(rightMax, rightVal);
                                
                                if (rightVal < 0) foundNegative = true;
                                if (rightVal > 0) foundPositive = true;
                            }
                            
                            float xPos = waveformBounds.getX() + x;
                            
                            // Left channel - top half - FIXED FORMULA
                            float leftCenterY = leftTop + halfHeight * 0.5f;
                            float leftHalfHeight = halfHeight * 0.5f;
                            
                            // CORRECTED: Use + for min (negative values go down), - for max (positive values go up)
                            float leftYMin = leftCenterY + (leftMin * leftHalfHeight);
                            float leftYMax = leftCenterY - (leftMax * leftHalfHeight);
                            float leftYTop = std::min(leftYMin, leftYMax);
                            float leftYBottom = std::max(leftYMin, leftYMax);
                            
                            leftYTop = juce::jlimit(leftTop + 1.0f, leftBottom - 1.0f, leftYTop);
                            leftYBottom = juce::jlimit(leftTop + 1.0f, leftBottom - 1.0f, leftYBottom);
                            
                            if (!leftStarted)
                            {
                                leftOrigPath.startNewSubPath(xPos, leftYTop);
                                leftStarted = true;
                            }
                            leftOrigPath.lineTo(xPos, leftYBottom);
                            
                            // Right channel - bottom half - FIXED FORMULA
                            float rightCenterY = rightTop + halfHeight * 0.5f;
                            float rightHalfHeight = halfHeight * 0.5f;
                            
                            // CORRECTED: Use + for min (negative values go down), - for max (positive values go up)
                            float rightYMin = rightCenterY + (rightMin * rightHalfHeight);
                            float rightYMax = rightCenterY - (rightMax * rightHalfHeight);
                            float rightYTop = std::min(rightYMin, rightYMax);
                            float rightYBottom = std::max(rightYMin, rightYMax);
                            
                            rightYTop = juce::jlimit(rightTop + 1.0f, rightBottom - 1.0f, rightYTop);
                            rightYBottom = juce::jlimit(rightTop + 1.0f, rightBottom - 1.0f, rightYBottom);
                            
                            if (!rightStarted)
                            {
                                rightOrigPath.startNewSubPath(xPos, rightYTop);
                                rightStarted = true;
                            }
                            rightOrigPath.lineTo(xPos, rightYBottom);
                        }
                        
                        // Draw left channel background (grey)
                        g.setColour(juce::Colours::grey.withAlpha(0.15f));
                        g.strokePath(leftOrigPath, juce::PathStrokeType(1.0f));
                        
                        // Draw right channel background (dark grey)
                        g.setColour(juce::Colours::darkgrey.withAlpha(0.15f));
                        g.strokePath(rightOrigPath, juce::PathStrokeType(1.0f));
                    }
                    else
                    {
                        // Mono background
                        juce::Path originalPath;
                        bool pathStarted = false;

                        for (int x = 0; x < renderWidth; ++x)
                        {
                            double startSample = x * origSamplesPerPixel;
                            double endSample = (x + 1) * origSamplesPerPixel;

                            if (startSample >= totalLength) break;

                            endSample = std::min(endSample, totalLength);
                            int numSamples = static_cast<int>(endSample - startSample);
                            if (numSamples <= 0) continue;

                            reader->read(&tempBuffer, 0, numSamples, static_cast<juce::int64>(startSample), true, true);

                            float minVal = 1.0f;
                            float maxVal = -1.0f;

                            for (int s = 0; s < numSamples; ++s)
                            {
                                for (int ch = 0; ch < numChannels; ++ch)
                                {
                                    float val = tempBuffer.getSample(ch, s);
                                    minVal = std::min(minVal, val);
                                    maxVal = std::max(maxVal, val);
                                    if (val < 0) foundNegative = true;
                                    if (val > 0) foundPositive = true;
                                }
                            }

                            float xPos = waveformBounds.getX() + x;
                            float centerY = renderTop + renderHeight * 0.5f;
                            float halfHeight = renderHeight * 0.5f;

                            float yMin = centerY - (minVal * halfHeight);
                            float yMax = centerY - (maxVal * halfHeight);
                            float yTop = std::min(yMin, yMax);
                            float yBottom = std::max(yMin, yMax);

                            yTop = juce::jlimit(renderTop + 1.0f, renderBottom - 1.0f, yTop);
                            yBottom = juce::jlimit(renderTop + 1.0f, renderBottom - 1.0f, yBottom);

                            if (!pathStarted)
                            {
                                originalPath.startNewSubPath(xPos, yTop);
                                pathStarted = true;
                            }
                            originalPath.lineTo(xPos, yBottom);
                        }

                        g.setColour(juce::Colours::grey.withAlpha(0.3f));
                        g.strokePath(originalPath, juce::PathStrokeType(1.0f));
                    }
                }

                // ===== DRAW MAIN WAVEFORM (WITH PITCH ADJUSTMENT) =====
                if (numChannels > 1)
                {
                    // STEREO - Draw channels in separate vertical spaces
                    juce::Path leftPath, rightPath;
                    bool leftStarted = false, rightStarted = false;
                    
                    // Split the height into two equal parts
                    int halfHeight = renderHeight / 2;
                    int leftTop = renderTop;
                    int leftBottom = renderTop + halfHeight;
                    int rightTop = renderTop + halfHeight;
                    int rightBottom = renderBottom;
                    
                    for (int x = 0; x < renderWidth; ++x)
                    {
                        double startSample = startOffset + x * samplesPerPixel;
                        double endSample = startSample + samplesPerPixel;
                        
                        if (startSample >= totalLength) break;
                        
                        endSample = std::min(endSample, totalLength);
                        int numSamples = static_cast<int>(endSample - startSample);
                        if (numSamples <= 0) continue;
                        
                        reader->read(&tempBuffer, 0, numSamples, static_cast<juce::int64>(startSample), true, true);
                        
                        float leftMin = 1.0f, leftMax = -1.0f;
                        float rightMin = 1.0f, rightMax = -1.0f;
                        
                        for (int s = 0; s < numSamples; ++s)
                        {
                            float leftVal = tempBuffer.getSample(0, s);
                            leftMin = std::min(leftMin, leftVal);
                            leftMax = std::max(leftMax, leftVal);
                            
                            if (leftVal < 0) foundNegative = true;
                            if (leftVal > 0) foundPositive = true;
                            
                            float rightVal = tempBuffer.getSample(1, s);
                            rightMin = std::min(rightMin, rightVal);
                            rightMax = std::max(rightMax, rightVal);
                            
                            if (rightVal < 0) foundNegative = true;
                            if (rightVal > 0) foundPositive = true;
                        }
                        
                        float xPos = waveformBounds.getX() + x;
                        
                        // Left channel - use FULL height of top half
                        float leftCenterY = leftTop + halfHeight * 0.5f;
                        float leftHalfHeight = halfHeight * 0.5f;
                        
                        // CORRECTED: Use the full range of the channel's lane
                        // Map -1.0 to bottom of lane, +1.0 to top of lane
                        float leftYMin = leftCenterY + (leftMin * leftHalfHeight);  // Note: + instead of -
                        float leftYMax = leftCenterY - (leftMax * leftHalfHeight);
                        float leftYTop = std::min(leftYMin, leftYMax);
                        float leftYBottom = std::max(leftYMin, leftYMax);
                        
                        leftYTop = juce::jlimit(leftTop + 1.0f, leftBottom - 1.0f, leftYTop);
                        leftYBottom = juce::jlimit(leftTop + 1.0f, leftBottom - 1.0f, leftYBottom);
                        
                        if (!leftStarted)
                        {
                            leftPath.startNewSubPath(xPos, leftYTop);
                            leftStarted = true;
                        }
                        leftPath.lineTo(xPos, leftYBottom);
                        
                        // Right channel - use FULL height of bottom half
                        float rightCenterY = rightTop + halfHeight * 0.5f;
                        float rightHalfHeight = halfHeight * 0.5f;
                        
                        // CORRECTED: Use the full range of the channel's lane
                        float rightYMin = rightCenterY + (rightMin * rightHalfHeight);  // Note: + instead of -
                        float rightYMax = rightCenterY - (rightMax * rightHalfHeight);
                        float rightYTop = std::min(rightYMin, rightYMax);
                        float rightYBottom = std::max(rightYMin, rightYMax);
                        
                        rightYTop = juce::jlimit(rightTop + 1.0f, rightBottom - 1.0f, rightYTop);
                        rightYBottom = juce::jlimit(rightTop + 1.0f, rightBottom - 1.0f, rightYBottom);
                        
                        if (!rightStarted)
                        {
                            rightPath.startNewSubPath(xPos, rightYTop);
                            rightStarted = true;
                        }
                        rightPath.lineTo(xPos, rightYBottom);
                    }
                    
                    // Draw left channel (green) - top half
                    g.setColour(juce::Colours::lightgreen);
                    g.strokePath(leftPath, juce::PathStrokeType(1.0f));
                    
                    // Draw right channel (orange) - bottom half
                    g.setColour(juce::Colours::orange);
                    g.strokePath(rightPath, juce::PathStrokeType(1.0f));
                    
                    // Draw channel separator line
                    g.setColour(juce::Colours::darkgrey.withAlpha(0.5f));
                    g.drawHorizontalLine(renderTop + halfHeight, waveformBounds.getX(), waveformBounds.getRight());
                    
                    // Add channel labels
                    g.setColour(juce::Colours::lightgrey);
                    g.setFont(juce::Font(10.0f));
                    g.drawText("L", waveformBounds.getX() + 5, leftTop + 2, 20, 15, juce::Justification::left);
                    g.drawText("R", waveformBounds.getX() + 5, rightTop + 2, 20, 15, juce::Justification::left);
                }
                else
                {
                    // MONO - draw single waveform
                    juce::Path waveformPath;
                    bool pathStarted = false;

                    for (int x = 0; x < renderWidth; ++x)
                    {
                        double startSample = startOffset + x * samplesPerPixel;
                        double endSample = startSample + samplesPerPixel;

                        if (startSample >= totalLength) break;

                        endSample = std::min(endSample, totalLength);
                        int numSamples = static_cast<int>(endSample - startSample);
                        if (numSamples <= 0) continue;

                        reader->read(&tempBuffer, 0, numSamples, static_cast<juce::int64>(startSample), true, true);

                        float minVal = 1.0f;
                        float maxVal = -1.0f;

                        for (int s = 0; s < numSamples; ++s)
                        {
                            for (int ch = 0; ch < numChannels; ++ch)
                            {
                                float val = tempBuffer.getSample(ch, s);
                                minVal = std::min(minVal, val);
                                maxVal = std::max(maxVal, val);
                                if (val < 0) foundNegative = true;
                                if (val > 0) foundPositive = true;
                            }
                        }

                        float xPos = waveformBounds.getX() + x;
                        float centerY = renderTop + renderHeight * 0.5f;
                        float halfHeight = renderHeight * 0.5f;
                        
                        float yMin = centerY - (minVal * halfHeight);
                        float yMax = centerY - (maxVal * halfHeight);
                        float yTop = std::min(yMin, yMax);
                        float yBottom = std::max(yMin, yMax);

                        yTop = juce::jlimit(renderTop + 1.0f, renderBottom - 1.0f, yTop);
                        yBottom = juce::jlimit(renderTop + 1.0f, renderBottom - 1.0f, yBottom);

                        if (!pathStarted)
                        {
                            waveformPath.startNewSubPath(xPos, yTop);
                            pathStarted = true;
                        }
                        waveformPath.lineTo(xPos, yBottom);
                    }

                    // Set color based on pitch
                    if (pitchOffset > 0)
                        g.setColour(juce::Colours::orange);
                    else if (pitchOffset < 0)
                        g.setColour(juce::Colours::cyan);
                    else
                        g.setColour(juce::Colours::lightgreen);

                    g.strokePath(waveformPath, juce::PathStrokeType(1.5f));
                }

                // Draw center line (always)
                g.setColour(juce::Colours::darkgrey.withAlpha(0.3f));
                g.drawHorizontalLine(renderCenter, waveformBounds.getX(), waveformBounds.getRight());
            }
            
            void setPitchFactor(double factor, int semitones)
            {
                currentPitchFactor = factor;
                currentSemitones = semitones;
                repaint(); // Force immediate repaint on pitch change
            }
            
        private:
            void timerCallback() override
            {
                // Check if we need to regenerate due to file changes
                if (currentAudioFile != lastFile)
                {
                    lastFile = currentAudioFile;
                    repaint();
                }
            }
            
            juce::AudioFormatManager& formatManager;
            juce::File& currentAudioFile;
            int& pitchOffset;
            double currentPitchFactor = 1.0;
            int currentSemitones = 0;
            
            juce::File lastFile;
            
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
        };
    
    
    
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
        int newOffset = pitchOffset - 1;
        
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
        int newOffset = pitchOffset + 1;
        
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
    
    void updateWaveformSize()
    {
        if (waveformComponent == nullptr || waveformContainer == nullptr)
            return;
        
        auto viewportBounds = waveformViewport.getLocalBounds();
        int containerWidth = viewportBounds.getWidth();
        
        if (pitchOffset != 0)
        {


            // double pitchFactor = std::pow(2.0, -pitchOffset / 12.0);
            //containerWidth = (int)(viewportBounds.getWidth() * pitchFactor);

            double pitchFactor = std::pow(2.0, pitchOffset / 12.0);
            containerWidth = (int)(viewportBounds.getWidth() * pitchFactor);
            // containerWidth = (int)(viewportBounds.getWidth() / pitchFactor);

        }
        
        // Ensure minimum width
        containerWidth = juce::jmax(containerWidth, viewportBounds.getWidth());
        
        // Set container size
        waveformContainer->setBounds(0, 0, containerWidth, viewportBounds.getHeight());
        
        // Set waveform component to fill the container
        waveformComponent->setBounds(waveformContainer->getLocalBounds().reduced(2));
        
        // Always anchor to left side
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
    
    // Audio file info for resampling
    juce::File currentAudioFile;
    juce::int64 originalLengthInSamples = 0;
    double originalSampleRate = 0.0;
    
    // Listener list
    juce::ListenerList<Listener> listeners;
};

