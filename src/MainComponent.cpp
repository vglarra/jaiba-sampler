#include "MainComponent.h"

// Windows-specific includes and definitions
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <cstdio>
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
    
    addAndMakeVisible(fileNameLabel);
    fileNameLabel.setText("No file loaded", juce::dontSendNotification);
    fileNameLabel.setJustificationType(juce::Justification::centredLeft);
    
    // Initialize audio device manager
    deviceManager.initialiseWithDefaultDevices(0, 2);
    
    // Make sure window is visible
    setVisible(true);
    toFront(true);
    
    printf("MainComponent initialized\n");
    fflush(stdout);
}

MainComponent::~MainComponent()
{
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
    
    // Create a MIDI buffer
    juce::MidiBuffer midiMessages;
    
    // Add a test note if we have a sound loaded
    static double time = 0.0;
    static bool noteOn = false;
    
    if (sampler.getNumSounds() > 0 && !noteOn)
    {
        midiMessages.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
        noteOn = true;
    }
    
    time += bufferToFill.numSamples / sampler.getSampleRate();
    if (time > 1.0 && noteOn)
    {
        midiMessages.addEvent(juce::MidiMessage::noteOff(1, 60), bufferToFill.numSamples - 1);
        noteOn = false;
        time = 0.0;
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
    auto buttonArea = area.removeFromTop(40);
    loadButton.setBounds(buttonArea.removeFromLeft(120).reduced(2));
    fileNameLabel.setBounds(buttonArea);
}

void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &loadButton)
    {
        printf("Load button clicked!\n");
        fflush(stdout);
        
#ifdef _WIN32
        // Use Windows native file dialog
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
            
            // Convert to JUCE File and load
            juce::File selectedFile(fileName);
            loadSampleFile(selectedFile);
        }
        else
        {
            // User cancelled or error
            printf("File dialog cancelled or no file selected\n");
            fflush(stdout);
        }
#else
        // Fallback for non-Windows platforms
        juce::FileChooser chooser("Select a sample file...",
                                  juce::File::getSpecialLocation(juce::File::userDesktopDirectory),
                                  "*.wav;*.aiff;*.mp3");
        
        chooser.launchAsync(juce::FileBrowserComponent::openMode | 
                           juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                auto results = fc.getResults();
                if (results.size() > 0)
                {
                    loadSampleFile(results[0]);
                }
            });
#endif
        
        printf("Button click handling complete\n");
        fflush(stdout);
    }
}

void MainComponent::loadSampleFile(const juce::File& file)
{
    printf("Loading file: %s\n", file.getFullPathName().toRawUTF8());
    fflush(stdout);
    
    // Clear any existing sounds
    sampler.clearSounds();
    
    // Create a reader for the file
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    
    if (reader != nullptr)
    {
        printf("File loaded successfully, creating sound...\n");
        fflush(stdout);
        
        // Create the sound
        auto sound = new juce::SamplerSound(
            "Sample",
            *reader,
            juce::BigInteger().setRange(0, 128, true),
            60,     // root note middle C
            0.1,    // attack
            0.1,    // release
            10.0    // max length
        );
        
        sampler.addSound(sound);
        
        // Update UI
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