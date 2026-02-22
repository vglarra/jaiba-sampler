#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <memory>

#include "UIComponents.h"
#include "SampleCard.h"

class MainComponent : public juce::AudioAppComponent,
                      public juce::Button::Listener,
                      public juce::ChangeListener,
                      public juce::MidiInputCallback,
                      public juce::Slider::Listener,
                      public MidiSelectorComponent::OwnerInterface,
                      public MappingComponent::OwnerInterface
{
public:
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    //==============================================================================
    void paint(juce::Graphics& g) override;
    void resized() override;

    //==============================================================================
    void buttonClicked(juce::Button* button) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void sliderValueChanged(juce::Slider* slider) override;

    //==============================================================================
    // SampleListModel access methods
    int getSamplesCount() const { return samples.size(); }
    juce::String getSampleName(int index) const 
    { 
        if (index >= 0 && index < samples.size())
            return samples[index]->name;
        return juce::String();
    }
    int getSampleLowNote(int index) const
    {
        if (index >= 0 && index < samples.size())
            return samples[index]->lowNote;
        return 0;
    }
    int getSampleHighNote(int index) const
    {
        if (index >= 0 && index < samples.size())
            return samples[index]->highNote;
        return 0;
    }
    int getSampleRootNote(int index) const
    {
        if (index >= 0 && index < samples.size())
            return samples[index]->rootNote;
        return 0;
    }
    int getSelectedSampleIndex() const { return selectedSampleIndex; }
    void setSelectedSampleIndex(int index) 
    { 
        selectedSampleIndex = index; 
        updateMappingUI();
    }

private:
    //==============================================================================
    // Sample loading and management
    void loadSampleFile(const juce::File& file);
    void loadSampleFileAsync(const juce::File& file);
    void updateSamplerSounds();
    
    // Audio device management
    void showAudioDeviceSettings();
    void updateDeviceInfo();
    
    // MIDI device management
    void showMidiDeviceSettings();
    void updateMidiDeviceList();
    
    // Sine wave test
    void toggleSineWave();
    
    // Multi-sample mapping interface
    void showMappingInterface();
    void addSampleToMap();
    void removeSelectedSample();
    void clearAllSamples();
    void updateMappingUI();
    
    //==============================================================================
    // New UI functionality
    void showSettingsMenu();
    void scanCurrentFolderForAudioFiles();
    void navigateToFile(int index);
    void loadNextSample();
    void loadPrevSample();

    //==============================================================================
    // UI Components
    juce::TextButton menuButton{ "Menu" };
    juce::TextButton testToneButton{ "Test tone" };
    SampleCard sampleCard;  // New card component
    juce::Label audioDeviceInfoLabel;
    juce::Label midiDeviceInfoLabel;
    juce::Label cpuUsageLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    
    //==============================================================================
    // Folder navigation
    juce::File currentFolder;
    juce::Array<juce::File> folderAudioFiles;
    int currentFileIndex = -1;
    std::atomic<bool> isScanning{false};
    juce::ReadWriteLock folderLock;  // Protect folderAudioFiles
    
    //==============================================================================
    // Audio components
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;
    juce::ThreadPool backgroundThreads{4};  // For background tasks
    
    //==============================================================================
    // MIDI components
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::StringArray midiInputNames;
    juce::String currentMidiDeviceName;
    juce::CriticalSection midiLock;  // Thread safety for MIDI device management
    
    //==============================================================================
    // Sine wave generation
    bool sineWaveActive = false;
    double sineWavePhase = 0.0;
    double sineWaveFrequency = 440.0;
    float sineWaveAmplitude = 0.2f;
    
    //==============================================================================
    // Sample management for multi-sampling
    struct MappedSample
    {
        juce::File file;
        juce::String name;
        int rootNote = 60;
        int lowNote = 48;
        int highNote = 60;
        double attack = 0.1;
        double release = 0.1;
        bool isSelected = false;
        
        // Cached audio data
        std::unique_ptr<juce::AudioBuffer<float>> audioData;
        double sampleRate = 0;
        int numChannels = 0;
        juce::int64 lengthInSamples = 0;
        
        // Add a flag to indicate if sample is loaded
        bool isValid() const { return audioData != nullptr && audioData->getNumSamples() > 0; }
        
        ~MappedSample() = default;
    };
    
    juce::OwnedArray<MappedSample> samples;
    int selectedSampleIndex = -1;
    juce::CriticalSection sampleLock;  // Thread safety for sample array access
    
    //==============================================================================
    // Mapping UI components
    std::unique_ptr<::SampleListModel> sampleListModel;
    juce::ListBox sampleListBox;
    
    juce::TextButton addSampleButton{ "Add Sample" };
    juce::TextButton removeSampleButton{ "Remove Sample" };
    juce::TextButton clearAllButton{ "Clear All" };
    juce::Slider rootNoteSlider;
    juce::Slider lowNoteSlider;
    juce::Slider highNoteSlider;
    juce::Label rootNoteLabel{ "Root Note:" };
    juce::Label lowNoteLabel{ "Low Note:" };
    juce::Label highNoteLabel{ "High Note:" };
    juce::Label mappingInstructions;
    
    //==============================================================================
    // Timer for updates
    class CpuTimer : public juce::Timer
    {
    public:
        CpuTimer(MainComponent& owner) : mainOwner(owner) {}
        void timerCallback() override { mainOwner.updateDeviceInfo(); }
    private:
        MainComponent& mainOwner;
    };
    
    CpuTimer cpuTimer;
    double lastCPU = 0.0;
    int cpuUpdateCounter = 0;

    //==============================================================================
    // MidiSelectorComponent::OwnerInterface implementation
    juce::String getCurrentMidiDeviceName() const override 
    { 
        juce::ScopedLock lock(midiLock);
        return currentMidiDeviceName; 
    }
    void setCurrentMidiDeviceName(const juce::String& deviceName) override 
    { 
        juce::ScopedLock lock(midiLock);
        currentMidiDeviceName = deviceName; 
    }
    void stopMidiInput() override 
    { 
        if (midiInput != nullptr)
        {
            midiInput->stop();
            midiInput.reset();
        }
        midiCollector.reset(sampler.getSampleRate());
    }
    void startMidiInput(const juce::String& deviceName) override
    {
        auto devices = juce::MidiInput::getAvailableDevices();
        for (auto& device : devices)
        {
            if (device.name == deviceName)
            {
                midiInput = juce::MidiInput::openDevice(device.identifier, this);
                if (midiInput != nullptr)
                {
                    midiInput->start();
                }
                break;
            }
        }
    }

    //==============================================================================
    // MappingComponent::OwnerInterface implementation
    juce::ListBox& getSampleListBox() override { return sampleListBox; }
    juce::TextButton& getAddSampleButton() override { return addSampleButton; }
    juce::TextButton& getRemoveSampleButton() override { return removeSampleButton; }
    juce::TextButton& getClearAllButton() override { return clearAllButton; }
    juce::Slider& getLowNoteSlider() override { return lowNoteSlider; }
    juce::Slider& getHighNoteSlider() override { return highNoteSlider; }
    juce::Slider& getRootNoteSlider() override { return rootNoteSlider; }
    juce::Label& getLowNoteLabel() override { return lowNoteLabel; }
    juce::Label& getHighNoteLabel() override { return highNoteLabel; }
    juce::Label& getRootNoteLabel() override { return rootNoteLabel; }
    juce::Label& getMappingInstructions() override { return mappingInstructions; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};