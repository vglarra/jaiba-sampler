#include "MainComponent.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

MainComponent::MainComponent()
{
    setSize(800, 600);
    
    // Initialize audio format manager
    formatManager.registerBasicFormats();
    
    // Set up the sampler with 4 voices
    for (int i = 0; i < 4; ++i)
        sampler.addVoice(new juce::SamplerVoice());
    
    // Configure UI
    addAndMakeVisible(loadButton);
    loadButton.addListener(this);
    
    addAndMakeVisible(audioSettingsButton);
    audioSettingsButton.addListener(this);
    audioSettingsButton.setButtonText("Audio Settings");
    
    addAndMakeVisible(fileNameLabel);
    fileNameLabel.setText("No file loaded", juce::dontSendNotification);
    fileNameLabel.setJustificationType(juce::Justification::centredLeft);
    
    addAndMakeVisible(audioDeviceInfoLabel);
    audioDeviceInfoLabel.setJustificationType(juce::Justification::centredLeft);
    
    addAndMakeVisible(cpuUsageLabel);
    cpuUsageLabel.setJustificationType(juce::Justification::centredRight);
    
    // Initialize audio device manager with default devices
    deviceManager.initialiseWithDefaultDevices(0, 2);
    
    // Listen for audio device changes
    deviceManager.addChangeListener(this);
    
    // Start timer for CPU usage updates (every 100ms)
    cpuTimer.startTimer(100);
    
    // Update device info display
    updateDeviceInfo();
    
    // Make sure window is visible
    setVisible(true);
    toFront(true);
    
    printf("MainComponent initialized\n");
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
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion();
    
    // Create a MIDI buffer for test notes
    juce::MidiBuffer midiMessages;
    
    // Test note generation (only if we have a sound loaded)
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
    
    // FIXED: Use sampler.getSampleRate()
    time += (double)bufferToFill.numSamples / sampler.getSampleRate();
    
    if (time > 1.0 && noteIsPlaying)
    {
        midiMessages.addEvent(juce::MidiMessage::noteOff(1, 60), bufferToFill.numSamples - 1);
        noteIsPlaying = false;
        printf("Note off\n");
        fflush(stdout);
    }
    
    // Render audio
    sampler.renderNextBlock(*bufferToFill.buffer, midiMessages, 0, bufferToFill.numSamples);
}

void MainComponent::releaseResources()
{
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
    
    // Top button row
    auto buttonRow = area.removeFromTop(40);
    loadButton.setBounds(buttonRow.removeFromLeft(120).reduced(2));
    audioSettingsButton.setBounds(buttonRow.removeFromLeft(120).reduced(2));
    
    // File name label
    fileNameLabel.setBounds(area.removeFromTop(30));
    
    // Audio device info area
    auto infoArea = area.removeFromTop(80);
    audioDeviceInfoLabel.setBounds(infoArea);
    
    // CPU usage at bottom
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
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        // Audio device settings changed - update display
        updateDeviceInfo();
    }
}

void MainComponent::showAudioDeviceSettings()
{
    printf("Opening audio device settings...\n");
    fflush(stdout);
    
    // Create the selector component
    auto* selector = new juce::AudioDeviceSelectorComponent(
        deviceManager,
        0,        // minAudioInputChannels
        256,      // maxAudioInputChannels
        0,        // minAudioOutputChannels
        2,        // maxAudioOutputChannels
        false,    // showMidiInputOptions
        false,    // showMidiOutputSelector
        false,    // showChannelsAsStereoPairs
        false     // hideAdvancedOptionsWithButton
    );
    
    selector->setSize(500, 400);
    
    // Create a dialog that will close properly
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Audio Device Settings";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    // This makes it a proper modal dialog that closes with ESC or Close button
    options.launchAsync();
    
    updateDeviceInfo();
    
    printf("Audio settings launched - click Close or press ESC to exit\n");
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
    
    // Update CPU usage
    double cpu = deviceManager.getCpuUsage() * 100.0;
    cpuUsageLabel.setText("CPU: " + juce::String(cpu, 2) + "%", juce::dontSendNotification);
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
        
        auto* sound = new juce::SamplerSound(
            "Sample",
            *reader,
            juce::BigInteger().setRange(0, 128, true),
            60,     // root note middle C
            0.1,    // attack
            0.1,    // release
            10.0    // max length
        );
        
        sampler.addSound(sound);
        fileNameLabel.setText(file.getFileName(), juce::dontSendNotification);
        
        printf("Sound loaded and ready to play\n");
        fflush(stdout);
        
        // Play test note
        sampler.noteOn(1, 60, 0.8f);
    }
    else
    {
        printf("Failed to load file: unsupported format\n");
        fflush(stdout);
        fileNameLabel.setText("Unsupported file format", juce::dontSendNotification);
    }
}