#include "MainComponent.h"
#include "UIComponents.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <vfw.h>  // Add this for GetOpenFileNamePreviewA
#endif


//==============================================================================
MainComponent::MainComponent()
    : sampleListBox("samples", nullptr),
      cpuTimer(*this)
{
    printf("DEBUG: MainComponent constructor started\n");
    fflush(stdout);
    
    // Create configuration manager for session persistence
    configManager = std::make_unique<ConfigurationManager>();
    
    // Create the model
    sampleListModel = std::make_unique<SampleListModel>(*this);
    sampleListBox.setModel(sampleListModel.get());
    
    setSize(900, 700);
    
    formatManager.registerBasicFormats();
    
    // Add sampler voices with note stealing enabled
    for (int i = 0; i < 16; ++i)
        sampler.addVoice(new juce::SamplerVoice());
    
    // Enable note stealing for better voice management
    sampler.setNoteStealingEnabled(true);
    
    // Configure buttons
    menuButton.setButtonText("Menu");
    menuButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    menuButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible(menuButton);
    menuButton.addListener(this);
    
      testToneButton.setButtonText("Test tone");
      testToneButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
      testToneButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
      addAndMakeVisible(testToneButton);
      testToneButton.addListener(this);

      // Add MIDI activity light to the title area
      addAndMakeVisible(midiActivityLight);
      midiActivityLight.setAlwaysOnTop(true); // Ensure it's visible

      // Add the sample card
      addAndMakeVisible(sampleCard);

      // Add listeners for the card buttons
      sampleCard.getAddButton().addListener(this);
      sampleCard.getPrevButton().addListener(this);
      sampleCard.getNextButton().addListener(this);

      // Add listener for MIDI note changes
      sampleCard.addListener(this);

      // Set initial sample name
      sampleCard.setSampleName("No sample loaded");

      addAndMakeVisible(audioDeviceInfoLabel);
      audioDeviceInfoLabel.setJustificationType(juce::Justification::left);
      audioDeviceInfoLabel.setFont(juce::Font(12.0f));
      audioDeviceInfoLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

      addAndMakeVisible(midiDeviceInfoLabel);
      midiDeviceInfoLabel.setJustificationType(juce::Justification::left);
      midiDeviceInfoLabel.setFont(juce::Font(12.0f));
      midiDeviceInfoLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

      addAndMakeVisible(cpuUsageLabel);
      cpuUsageLabel.setJustificationType(juce::Justification::right);
      cpuUsageLabel.setFont(juce::Font(12.0f, juce::Font::bold));
      cpuUsageLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    
    // Configure sliders
    lowNoteSlider.setRange(0, 127, 1);
    highNoteSlider.setRange(0, 127, 1);
    rootNoteSlider.setRange(0, 127, 1);
    
    lowNoteSlider.addListener(this);
    highNoteSlider.addListener(this);
    rootNoteSlider.addListener(this);
    
    // Set up audio
    setAudioChannels(0, 2);
    
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    setup.bufferSize = 512;
    deviceManager.setAudioDeviceSetup(setup, true);
    
    // Start CPU timer for updates every 500ms
    cpuTimer.startTimer(500);
    
    // Load last session (sample, MIDI settings, directory)
    loadLastSession();
    
    printf("DEBUG: MainComponent constructor completed\n");
    fflush(stdout);
}

MainComponent::~MainComponent()
{
    cpuTimer.stopTimer();
    deviceManager.removeChangeListener(this);
    
    shutdownAudio();
}

//==============================================================================
void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    sampler.setCurrentPlaybackSampleRate(sampleRate);
    midiCollector.reset(sampleRate);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion();
    
    juce::MidiBuffer midiMessages;
    juce::MidiBuffer incomingMidi;
    midiCollector.removeNextBlockOfMessages(incomingMidi, bufferToFill.numSamples);
    midiMessages.addEvents(incomingMidi, 0, bufferToFill.numSamples, 0);
    
    sampler.renderNextBlock(*bufferToFill.buffer, midiMessages, 0, bufferToFill.numSamples);
    
    if (sineWaveActive)
    {
        const double sampleRate = sampler.getSampleRate();
        if (sampleRate > 0)
        {
            const double phaseIncrement = sineWaveFrequency * juce::MathConstants<double>::twoPi / sampleRate;
            
            for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
            {
                float* channelData = bufferToFill.buffer->getWritePointer(channel);
                
                for (int i = 0; i < bufferToFill.numSamples; ++i)
                {
                    channelData[i] += (float)(std::sin(sineWavePhase + i * phaseIncrement) * sineWaveAmplitude);
                }
            }
            
            sineWavePhase += phaseIncrement * bufferToFill.numSamples;
            while (sineWavePhase >= juce::MathConstants<double>::twoPi)
                sineWavePhase -= juce::MathConstants<double>::twoPi;
        }
    }
}

void MainComponent::releaseResources()
{
}

//==============================================================================
void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF2A2A2A));
    
    // Draw title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    
    // Title area matches reduced top bar height (30px)
    auto titleArea = getLocalBounds().reduced(20).removeFromTop(30);
    g.drawText("Jaiva Sampler V001", titleArea, juce::Justification::centred, true);
    
    // Draw separator lines
    g.setColour(juce::Colours::darkgrey);
    
    // Line below title (adjusted for reduced top bar)
    auto lineY = titleArea.getBottom() + 5;
    g.drawHorizontalLine(lineY, 20, getWidth() - 20);
    
    // Line above footer - adjusted for reduced footer height (50px + 20px padding)
    auto footerY = getHeight() - 70;
    g.drawHorizontalLine(footerY, 20, getWidth() - 20);
}

void MainComponent::resized()
{
    // Add a flag to prevent recursive resizing
    static bool isResizing = false;
    if (isResizing) return;
    isResizing = true;
    
    auto area = getLocalBounds().reduced(20);
    
    // Top bar: Menu button on left, Title in center, MIDI light button and Test tone button on right
    // Reduced top bar height by 40% (from 50px to 30px)
    auto topBar = area.removeFromTop(30);
    
    // Menu button on left (60px wide like Prev/Next buttons in SampleCard)
    menuButton.setBounds(topBar.removeFromLeft(60).reduced(2));
    
    // Title area (centered in remaining space)
    auto titleArea = topBar;
    auto titleBounds = titleArea.withSizeKeepingCentre(300, 30);
    
    // Right side: MIDI light button and Test tone button
    auto rightSide = topBar.removeFromRight(60 + 5 + 30); // Test tone (60px) + margin (5px) + MIDI light (30px)
    
    // MIDI light button (30px wide, same height as other buttons)
    auto midiLightButtonArea = rightSide.removeFromLeft(30).reduced(2);
    // Set the MIDI activity light to fill the entire button area
    midiActivityLight.setBounds(midiLightButtonArea);
    
    // 5px margin between buttons
    rightSide.removeFromLeft(5);
    
    // Test tone button on right (60px wide like Prev/Next buttons)
    testToneButton.setBounds(rightSide.removeFromLeft(60).reduced(2));
    
    // Body area - this is where the card goes
    auto bodyArea = area.reduced(10, 5);
    
    // Make the card take most of the body width, but with max width to maintain proportions
    const int cardMaxWidth = 600;  // Maximum width to keep card from getting too wide
    const int cardHeight = 280;     // Fixed height
    
    int cardWidth = (bodyArea.getWidth() - 40 < cardMaxWidth) ? (bodyArea.getWidth() - 40) : cardMaxWidth;
    
    // Ensure card width is positive
    if (cardWidth < 100) cardWidth = 100;
    
    auto cardBounds = bodyArea.withWidth(cardWidth)
                              .withHeight(cardHeight)
                              .withCentre(bodyArea.getCentre());
    
    // Ensure card bounds are valid before setting
    if (cardBounds.getWidth() > 0 && cardBounds.getHeight() > 0)
    {
        sampleCard.setBounds(cardBounds);
    }
    
    // Footer area at bottom - reduced by 40% (from 80px to 48px, using 50px for clean math)
    auto footerArea = getLocalBounds().reduced(20).removeFromBottom(50);
    
    // Left side: Device info - two rows with vertical centering
    // Use all available space before CPU indicator
    auto leftFooter = footerArea.withTrimmedRight(130); // Reserve space for CPU label + padding
    
    // Calculate row height for two rows with vertical centering
    const int rowHeight = 20; // Each row gets 20px
    const int totalRowsHeight = rowHeight * 2;
    const int verticalPadding = (footerArea.getHeight() - totalRowsHeight) / 2;
    
    // Audio info row (top)
    auto audioRow = leftFooter.removeFromTop(rowHeight).translated(0, verticalPadding);
    audioRow.removeFromLeft(5); // Left margin
    audioDeviceInfoLabel.setBounds(audioRow);
    audioDeviceInfoLabel.setFont(juce::Font(10.0f));
    audioDeviceInfoLabel.setJustificationType(juce::Justification::left);
    
    // MIDI info row (bottom)
    auto midiRow = leftFooter.removeFromTop(rowHeight).translated(0, verticalPadding);
    midiRow.removeFromLeft(5); // Left margin
    midiDeviceInfoLabel.setBounds(midiRow);
    midiDeviceInfoLabel.setFont(juce::Font(10.0f));
    midiDeviceInfoLabel.setJustificationType(juce::Justification::left);
    
    // Right side: CPU usage - maintain current positioning
    cpuUsageLabel.setBounds(footerArea.removeFromRight(120).reduced(5));
    cpuUsageLabel.setFont(juce::Font(11.0f, juce::Font::bold));
    cpuUsageLabel.setJustificationType(juce::Justification::right);
    
    isResizing = false;
}

//==============================================================================
void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &menuButton)
    {
        showSettingsMenu();
    }
    else if (button == &testToneButton)
    {
        toggleSineWave();
    }
    else if (button == &sampleCard.getAddButton())
    {
        auto* previewComp = new ::AudioPreviewComponent(formatManager);
        
        // Use last saved directory or default
        juce::File startingDirectory = configManager->getLastDirectory();
        
        fileChooser = std::make_unique<juce::FileChooser>(
            "Select sample",
            startingDirectory,
            "*.wav;*.aiff;*.mp3"
        );
        
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode,
            [this](const juce::FileChooser& fc)
            {
                auto results = fc.getResults();
                if (results.size() > 0)
                {
                    auto file = results[0];
                    
                    // Save the directory for next time
                    configManager->saveLastDirectory(file.getParentDirectory());
                    
                    // Set current folder and scan for audio files
                    currentFolder = file.getParentDirectory();
                    scanCurrentFolderForAudioFiles();
                    
                    // Find and set current file index
                    for (int i = 0; i < folderAudioFiles.size(); ++i)
                    {
                        if (folderAudioFiles[i] == file)
                        {
                            currentFileIndex = i;
                            break;
                        }
                    }
                    
                    // Use the async loading method instead of synchronous loadSampleFile
                    // This ensures proper UI updates and session saving
                    // New sample from Add button - reset pitch to 0
                    loadSampleFileAsync(file, true, 0); // true for auto-play, 0 for pitch offset
                    
                    // DON'T call saveCurrentSession() here - it will be called 
                    // from within loadSampleFileAsync after the sample is fully loaded
                }
            },
            previewComp
        );
    }
    else if (button == &sampleCard.getPrevButton())
    {
        loadPrevSample();
    }
    else if (button == &sampleCard.getNextButton())
    {
        loadNextSample();
    }
}

void MainComponent::toggleSineWave()
{
    sineWaveActive = !sineWaveActive;
    
    if (sineWaveActive)
    {
        sineWavePhase = 0.0;
        testToneButton.setButtonText("Stop tone");
        testToneButton.setColour(juce::TextButton::buttonColourId, juce::Colours::lightcoral);
    }
    else
    {
        testToneButton.setButtonText("Test tone");
        testToneButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    }
    
    // TEMPORARY TEST: Trigger the light manually
    midiActivityLight.triggerActivity();
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        updateDeviceInfo();
    }
}

//==============================================================================
void MainComponent::showAudioDeviceSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(
        deviceManager,
        0, 256, 0, 2, true, true, false, false
    );
    
    selector->setSize(500, 400);
    
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Audio Device Settings";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    options.launchAsync();
    updateDeviceInfo();
}

void MainComponent::updateDeviceInfo()
{
    // Update CPU usage only if changed significantly
    double newCPU = deviceManager.getCpuUsage() * 100.0;
    if (std::abs(newCPU - lastCPU) > 0.01)  // Only update if changed significantly
    {
        cpuUsageLabel.setText(juce::String::formatted("CPU: %.2f%%", newCPU), 
                              juce::dontSendNotification);
        lastCPU = newCPU;
    }
    
    // Only update device info occasionally or when changed
    cpuUpdateCounter++;
    if (cpuUpdateCounter % 10 == 0)  // Every 5 seconds (500ms * 10)
    {
        juce::String info;
        
        if (auto* currentDevice = deviceManager.getCurrentAudioDevice())
        {
            info += "Device: " + currentDevice->getName() + "\n";
            info += "Sample Rate: " + juce::String(currentDevice->getCurrentSampleRate()) + " Hz\n";
            info += "Buffer Size: " + juce::String(currentDevice->getCurrentBufferSizeSamples()) + " samples\n";
            
            auto activeOutputs = currentDevice->getActiveOutputChannels();
            info += "Outputs: " + juce::String(activeOutputs.countNumberOfSetBits()) + " channels\n";
        }
        else
        {
            info = "No audio device selected";
        }
        
        audioDeviceInfoLabel.setText(info, juce::dontSendNotification);
        
        juce::String midiInfo = "MIDI: ";
        if (midiInput != nullptr && !currentMidiDeviceName.isEmpty())
        {
            midiInfo += currentMidiDeviceName + " (Connected)";
        }
        else
        {
            midiInfo += "No device selected";
        }
        
        midiDeviceInfoLabel.setText(midiInfo, juce::dontSendNotification);
        
        // Reset counter to avoid overflow
        if (cpuUpdateCounter >= 1000) cpuUpdateCounter = 0;
    }
}

//==============================================================================
void MainComponent::loadSampleFile(const juce::File& file)
{
    // For single-sample mode, clear existing samples before loading new one
    // This ensures only one sample is active at a time (consistent with Prev/Next navigation)
    {
        juce::ScopedLock lock(sampleLock);
        samples.clear();
        selectedSampleIndex = 0;
    }
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    
    if (reader != nullptr)
    {
        auto* sample = new MappedSample();
        sample->file = file;
        sample->name = file.getFileName();
        
        // IMPORTANT: Use the current MIDI note from the card as the root note
        sample->rootNote = sampleCard.getMidiNote();  // Use card's current note
        
        // For single-note mode, all notes are the same
        sample->lowNote = sample->rootNote;
        sample->highNote = sample->rootNote;
        
        // Cache the audio data in memory
        sample->sampleRate = reader->sampleRate;
        sample->numChannels = reader->numChannels;
        sample->lengthInSamples = reader->lengthInSamples;
        
        auto buffer = std::make_unique<juce::AudioBuffer<float>>(
            (int)reader->numChannels, 
            (int)reader->lengthInSamples
        );
        
        reader->read(buffer.get(), 0, (int)reader->lengthInSamples, 0, true, true);
        sample->audioData = std::move(buffer);
        
        {
            juce::ScopedLock lock(sampleLock);
            samples.add(sample);
        }
        
        updateSamplerSounds();
        sampleCard.setSampleName(file.getFileName());
        sampleCard.setWaveform(file);  // Set the waveform
        
        // Calculate duration
        double durationInSeconds = reader->lengthInSamples / reader->sampleRate;
        sampleCard.setDuration(durationInSeconds);
        
        printf("Sample loaded (single mode): %s -> note %d (%lld samples, %.2f s)\n", 
               file.getFileName().toRawUTF8(), 
               sample->rootNote,
               reader->lengthInSamples, 
               durationInSeconds);
    }
}

//==============================================================================
// Add all other method implementations here (showMidiDeviceSettings, 
// handleIncomingMidiMessage, SampleListModel methods, sliderValueChanged,
// updateMappingUI, showMappingInterface, addSampleToMap, removeSelectedSample,
// clearAllSamples, updateSamplerSounds, etc.)

void MainComponent::updateMidiDeviceList()
{
    midiInputNames.clear();
    
    auto devices = juce::MidiInput::getAvailableDevices();
    
    for (auto& device : devices)
    {
        midiInputNames.add(device.name);
    }
    
    printf("Found %d MIDI input devices\n", devices.size());
    fflush(stdout);
}

void MainComponent::showMidiDeviceSettings()
{
    printf("Opening MIDI device settings...\n");
    fflush(stdout);

    updateMidiDeviceList();

    auto* selector = new ::MidiSelectorComponent(*this, midiInputNames);
    selector->setSize(400, 200);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "MIDI Input Settings";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    // IMPORTANT FIX: Don't try to capture the return value of launchAsync()
    // Instead, let the dialog manage itself and just track it with a weak reference
    options.launchAsync();
    
    // We can't store the dialog pointer here because launchAsync returns immediately
    // and the dialog is created asynchronously. Instead, we'll rely on the dialog
    // to clean itself up.
    
    printf("MIDI settings launched\n");
    fflush(stdout);
}


void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    // Filter out background MIDI messages that shouldn't trigger the light
    // Ignore MIDI clock and active sense messages as they're sent continuously
    if (message.isMidiClock() || message.isActiveSense())
    {
        // Don't print anything - just return immediately
        // These messages are ignored completely to avoid performance issues
        return;
    }
    
      // ALWAYS trigger the MIDI activity light for ANY MIDI message
      // This happens before any filtering so it shows activity from all channels
      midiActivityLight.triggerActivity();
      
      // Update MIDI activity light based on message type for more nuanced feedback
      if (message.isNoteOn())
      {
          midiActivityLight.noteOn();
      }
      else if (message.isNoteOff())
      {
          midiActivityLight.noteOff();
      }
      else
      {
          // For other MIDI messages (controllers, etc.) trigger a brief flash
          midiActivityLight.triggerActivity();
      }
      
      // Print ALL incoming MIDI messages for debugging (temporarily)
      // You can comment these out once everything is working
      if (message.isNoteOn())
      {
          printf("RAW MIDI Note On: %d, Vel: %d, Ch: %d\n", 
                 message.getNoteNumber(), 
                 message.getVelocity(),
                 message.getChannel());
      }
      else if (message.isNoteOff())
      {
          printf("RAW MIDI Note Off: %d, Ch: %d\n", 
                 message.getNoteNumber(),
                 message.getChannel());
      }
      else if (message.isController())
      {
          printf("RAW MIDI Controller: %d, Val: %d, Ch: %d\n",
                 message.getControllerNumber(),
                 message.getControllerValue(),
                 message.getChannel());
      }
      else if (message.isPitchWheel())
      {
          printf("RAW MIDI Pitch Wheel: %d, Ch: %d\n",
                 message.getPitchWheelValue(),
                 message.getChannel());
      }
      else if (message.isAftertouch())
      {
          printf("RAW MIDI Aftertouch: %d, Ch: %d\n",
                 message.getAfterTouchValue(),
                 message.getChannel());
      }
      else if (message.isChannelPressure())
      {
          printf("RAW MIDI Channel Pressure: %d, Ch: %d\n",
                 message.getChannelPressureValue(),
                 message.getChannel());
      }
      else if (message.isSysEx())
      {
          printf("RAW MIDI SysEx: %d bytes\n", message.getRawDataSize());
      }
      else if (message.isMidiStart() || message.isMidiStop() || message.isMidiContinue())
      {
          // These are transport messages - print them but they're less frequent
          if (message.isMidiStart()) printf("RAW MIDI Start\n");
          else if (message.isMidiStop()) printf("RAW MIDI Stop\n");
          else if (message.isMidiContinue()) printf("RAW MIDI Continue\n");
      }
    
    // ANTI-FLOOD PROTECTION: Ignore duplicate messages in quick succession
    static juce::uint64 lastMessageTime = 0;
    static int lastNoteNumber = -1;
    static int lastNoteCount = 0;
    static int totalIgnored = 0;
    
    juce::uint64 currentTime = juce::Time::getMillisecondCounter();
    int timeSinceLast = (int)(currentTime - lastMessageTime);
    
    // If we're in learn mode and get a note on message, handle it specially
    // In learn mode, we ignore channel filtering - learn from any channel
    if (isLearningMode && message.isNoteOn())
    {
        int currentNote = message.getNoteNumber();
        handleMidiLearn(currentNote);
        // Still add to collector so user can hear the note
        midiCollector.addMessageToQueue(message);
        
        printf("🎹 LEARN MODE: Captured note %d from channel %d\n", 
               currentNote, message.getChannel());
        return;
    }
    
    // Get the currently selected MIDI channel from the sample card
    int selectedChannel = sampleCard.getMidiChannel();
    
    // For normal operation, filter by selected channel if not "All Channels" (0)
    // Let's assume channel 0 means "All Channels"
    bool channelMatches = (selectedChannel == 0) || (message.getChannel() == selectedChannel);
    
    // If channel doesn't match and we're not in learn mode, ignore the message
    if (!channelMatches && !isLearningMode)
    {
        // The light still shows activity (we triggered it above), but audio is filtered
        return;
    }
    
    // If we're getting the same note message repeatedly within 10ms, ignore it
    if (message.isNoteOn() || message.isNoteOff())
    {
        int currentNote = message.getNoteNumber();
        
        if (currentNote == lastNoteNumber && timeSinceLast < 10)
        {
            lastNoteCount++;
            totalIgnored++;
            
            // If we've seen this note more than 5 times in a row within 10ms, ignore it
            if (lastNoteCount > 5)
            {
                // Only print occasionally to avoid console flood
                if (lastNoteCount % 100 == 0)
                {
                    printf("⚠️ Flood protection: Ignored %d duplicate messages on note %d (last interval: %dms)\n", 
                           totalIgnored, currentNote, timeSinceLast);
                }
                return;  // IGNORE THE MESSAGE
            }
        }
        else
        {
            // New note or timing out - reset counter
            if (lastNoteCount > 5)
            {
                printf("✅ Flood ended - normal playing resumed (ignored %d messages total)\n", totalIgnored);
                totalIgnored = 0;
            }
            lastNoteNumber = currentNote;
            lastNoteCount = 0;
        }
        
        lastMessageTime = currentTime;
    }
    
    // Only add to collector if we passed the flood filter
    midiCollector.addMessageToQueue(message);
    
    // Print human-performed notes normally (no throttling for these)
    if (message.isNoteOn() && lastNoteCount <= 5)
    {
        printf("🎹 Note On: %d, Vel: %d, Ch: %d (interval: %dms)\n", 
               message.getNoteNumber(), 
               message.getVelocity(),
               message.getChannel(),
               timeSinceLast);
    }
    else if (message.isNoteOff() && lastNoteCount <= 5)
    {
        printf("🎹 Note Off: %d, Ch: %d\n", 
               message.getNoteNumber(),
               message.getChannel());
    }
    else if (message.isController())
    {
        int controller = message.getControllerNumber();
        int value = message.getControllerValue();
        printf("MIDI Controller: %d, Value: %d\n", controller, value);
    }
}


void MainComponent::sliderValueChanged(juce::Slider* slider)
{
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        
        if (slider == &lowNoteSlider)
        {
            int newLow = (int)lowNoteSlider.getValue();
            // Manual bounds checking to avoid macro conflict
            if (newLow < 0) newLow = 0;
            if (newLow > 127) newLow = 127;
            
            if (newLow <= sample->highNote)
            {
                sample->lowNote = newLow;
                printf("Sample %s: low note set to %d\n", sample->name.toRawUTF8(), newLow);
            }
            else
            {
                lowNoteSlider.setValue(sample->lowNote);
            }
        }
        else if (slider == &highNoteSlider)
        {
            int newHigh = (int)highNoteSlider.getValue();
            if (newHigh >= sample->lowNote)
            {
                sample->highNote = newHigh;
                printf("Sample %s: high note set to %d\n", sample->name.toRawUTF8(), newHigh);
            }
            else
            {
                highNoteSlider.setValue(sample->highNote);
            }
        }
        else if (slider == &rootNoteSlider)
        {
            sample->rootNote = (int)rootNoteSlider.getValue();
            printf("Sample %s: root note set to %d\n", sample->name.toRawUTF8(), sample->rootNote);
        }
        
        // Update sampler with new mapping
        updateSamplerSounds();
        sampleListBox.updateContent();
    }
    fflush(stdout);
}

void MainComponent::updateMappingUI()
{
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        
        lowNoteSlider.setValue(sample->lowNote, juce::dontSendNotification);
        highNoteSlider.setValue(sample->highNote, juce::dontSendNotification);
        rootNoteSlider.setValue(sample->rootNote, juce::dontSendNotification);
        
        lowNoteSlider.setEnabled(true);
        highNoteSlider.setEnabled(true);
        rootNoteSlider.setEnabled(true);
    }
    else
    {
        lowNoteSlider.setEnabled(false);
        highNoteSlider.setEnabled(false);
        rootNoteSlider.setEnabled(false);
    }
}

// Also make sure you have these methods implemented (they might be missing too):

void MainComponent::showMappingInterface()
{
    printf("Opening sample mapping interface...\n");
    fflush(stdout);

    // Use the separated UI component
    auto* mappingUI = new ::MappingComponent(*this);
    
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(mappingUI);
    options.dialogTitle = "Sample Mapping";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    options.launchAsync();
    
    printf("Mapping interface launched\n");
    fflush(stdout);
}

void MainComponent::addSampleToMap()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select sample files",
        currentFolder.exists() ? currentFolder : juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        formatManager.getWildcardForAllFormats()
    );
    
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | 
                             juce::FileBrowserComponent::canSelectMultipleItems,
                             [this](const juce::FileChooser& fc)
    {
        auto results = fc.getResults();
        for (auto& file : results)
        {
            loadSampleFile(file);
        }
        sampleListBox.updateContent();
        updateMappingUI();
    });
}

void MainComponent::removeSelectedSample()
{
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        juce::String sampleName = samples[selectedSampleIndex]->name;
        samples.remove(selectedSampleIndex);
        selectedSampleIndex = -1;
        updateSamplerSounds();
        sampleListBox.updateContent();
        updateMappingUI();
        
        printf("Sample '%s' removed. Total samples: %d\n", 
               sampleName.toRawUTF8(), samples.size());
        fflush(stdout);
    }
}

void MainComponent::clearAllSamples()
{
    samples.clear();
    selectedSampleIndex = -1;
    sampler.clearSounds();
    sampleListBox.updateContent();
    updateMappingUI();
    sampleCard.clearWaveform();  // Clear the waveform
    sampleCard.setSampleName("No sample loaded");
    sampleCard.setDuration(0.0);
    
    printf("All samples cleared\n");
    fflush(stdout);
}

void MainComponent::updateSamplerSounds()
{
    sampler.clearSounds();
    
    for (auto* sample : samples)
    {
        if (sample == nullptr || sample->audioData == nullptr) 
            continue;
        
        // Create a proper memory-based reader with correct overrides
        class MemoryAudioReader : public juce::AudioFormatReader
        {
        public:
            MemoryAudioReader(juce::AudioBuffer<float>* buffer, double sourceSampleRate, int sourceChannels)
                : juce::AudioFormatReader(nullptr, "Memory Reader"),
                  cachedBuffer(buffer)
            {
                sampleRate = sourceSampleRate;
                numChannels = sourceChannels;
                lengthInSamples = buffer->getNumSamples();
                bitsPerSample = 32;
                usesFloatingPointData = true;
            }
            
            // Override readSamples method (required by JUCE AudioFormatReader)
            bool readSamples(int* const* destChannels, int numDestChannels,
                            int startOffsetInDestBuffer, juce::int64 startSampleInFile,
                            int numSamples) override
            {
                // Convert int* to float* for processing
                for (int channel = 0; channel < numDestChannels; ++channel)
                {
                    if (destChannels[channel] != nullptr && channel < cachedBuffer->getNumChannels())
                    {
                        float* dest = reinterpret_cast<float*>(destChannels[channel]);
                        const float* src = cachedBuffer->getReadPointer(channel, (int)startSampleInFile);
                        
                        for (int i = 0; i < numSamples; ++i)
                        {
                            dest[startOffsetInDestBuffer + i] = src[i];
                        }
                    }
                }
                return true;
            }
            
        private:
            juce::AudioBuffer<float>* cachedBuffer;
        };
        
        auto* reader = new MemoryAudioReader(
            sample->audioData.get(),
            sample->sampleRate,
            sample->numChannels
        );
        
        // IMPORTANT FIX: Create note range that includes ALL notes that should trigger this sample
        // For single-note mode with pitch offset, we want the sample to trigger on the ORIGINAL root note
        // The pitch offset will be applied by the sampler automatically based on the difference
        // between the played note and the root note
        juce::BigInteger noteRange;
        noteRange.setRange(0, 128, false);  // Clear all notes first
        
        // Set ONLY the original root note as the trigger note
        // This means the sample will ONLY play when we hit the original root note
        // The pitch offset will be applied automatically by the sampler
        noteRange.setBit(sample->rootNote);
        
        printf("Creating sound: %s -> triggers on note %d (root: %d) with pitch offset %+d\n", 
               sample->name.toRawUTF8(), sample->rootNote, sample->rootNote, sample->pitchOffset);
        
        // Create the sound with adjusted root note
        // The sampler will calculate: played note - root note = pitch offset
        // So if we play the root note, pitch offset is 0
        // To achieve our desired pitch offset, we need to adjust the ROOT NOTE in the sampler
        auto* sound = new juce::SamplerSound(
            sample->name,
            *reader,
            noteRange,
            sample->rootNote + sample->pitchOffset,  // Adjust the root note by pitch offset
            sample->attack,
            sample->release,
            10.0
        );
        
        sampler.addSound(sound);
        
        printf("Added sound from cache: %s -> triggers on note %d (adjusted root: %d) with pitch offset %+d\n", 
               sample->name.toRawUTF8(),
               sample->rootNote,
               sample->rootNote + sample->pitchOffset,
               sample->pitchOffset);
    }
    
    printf("Sampler updated with %d sounds (each mapped to single note)\n", samples.size());
    fflush(stdout);
}

//==============================================================================
// New UI functionality implementations
void MainComponent::showSettingsMenu()
{
    juce::PopupMenu menu;
    
    menu.addItem(1, "Audio Settings");
    menu.addItem(2, "MIDI Settings");
    
    menu.showMenuAsync(juce::PopupMenu::Options()
                       .withTargetComponent(&menuButton)
                       .withMinimumWidth(150),
                       [this](int result)
                       {
                           if (result == 1)
                           {
                               showAudioDeviceSettings();
                           }
                           else if (result == 2)
                           {
                               showMidiDeviceSettings();
                           }
                       });
}

void MainComponent::scanCurrentFolderForAudioFiles()
{
    if (isScanning.exchange(true))  // If already scanning, return
        return;
    
    backgroundThreads.addJob([this]() {
        juce::Array<juce::File> newFiles;
        
        if (currentFolder.exists())
        {
            juce::Array<juce::File> allFiles;
            currentFolder.findChildFiles(allFiles, juce::File::findFiles, false);
            
            // Filter for audio files
            for (auto& file : allFiles)
            {
                if (formatManager.findFormatForFileExtension(file.getFileExtension()) != nullptr)
                {
                    newFiles.add(file);
                }
            }
            
            newFiles.sort();
        }
        
        // Update UI on message thread
        juce::MessageManager::callAsync([this, newFiles]() {
            {
                juce::ScopedWriteLock lock(folderLock);
                folderAudioFiles = newFiles;
            }
            isScanning = false;
            printf("Scanned folder: %s, found %d audio files\n", 
                   currentFolder.getFullPathName().toRawUTF8(), 
                   newFiles.size());
            fflush(stdout);
        });
    });
}

void MainComponent::navigateToFile(int index)
{
    if (folderAudioFiles.isEmpty())
        return;
    
    if (index >= 0 && index < folderAudioFiles.size())
    {
        currentFileIndex = index;
        auto file = folderAudioFiles[currentFileIndex];
        
        // Stop all currently playing notes before loading new sample
        sampler.allNotesOff(1, false);
        
        // Clear the sampler sounds immediately on the message thread
        juce::MessageManager::callAsync([this]() {
            sampler.clearSounds();
        });
        
        // Load sample on background thread
        backgroundThreads.addJob([this, file]() {
            loadSampleFileAsync(file, true, sampleCard.getPitchOffset());
        });
    }
}

void MainComponent::loadSampleFileAsync(const juce::File& file, bool autoPlay, int pitchOffsetToUse)
{
    // Create reader on background thread
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    
    if (reader == nullptr)
    {
        printf("Failed to load: %s\n", file.getFileName().toRawUTF8());
        fflush(stdout);
        return;
    }
    
    printf("Loading sample: %s (%lld samples, %d ch, %.1f kHz) - Auto-play: %s - Pitch offset: %+d\n", 
           file.getFileName().toRawUTF8(),
           reader->lengthInSamples,
           reader->numChannels,
           reader->sampleRate / 1000.0,
           autoPlay ? "YES" : "NO",
           pitchOffsetToUse);
    
    // Create sample on background thread
    auto* sample = new MappedSample();
    sample->file = file;
    sample->name = file.getFileName();
    
    // IMPORTANT: Get the current MIDI note from the card
    // We need to capture the note value before going to background thread
    int currentNote = sampleCard.getMidiNote();  // This is safe here
    
    sample->rootNote = currentNote;  // Use card's current note
    sample->lowNote = currentNote;   // Same for low note
    sample->highNote = currentNote;  // Same for high note
    sample->sampleRate = reader->sampleRate;
    sample->numChannels = reader->numChannels;
    sample->lengthInSamples = reader->lengthInSamples;
    sample->attack = 0.01;  // Fast attack for preview
    sample->release = 0.1;   // Short release
    
    // Use the provided pitch offset instead of determining from autoPlay
    sample->pitchOffset = pitchOffsetToUse;
    printf("Setting pitch offset to: %+d\n", sample->pitchOffset);
    
    // Load audio data on background thread
    auto buffer = std::make_unique<juce::AudioBuffer<float>>(
        (int)reader->numChannels, 
        (int)reader->lengthInSamples
    );
    
    bool readSuccess = reader->read(buffer.get(), 0, (int)reader->lengthInSamples, 0, true, true);
    
    if (!readSuccess)
    {
        printf("Failed to read audio data: %s\n", file.getFileName().toRawUTF8());
        delete sample;
        return;
    }
    
    // Debug: check sample peak level
    float maxSample = 0.0f;
    for (int ch = 0; ch < buffer->getNumChannels(); ++ch)
    {
        for (int s = 0; s < buffer->getNumSamples(); ++s)
        {
            float val = std::abs(buffer->getSample(ch, s));
            if (val > maxSample) maxSample = val;
        }
    }
    printf("Sample peak level: %.4f\n", maxSample);
    
    sample->audioData = std::move(buffer);
    
    // Update UI on message thread
    juce::MessageManager::callAsync([this, sample, file, autoPlay]() {
        // Stop any currently playing notes
        sampler.allNotesOff(1, false);
        
        // Clear existing samples on UI thread with lock
        {
            juce::ScopedLock lock(sampleLock);
            samples.clear();
            selectedSampleIndex = 0;
            samples.add(sample);
        }
        
        // ALL UI UPDATES MUST BE ON MESSAGE THREAD
        sampleCard.setSampleName(file.getFileName());
        sampleCard.setWaveform(file);
        
        double durationInSeconds = sample->lengthInSamples / sample->sampleRate;
        sampleCard.setDuration(durationInSeconds);
        
        sample->rootNote = sampleCard.getMidiNote();
        sample->lowNote = sampleCard.getMidiNote();
        sample->highNote = sampleCard.getMidiNote();
        
        updateSamplerSounds();
        sampleCard.setMidiNote(sample->rootNote);
        sampleCard.setPitchOffset(sample->pitchOffset);
        
        // Save the session now that the sample is fully loaded
        saveCurrentSession();
        
        if (autoPlay)
        {
            // Use MessageManager for timer callbacks too
            juce::MessageManager::callAsync([this, sample]() {
                sampler.noteOn(1, sample->rootNote, 0.8f);
                
                juce::Timer::callAfterDelay(800, [this, sample]() {
                    sampler.noteOff(1, sample->rootNote, 0.0f, true);
                });
            });
        }
        
        printf("Async load complete: %s (selected index: %d) with root note %d\n", 
               file.getFileName().toRawUTF8(), selectedSampleIndex, sample->rootNote);
        fflush(stdout);
    });
}

void MainComponent::loadNextSample()
{
    if (folderAudioFiles.isEmpty())
        return;
    
    if (currentFileIndex < 0)
        currentFileIndex = 0;
    else
        currentFileIndex = (currentFileIndex + 1) % folderAudioFiles.size();
    
    navigateToFile(currentFileIndex);
}

void MainComponent::loadPrevSample()
{
    if (folderAudioFiles.isEmpty())
        return;
    
    if (currentFileIndex < 0)
        currentFileIndex = folderAudioFiles.size() - 1;
    else
        currentFileIndex = (currentFileIndex - 1 + folderAudioFiles.size()) % folderAudioFiles.size();
    
    navigateToFile(currentFileIndex);
}

//==============================================================================
// SampleCard::Listener implementation
void MainComponent::midiNoteChanged(int newNote)
{
    // Always update the current sample if one is selected
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        sample->rootNote = newNote;
        
        // Also update low/high notes to match for single-note mode
        sample->lowNote = newNote;
        sample->highNote = newNote;
        
        printf("Sample root note updated: %s -> %d (%s) (applied immediately)\n", 
               sample->name.toRawUTF8(),
               newNote,
               juce::MidiMessage::getMidiNoteName(newNote, true, true, true).toRawUTF8());
        
        // Update sampler with new mapping IMMEDIATELY
        updateSamplerSounds();
        
        // SAVE THE SESSION whenever MIDI note changes
        saveCurrentSession();
    }
    else
    {
        printf("No sample selected to apply MIDI note change\n");
        
        // Even if no sample is selected, save the MIDI note for future samples
        saveCurrentSession();
    }
}

void MainComponent::midiChannelChanged(int newChannel)
{
    printf("MIDI channel filter set to: %s\n", 
           newChannel == 0 ? "All Channels" : juce::String(newChannel).toRawUTF8());
    
    // The actual filtering happens in handleIncomingMidiMessage
    // No need to update samples, but we might want to stop currently playing notes
    // when changing channels to avoid stuck notes
    if (newChannel != sampleCard.getMidiChannel())
    {
        // Stop all notes when changing channels to avoid confusion
        sampler.allNotesOff(1, false);
    }
    
    // SAVE THE SESSION when MIDI channel changes
    saveCurrentSession();
}

void MainComponent::learningModeChanged(bool isLearning)
{
    isLearningMode = isLearning;
    printf("MIDI Learn mode: %s\n", isLearning ? "ON" : "OFF");
}

void MainComponent::pitchOffsetChanged(int pitchOffset)
{
    printf("Pitch offset changed to: %+d semitones\n", pitchOffset);
    
    // Apply pitch offset to currently loaded sample
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        sample->pitchOffset = pitchOffset;
        
        printf("Before update - Sample root: %d, pitch offset: %d, adjusted root: %d\n", 
               sample->rootNote, sample->pitchOffset, sample->rootNote + sample->pitchOffset);
        
        // Update sampler with new pitch offset
        updateSamplerSounds();
        
        printf("After update - Sample should now play at adjusted root: %d\n", 
               sample->rootNote + sample->pitchOffset);
        printf("Applied pitch offset %+d to sample: %s\n", pitchOffset, sample->name.toRawUTF8());
    }
    
    // Save pitch offset to configuration
    if (configManager != nullptr)
    {
        configManager->savePitchOffset(pitchOffset);
    }
    
    // Save session to persist the change
    saveCurrentSession();
}

void MainComponent::handleMidiLearn(int noteNumber)
{
    if (isLearningMode)
    {
        // Update the sample card with the learned note
        sampleCard.setMidiNoteFromLearn(noteNumber);
        
        // If there's a selected sample, update its root note and sampler IMMEDIATELY
        if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
        {
            auto* sample = samples[selectedSampleIndex];
            sample->rootNote = noteNumber;
            
            // Also update low/high notes to match for single-note mode
            sample->lowNote = noteNumber;
            sample->highNote = noteNumber;
            
            // Force immediate update of the sampler
            updateSamplerSounds();
            
            printf("Sample %s root note updated to %d via MIDI Learn\n", 
                   sample->name.toRawUTF8(), noteNumber);
        }
        else
        {
            printf("No sample selected, but card note updated to %d\n", noteNumber);
        }
        
        // SAVE THE SESSION after MIDI learn
        saveCurrentSession();
    }
}

//==============================================================================
// Session persistence methods
void MainComponent::loadLastSession()
{
    // Load MIDI settings
    int savedNote = configManager->getMidiNote();
    int savedChannel = configManager->getMidiChannel();
    juce::String savedDevice = configManager->getMidiDevice();
    int savedPitchOffset = configManager->getPitchOffset();
    
    // Debug output for saved settings
    printf("Loading saved MIDI note: %d\n", savedNote);
    printf("Loading saved MIDI channel: %d\n", savedChannel);
    printf("Loading saved MIDI device: %s\n", savedDevice.toRawUTF8());
    printf("Loading saved pitch offset: %+d\n", savedPitchOffset);
    
    // Apply MIDI settings to the card
    sampleCard.setMidiNote(savedNote);
    sampleCard.setPitchOffset(savedPitchOffset);
    
    // Handle channel with wrap-around logic
    if (savedChannel >= 0 && savedChannel <= 16)
    {
        // Use the new public setMidiChannel method
        sampleCard.setMidiChannel(savedChannel);
    }
    
    // Try to restore MIDI device if it's still available
    if (savedDevice.isNotEmpty() && isValidMidiDevice(savedDevice))
    {
        // This will be handled by the MIDI selector when it initializes
        currentMidiDeviceName = savedDevice;
        
        // Start the MIDI input
        auto devices = juce::MidiInput::getAvailableDevices();
        for (auto& device : devices)
        {
            if (device.name == savedDevice)
            {
                midiInput = juce::MidiInput::openDevice(device.identifier, this);
                if (midiInput != nullptr)
                {
                    midiInput->start();
                    printf("Restored MIDI device: %s\n", savedDevice.toRawUTF8());
                }
                break;
            }
        }
    }
    
    // Load last sample if it exists
    juce::File lastSample = configManager->getLastSample();
    if (lastSample.existsAsFile())
    {
        // Check if the file is in a valid audio format
        if (formatManager.findFormatForFileExtension(lastSample.getFileExtension()) != nullptr)
        {
            printf("Loading last session sample: %s (NO AUTO-PLAY)\n", lastSample.getFileName().toRawUTF8());
            
            // Set current folder to the sample's directory
            currentFolder = lastSample.getParentDirectory();
            scanCurrentFolderForAudioFiles();
            
            // Find the index of this file in the folder
            for (int i = 0; i < folderAudioFiles.size(); ++i)
            {
                if (folderAudioFiles[i] == lastSample)
                {
                    currentFileIndex = i;
                    break;
                }
            }
            
            // Load the sample but DON'T auto-play it, but DO use the saved pitch offset
            loadSampleFileAsync(lastSample, false, savedPitchOffset);
        }
    }
}

void MainComponent::saveCurrentSession()
{
    // Always save the current MIDI settings, regardless of whether a sample is loaded
    configManager->saveMidiSettings(
        sampleCard.getMidiNote(),
        sampleCard.getMidiChannel(),
        currentMidiDeviceName
    );
    
    // Save pitch offset
    configManager->savePitchOffset(sampleCard.getPitchOffset());
    
    // Only save the sample path if a sample is actually loaded
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        configManager->saveLastSample(sample->file);
    }
}

bool MainComponent::isValidMidiDevice(const juce::String& deviceName)
{
    auto devices = juce::MidiInput::getAvailableDevices();
    for (auto& device : devices)
    {
        if (device.name == deviceName)
            return true;
    }
    return false;
}

void MainComponent::midiDeviceChanged(const juce::String& newDevice)
{
    currentMidiDeviceName = newDevice;
    saveCurrentSession();
}
