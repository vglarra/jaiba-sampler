#include "MainComponent.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <string>
#include <vector>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

MainComponent::MainComponent()
{
    setSize(800, 600);
    
    formatManager.registerBasicFormats();
    
    for (int i = 0; i < 16; ++i)
        sampler.addVoice(new juce::SamplerVoice());
    
    // Configure UI
    addAndMakeVisible(loadButton);
    loadButton.addListener(this);
    
    addAndMakeVisible(audioSettingsButton);
    audioSettingsButton.addListener(this);
    audioSettingsButton.setButtonText("Audio Settings");
    
    addAndMakeVisible(midiSettingsButton);
    midiSettingsButton.addListener(this);
    midiSettingsButton.setButtonText("MIDI Settings");
    
    addAndMakeVisible(sineWaveButton);
    sineWaveButton.addListener(this);
    sineWaveButton.setButtonText("Test Sine Wave");
    sineWaveButton.setColour(juce::TextButton::buttonColourId, juce::Colours::lightblue);
    
    addAndMakeVisible(fileNameLabel);
    fileNameLabel.setText("No file loaded", juce::dontSendNotification);
    fileNameLabel.setJustificationType(juce::Justification::centredLeft);
    
    addAndMakeVisible(audioDeviceInfoLabel);
    audioDeviceInfoLabel.setJustificationType(juce::Justification::centredLeft);
    
    addAndMakeVisible(midiDeviceInfoLabel);
    midiDeviceInfoLabel.setJustificationType(juce::Justification::centredLeft);
    
    addAndMakeVisible(cpuUsageLabel);
    cpuUsageLabel.setJustificationType(juce::Justification::centredRight);
    
    // Set up audio with 2 output channels
    setAudioChannels(0, 2);

    // Don't force any specific configuration - let the user choose
    // Just set a reasonable buffer size
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    setup.bufferSize = 512;  // Stable buffer size
    deviceManager.setAudioDeviceSetup(setup, true);

    printf("Audio initialized - Use Audio Settings to configure\n");
    fflush(stdout);
}

MainComponent::~MainComponent()
{
    cpuTimer.stopTimer();
    deviceManager.removeChangeListener(this);
    shutdownAudio();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    sampler.setCurrentPlaybackSampleRate(sampleRate);
    midiCollector.reset(sampleRate);
    
    printf("PREPARE TO PLAY: sampleRate=%.0f, blockSize=%d\n", 
           sampleRate, samplesPerBlockExpected);
    fflush(stdout);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion();
    
    static int blockCounter = 0;
    if (++blockCounter % 100 == 0)
    {
        printf("Audio callback running (block %d), sineWaveActive=%d\n", 
               blockCounter, sineWaveActive ? 1 : 0);
        fflush(stdout);
    }
    
    juce::MidiBuffer midiMessages;
    
    static double time = 0.0;
    static bool noteIsPlaying = false;
    
    if (sampler.getNumSounds() > 0 && !noteIsPlaying)
    {
        midiMessages.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
        noteIsPlaying = true;
        time = 0.0;
        printf("Playing test note\n");
        fflush(stdout);
    }
    
    time += (double)bufferToFill.numSamples / sampler.getSampleRate();
    
    if (time > 1.0 && noteIsPlaying)
    {
        midiMessages.addEvent(juce::MidiMessage::noteOff(1, 60), bufferToFill.numSamples - 1);
        noteIsPlaying = false;
        printf("Note off\n");
        fflush(stdout);
    }
    
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
            
            // Generate mono sine wave
            float sample = (float)(std::sin(sineWavePhase) * sineWaveAmplitude);
            
            // Write the same sample to ALL active channels (stereo)
            for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
            {
                float* channelData = bufferToFill.buffer->getWritePointer(channel);
                
                for (int i = 0; i < bufferToFill.numSamples; ++i)
                {
                    // Use the pre-calculated sample or update per sample for continuous phase
                    channelData[i] += (float)(std::sin(sineWavePhase + i * phaseIncrement) * sineWaveAmplitude);
                }
            }
            
            // Update phase for next block
            sineWavePhase += phaseIncrement * bufferToFill.numSamples;
            while (sineWavePhase >= juce::MathConstants<double>::twoPi)
                sineWavePhase -= juce::MathConstants<double>::twoPi;
            
            // Debug output
            static int sineCounter = 0;
            if (++sineCounter % 100 == 0)
            {
                printf("Sine wave: phase=%.2f, channels=%d\n", 
                    sineWavePhase, bufferToFill.buffer->getNumChannels());
                fflush(stdout);
            }
        }
    }
}

void MainComponent::releaseResources()
{
    printf("Audio resources released\n");
    fflush(stdout);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);
    
    g.setColour(juce::Colours::white);
    g.drawText("My Sampler", getLocalBounds().removeFromTop(40), 
               juce::Justification::centred, true);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(20);
    
    auto buttonRow = area.removeFromTop(40);
    loadButton.setBounds(buttonRow.removeFromLeft(120).reduced(2));
    audioSettingsButton.setBounds(buttonRow.removeFromLeft(120).reduced(2));
    midiSettingsButton.setBounds(buttonRow.removeFromLeft(120).reduced(2));
    sineWaveButton.setBounds(buttonRow.removeFromLeft(120).reduced(2));
    
    fileNameLabel.setBounds(area.removeFromTop(30));
    
    auto infoArea = area.removeFromTop(80);
    audioDeviceInfoLabel.setBounds(infoArea.removeFromTop(40));
    midiDeviceInfoLabel.setBounds(infoArea);
    
    cpuUsageLabel.setBounds(area.removeFromBottom(30));
}

void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &loadButton)
    {
        printf("Load button clicked!\n");
        fflush(stdout);
        
#ifdef _WIN32
        OPENFILENAMEA ofn = {0};
        char fileName[MAX_PATH] = {0};
        
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = "Audio Files\0*.wav;*.aiff;*.mp3\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        ofn.lpstrTitle = "Select a sample file";
        
        printf("Opening Windows file dialog...\n");
        fflush(stdout);
        
        if (GetOpenFileNameA(&ofn))
        {
            printf("File selected: %s\n", fileName);
            fflush(stdout);
            
            juce::File selectedFile(fileName);
            loadSampleFile(selectedFile);
        }
        else
        {
            printf("File dialog cancelled\n");
            fflush(stdout);
        }
#endif
    }
    else if (button == &audioSettingsButton)
    {
        showAudioDeviceSettings();
    }
    else if (button == &midiSettingsButton)
    {
        showMidiDeviceSettings();
    }
    else if (button == &sineWaveButton)
    {
        toggleSineWave();
    }
}

void MainComponent::toggleSineWave()
{
    sineWaveActive = !sineWaveActive;
    
    if (sineWaveActive)
    {
        sineWavePhase = 0.0;
        sineWaveButton.setButtonText("Stop Sine Wave");
        sineWaveButton.setColour(juce::TextButton::buttonColourId, juce::Colours::lightcoral);
        printf("Sine wave ON (440Hz) - checking audio device...\n");
        
        if (auto* currentDevice = deviceManager.getCurrentAudioDevice())
        {
            printf("  Audio device: %s\n", currentDevice->getName().toRawUTF8());
            printf("  Sample rate: %.0f Hz\n", currentDevice->getCurrentSampleRate());
            printf("  Buffer size: %d samples\n", currentDevice->getCurrentBufferSizeSamples());
        }
        else
        {
            printf("  WARNING: No audio device selected!\n");
        }
        fflush(stdout);
    }
    else
    {
        sineWaveButton.setButtonText("Test Sine Wave");
        sineWaveButton.setColour(juce::TextButton::buttonColourId, juce::Colours::lightblue);
        printf("Sine wave OFF\n");
        fflush(stdout);
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        updateDeviceInfo();
        // forceStereoConfiguration();
    }
}

void MainComponent::showAudioDeviceSettings()
{
    printf("Opening audio device settings...\n");
    fflush(stdout);
    
    // Full featured selector - let user choose everything
    auto* selector = new juce::AudioDeviceSelectorComponent(
        deviceManager,
        0,        // minAudioInputChannels
        256,      // maxAudioInputChannels
        0,        // minAudioOutputChannels
        2,        // maxAudioOutputChannels
        true,     // showMidiInputOptions
        true,     // showMidiOutputSelector
        false,    // showChannelsAsStereoPairs
        false     // hideAdvancedOptionsWithButton
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
    
    printf("Audio settings launched\n");
    fflush(stdout);
}

void MainComponent::updateDeviceInfo()
{
    juce::String info;
    
    if (auto* currentDevice = deviceManager.getCurrentAudioDevice())
    {
        info += "Device: " + currentDevice->getName() + "\n";
        info += "Sample Rate: " + juce::String(currentDevice->getCurrentSampleRate()) + " Hz\n";
        info += "Buffer Size: " + juce::String(currentDevice->getCurrentBufferSizeSamples()) + " samples\n";
        
        auto activeInputs = currentDevice->getActiveInputChannels();
        auto activeOutputs = currentDevice->getActiveOutputChannels();
        
        info += "Inputs: " + juce::String(activeInputs.countNumberOfSetBits()) + " channels\n";
        info += "Outputs: " + juce::String(activeOutputs.countNumberOfSetBits()) + " channels\n";
        
        if (auto* deviceType = deviceManager.getCurrentDeviceTypeObject())
        {
            info += "Type: " + deviceType->getTypeName();
        }
    }
    else
    {
        info = "No audio device selected";
    }
    
    audioDeviceInfoLabel.setText(info, juce::dontSendNotification);
    
    // ✅ FIXED: Explicit isEmpty() check
    juce::String midiInfo = "MIDI: ";
    if (midiInput != nullptr)
    {
        if (currentMidiDeviceName.isEmpty())
        {
            midiInfo += "No device selected";
        }
        else
        {
            midiInfo += currentMidiDeviceName;
            midiInfo += " (Connected)";
        }
    }
    else
    {
        midiInfo += "No device selected";
    }
    
    midiDeviceInfoLabel.setText(midiInfo, juce::dontSendNotification);
    
    double cpuUsage = deviceManager.getCpuUsage() * 100.0;
    cpuUsageLabel.setText("CPU: " + juce::String(cpuUsage, 2) + "%", juce::dontSendNotification);
}

void MainComponent::loadSampleFile(const juce::File& file)
{
    printf("Loading file: %s\n", file.getFullPathName().toRawUTF8());
    fflush(stdout);
    
    sampler.clearSounds();
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    
    if (reader != nullptr)
    {
        printf("File loaded successfully, creating sound...\n");
        fflush(stdout);
        
        juce::BigInteger allNotes;
        allNotes.setRange(0, 128, true);
        
        auto* sound = new juce::SamplerSound(
            "Sample",
            *reader,
            allNotes,
            60, 0.1, 0.1, 10.0
        );
        
        sampler.addSound(sound);
        fileNameLabel.setText(file.getFileName(), juce::dontSendNotification);
        
        printf("Sound loaded and ready to play\n");
        fflush(stdout);
        
        sampler.noteOn(1, 60, 0.8f);
    }
    else
    {
        printf("Failed to load file: unsupported format\n");
        fflush(stdout);
        fileNameLabel.setText("Unsupported file format", juce::dontSendNotification);
    }
}

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
    
    // Create a dialog with MIDI device selector
    class MidiSelectorComponent : public juce::Component,
                                   public juce::ComboBox::Listener
    {
    public:
        MidiSelectorComponent(MainComponent& owner, const juce::StringArray& devices, const juce::String& current)
            : mainOwner(owner)
        {
            addAndMakeVisible(instructionLabel);
            instructionLabel.setText("Select MIDI Input Device:", juce::dontSendNotification);
            instructionLabel.setJustificationType(juce::Justification::centredLeft);
            
            addAndMakeVisible(deviceCombo);
            deviceCombo.addItem("None (Disabled)", 1);
            
            // Store device names for safe comparison
            for (int i = 0; i < devices.size(); ++i)
            {
                deviceNames.add(devices[i]);
                deviceCombo.addItem(devices[i], i + 2);
            }
            
            // Set current selection using std::string comparison
            int selectedIndex = -1;
            if (!current.isEmpty())
            {
                std::string currentStr = current.toStdString();
                for (int i = 0; i < deviceNames.size(); ++i)
                {
                    if (deviceNames[i].toStdString() == currentStr)
                    {
                        selectedIndex = i;
                        break;
                    }
                }
            }
            
            if (selectedIndex >= 0)
                deviceCombo.setSelectedId(selectedIndex + 2);
            else
                deviceCombo.setSelectedId(1);
            
            deviceCombo.addListener(this);
            
            addAndMakeVisible(statusLabel);
            updateStatusLabel();
            
            setSize(400, 200);
        }
        
        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            instructionLabel.setBounds(area.removeFromTop(25));
            deviceCombo.setBounds(area.removeFromTop(30));
            statusLabel.setBounds(area.removeFromTop(50));
        }
        
        void comboBoxChanged(juce::ComboBox* combo) override
        {
            int selectedId = combo->getSelectedId();
            
            if (selectedId == 1)
            {
                // Disable MIDI input
                mainOwner.currentMidiDeviceName = juce::String();
                mainOwner.midiCollector.reset(mainOwner.sampler.getSampleRate());
                
                // Close any open MIDI input
                if (mainOwner.midiInput != nullptr)
                {
                    mainOwner.midiInput->stop();
                    mainOwner.midiInput.reset();
                }
                
                statusLabel.setText("MIDI Input: Disabled", juce::dontSendNotification);
            }
            else
            {
                int index = selectedId - 2;
                if (index >= 0 && index < deviceNames.size())
                {
                    juce::String deviceName = deviceNames[index];
                    mainOwner.currentMidiDeviceName = deviceName;
                    
                    // Update status text
                    statusLabel.setText("MIDI Input: " + deviceName, juce::dontSendNotification);
                    
                    // Close existing MIDI input if open
                    if (mainOwner.midiInput != nullptr)
                    {
                        mainOwner.midiInput->stop();
                        mainOwner.midiInput.reset();
                    }
                    
                    // Open the new MIDI device
                    auto devices = juce::MidiInput::getAvailableDevices();
                    for (auto& device : devices)
                    {
                        if (device.name == deviceName)
                        {
                            mainOwner.midiInput = juce::MidiInput::openDevice(device.identifier, &mainOwner);
                            if (mainOwner.midiInput != nullptr)
                            {
                                mainOwner.midiInput->start();
                                printf("MIDI device opened: %s\n", deviceName.toRawUTF8());
                            }
                            break;
                        }
                    }
                }
            }
            updateStatusLabel();
            mainOwner.updateDeviceInfo(); // Update the main display
        }
        
    private:
        void updateStatusLabel()
        {
            juce::String statusText;
            // In JUCE 8, if the unique_ptr is not null, the device is open
            if (mainOwner.midiInput != nullptr)
                statusText = "Status: Connected and ready";
            else
                statusText = "Status: Not connected";
            
            statusLabel.setText(statusText, juce::dontSendNotification);
        }
        
        MainComponent& mainOwner;
        juce::StringArray deviceNames;
        juce::Label instructionLabel;
        juce::ComboBox deviceCombo;
        juce::Label statusLabel;
    };
    
    // Create and show the dialog
    auto* selector = new MidiSelectorComponent(*this, midiInputNames, currentMidiDeviceName);
    selector->setSize(400, 200);
    
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "MIDI Input Settings";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    options.launchAsync();
    
    printf("MIDI settings launched\n");
    fflush(stdout);
}


void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    // Add incoming MIDI messages to the collector for timed playback
    midiCollector.addMessageToQueue(message);
    
    // Also trigger the sampler directly for immediate response
    if (message.isNoteOn())
    {
        printf("MIDI Note On: %d, Velocity: %d, Channel: %d\n", 
               message.getNoteNumber(), 
               message.getVelocity(),
               message.getChannel());
        fflush(stdout);
        
        // Trigger the note with normalized velocity (0.0 to 1.0)
        float velocity = message.getVelocity() / 127.0f;
        sampler.noteOn(message.getChannel(), message.getNoteNumber(), velocity);
    }
    else if (message.isNoteOff())
    {
        printf("MIDI Note Off: %d, Channel: %d\n", 
               message.getNoteNumber(),
               message.getChannel());
        fflush(stdout);
        
        sampler.noteOff(message.getChannel(), message.getNoteNumber(), 0.0f, true);
    }
    else if (message.isPitchWheel())
    {
        // Optional: handle pitch bend
        int pitchValue = message.getPitchWheelValue();
        float pitchBend = (pitchValue - 8192) / 8192.0f; // Range -1.0 to 1.0
        // You could add pitch bend processing here
    }
    else if (message.isController())
    {
        // Optional: handle MIDI controllers (mod wheel, etc.)
        int controller = message.getControllerNumber();
        int value = message.getControllerValue();
        printf("MIDI Controller: %d, Value: %d\n", controller, value);
        fflush(stdout);
    }
}

