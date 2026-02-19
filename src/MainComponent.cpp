#include "MainComponent.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

//==============================================================================
MainComponent::MainComponent()
    : sampleListBox("samples", nullptr),
      cpuTimer(*this)
{
    printf("DEBUG: MainComponent constructor started\n");
    fflush(stdout);
    
    // Create the model
    sampleListModel = std::make_unique<SampleListModel>(*this);
    sampleListBox.setModel(sampleListModel.get());
    
    setSize(900, 700);
    
    formatManager.registerBasicFormats();
    
    // Add sampler voices
    for (int i = 0; i < 16; ++i)
        sampler.addVoice(new juce::SamplerVoice());
    
    // Configure buttons
    loadButton.setButtonText("Load Sample");
    loadButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    loadButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible(loadButton);
    loadButton.addListener(this);
    
    audioSettingsButton.setButtonText("Audio Settings");
    audioSettingsButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    audioSettingsButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible(audioSettingsButton);
    audioSettingsButton.addListener(this);
    
    midiSettingsButton.setButtonText("MIDI Settings");
    midiSettingsButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    midiSettingsButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible(midiSettingsButton);
    midiSettingsButton.addListener(this);
    
    sineWaveButton.setButtonText("Test Sine Wave");
    sineWaveButton.setColour(juce::TextButton::buttonColourId, juce::Colours::lightblue);
    sineWaveButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    addAndMakeVisible(sineWaveButton);
    sineWaveButton.addListener(this);
    
    mapRangeButton.setButtonText("Map Samples");
    mapRangeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF6A4A3A));
    mapRangeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addAndMakeVisible(mapRangeButton);
    mapRangeButton.addListener(this);
    
    // Configure labels
    addAndMakeVisible(fileNameLabel);
    fileNameLabel.setText("No sample loaded", juce::dontSendNotification);
    fileNameLabel.setJustificationType(juce::Justification::centred);
    fileNameLabel.setFont(juce::Font(18.0f, juce::Font::bold));
    fileNameLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    
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
    
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(24.0f, juce::Font::bold));
    
    auto titleArea = getLocalBounds().reduced(20).removeFromTop(50);
    g.drawText("My Sampler", titleArea, juce::Justification::centred, true);
    
    g.setColour(juce::Colours::darkgrey);
    auto lineY = titleArea.getBottom() + 5;
    g.drawHorizontalLine(lineY, 20, getWidth() - 20);
    
    auto footerY = getHeight() - 110;
    g.drawHorizontalLine(footerY, 20, getWidth() - 20);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(20);
    
    auto buttonRow = area.removeFromTop(50);
    const int buttonWidth = 120;
    const int buttonHeight = 30;
    
    auto buttonArea = buttonRow.withSizeKeepingCentre(buttonWidth * 5 + 20, buttonHeight);
    
    loadButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
    audioSettingsButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
    midiSettingsButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
    sineWaveButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
    mapRangeButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
    
    auto contentArea = area.reduced(20, 10);
    auto currentSampleArea = contentArea.removeFromTop(60);
    fileNameLabel.setBounds(currentSampleArea.reduced(5));
    
    auto footerArea = area.removeFromBottom(100);
    
    auto audioFooter = footerArea.removeFromTop(40);
    audioDeviceInfoLabel.setBounds(audioFooter.reduced(5));
    
    auto midiFooter = footerArea.removeFromTop(40);
    midiDeviceInfoLabel.setBounds(midiFooter.reduced(5));
    
    cpuUsageLabel.setBounds(footerArea.removeFromBottom(30).removeFromRight(200));
}

//==============================================================================
void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &loadButton)
    {
#ifdef _WIN32
        OPENFILENAMEA ofn = {0};
        char fileName[MAX_PATH] = {0};
        
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = "Audio Files\0*.wav;*.aiff;*.mp3\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        
        if (GetOpenFileNameA(&ofn))
        {
            juce::File selectedFile(fileName);
            loadSampleFile(selectedFile);
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
    else if (button == &mapRangeButton)
    {
        showMappingInterface();
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
    }
    else
    {
        sineWaveButton.setButtonText("Test Sine Wave");
        sineWaveButton.setColour(juce::TextButton::buttonColourId, juce::Colours::lightblue);
    }
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
    
    double cpuUsage = deviceManager.getCpuUsage() * 100.0;
    cpuUsageLabel.setText("CPU: " + juce::String(cpuUsage, 2) + "%", juce::dontSendNotification);
}

//==============================================================================
void MainComponent::loadSampleFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    
    if (reader != nullptr)
    {
        auto* sample = new MappedSample();
        sample->file = file;
        sample->name = file.getFileName();
        
        if (samples.isEmpty())
        {
            sample->lowNote = 36;
            sample->highNote = 48;
            sample->rootNote = 42;
        }
        else
        {
            auto* lastSample = samples.getLast();
            sample->lowNote = lastSample->highNote + 1;
            
            if (sample->lowNote > 127)
            {
                delete sample;
                return;
            }
            
            int potentialHigh = sample->lowNote + 11;
            sample->highNote = (potentialHigh < 127) ? potentialHigh : 127;
            sample->rootNote = sample->lowNote + (sample->highNote - sample->lowNote) / 2;
        }
        
        samples.add(sample);
        updateSamplerSounds();
        fileNameLabel.setText(file.getFileName(), juce::dontSendNotification);
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

int MainComponent::SampleListModel::getNumRows()
{
    return mainOwner.samples.size();
}

void MainComponent::SampleListModel::paintListBoxItem(int rowNumber, juce::Graphics& g, 
                                                      int width, int height, bool rowIsSelected)
{
    if (rowNumber >= 0 && rowNumber < mainOwner.samples.size())
    {
        auto* sample = mainOwner.samples[rowNumber];
        
        if (rowIsSelected)
            g.fillAll(juce::Colours::lightblue);
        
        juce::String text = sample->name;
        text += " [" + juce::String(sample->lowNote) + "-" + 
                juce::String(sample->highNote) + "] root:" + 
                juce::String(sample->rootNote);
        
        g.setColour(juce::Colours::black);
        g.drawText(text, 2, 0, width - 4, height, 
                  juce::Justification::centredLeft, true);
    }
}

void MainComponent::SampleListModel::selectedRowsChanged(int lastRowSelected)
{
    mainOwner.selectedSampleIndex = lastRowSelected;
    mainOwner.updateMappingUI();
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
    
    class MappingComponent : public juce::Component,
                             public juce::Button::Listener
    {
    public:
        MappingComponent(MainComponent& owner) : mainOwner(owner)
        {
            // Set up the UI
            addAndMakeVisible(mainOwner.sampleListBox);
            mainOwner.sampleListBox.setColour(juce::ListBox::backgroundColourId, juce::Colours::white);
            
            addAndMakeVisible(mainOwner.addSampleButton);
            mainOwner.addSampleButton.addListener(this);
            
            addAndMakeVisible(mainOwner.removeSampleButton);
            mainOwner.removeSampleButton.addListener(this);
            
            addAndMakeVisible(mainOwner.clearAllButton);
            mainOwner.clearAllButton.addListener(this);
            
            // Note range controls
            addAndMakeVisible(mainOwner.lowNoteLabel);
            mainOwner.lowNoteLabel.attachToComponent(&mainOwner.lowNoteSlider, true);
            
            addAndMakeVisible(mainOwner.lowNoteSlider);
            mainOwner.lowNoteSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            mainOwner.lowNoteSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
            
            addAndMakeVisible(mainOwner.highNoteLabel);
            mainOwner.highNoteLabel.attachToComponent(&mainOwner.highNoteSlider, true);
            
            addAndMakeVisible(mainOwner.highNoteSlider);
            mainOwner.highNoteSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            mainOwner.highNoteSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
            
            addAndMakeVisible(mainOwner.rootNoteLabel);
            mainOwner.rootNoteLabel.attachToComponent(&mainOwner.rootNoteSlider, true);
            
            addAndMakeVisible(mainOwner.rootNoteSlider);
            mainOwner.rootNoteSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            mainOwner.rootNoteSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
            
            addAndMakeVisible(mainOwner.mappingInstructions);
            mainOwner.mappingInstructions.setText(
                "Instructions:\n"
                "• Click 'Add Sample' to load audio files\n"
                "• Select a sample from the list\n"
                "• Adjust the note range sliders\n"
                "• Low/High notes determine which keys trigger this sample\n"
                "• Root note is the original pitch of the sample\n"
                "• Multiple samples can be loaded for different key ranges",
                juce::dontSendNotification);
            mainOwner.mappingInstructions.setJustificationType(juce::Justification::topLeft);
            
            setSize(700, 500);
            
            // Update UI with current selection
            mainOwner.updateMappingUI();
        }
        
        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            
            // Sample list on left (40% width)
            auto listArea = area.removeFromLeft(area.getWidth() * 0.4f);
            mainOwner.sampleListBox.setBounds(listArea.reduced(5));
            
            // Buttons below list
            auto buttonArea = listArea.removeFromBottom(80).reduced(5);
            mainOwner.addSampleButton.setBounds(buttonArea.removeFromTop(25).reduced(2));
            mainOwner.removeSampleButton.setBounds(buttonArea.removeFromTop(25).reduced(2));
            mainOwner.clearAllButton.setBounds(buttonArea.removeFromTop(25).reduced(2));
            
            // Controls on right (60% width)
            auto controlsArea = area.reduced(10);
            
            // Sliders
            auto sliderArea = controlsArea.removeFromTop(200);
            mainOwner.lowNoteSlider.setBounds(sliderArea.removeFromTop(50).withTrimmedLeft(70));
            mainOwner.highNoteSlider.setBounds(sliderArea.removeFromTop(50).withTrimmedLeft(70));
            mainOwner.rootNoteSlider.setBounds(sliderArea.removeFromTop(50).withTrimmedLeft(70));
            
            // Instructions at bottom
            mainOwner.mappingInstructions.setBounds(controlsArea.reduced(5));
        }
        
        void buttonClicked(juce::Button* button) override
        {
            if (button == &mainOwner.addSampleButton)
                mainOwner.addSampleToMap();
            else if (button == &mainOwner.removeSampleButton)
                mainOwner.removeSelectedSample();
            else if (button == &mainOwner.clearAllButton)
                mainOwner.clearAllSamples();
        }
        
    private:
        MainComponent& mainOwner;
    };
    
    auto* mappingUI = new MappingComponent(*this);
    
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
#ifdef _WIN32
    OPENFILENAMEA ofn = {0};
    char fileName[MAX_PATH] = {0};
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "Audio Files\0*.wav;*.aiff;*.mp3\0All Files\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;
    ofn.lpstrTitle = "Select sample files";
    
    if (GetOpenFileNameA(&ofn))
    {
        // Handle multiple files
        char* filePtr = fileName;
        std::string directory(filePtr);
        filePtr += directory.length() + 1;
        
        if (*filePtr == 0)
        {
            // Single file
            juce::File selectedFile(fileName);
            loadSampleFile(selectedFile);
        }
        else
        {
            // Multiple files
            while (*filePtr)
            {
                std::string filename(filePtr);
                juce::File fullPath = juce::File(directory).getChildFile(filename);
                loadSampleFile(fullPath);
                filePtr += filename.length() + 1;
            }
        }
        
        sampleListBox.updateContent();
        updateMappingUI();
    }
#endif
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
    
    printf("All samples cleared\n");
    fflush(stdout);
}

void MainComponent::updateSamplerSounds()
{
    sampler.clearSounds();
    
    for (auto* sample : samples)
    {
        if (sample == nullptr) continue;  // Safety check
        
        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(sample->file));
        
        if (reader != nullptr)
        {
            juce::BigInteger noteRange;
            noteRange.setRange(0, 128, false);
            noteRange.setRange(sample->lowNote, 
                              (sample->highNote - sample->lowNote + 1), 
                              true);
            
            auto* sound = new juce::SamplerSound(
                sample->name,
                *reader,
                noteRange,
                sample->rootNote,
                sample->attack,
                sample->release,
                10.0
            );
            
            sampler.addSound(sound);
            
            printf("Added sound: %s (notes %d-%d, root %d)\n", 
                   sample->name.toRawUTF8(),
                   sample->lowNote, sample->highNote,
                   sample->rootNote);
        }
        else
        {
            printf("Warning: Could not load sample: %s\n", 
                   sample->file.getFullPathName().toRawUTF8());
        }
    }
    
    printf("Sampler updated with %d sounds\n", samples.size());
    fflush(stdout);
}

