#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <memory>

class MainComponent : public juce::AudioAppComponent,
                      public juce::Button::Listener,
                      public juce::ChangeListener,
                      public juce::MidiInputCallback,
                      public juce::Slider::Listener
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

private:
    //==============================================================================
    // Sample loading and management
    void loadSampleFile(const juce::File& file);
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
    // FIRST: Define the ListBoxModel class
    class SampleListModel : public juce::ListBoxModel
    {
    public:
        SampleListModel(MainComponent& owner) : mainOwner(owner) {}
        
        int getNumRows() override;
        void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        
    private:
        MainComponent& mainOwner;
    };
    
    //==============================================================================
    // UI Components
    juce::TextButton loadButton{ "Load Sample" };
    juce::TextButton audioSettingsButton{ "Audio Settings" };
    juce::TextButton midiSettingsButton{ "MIDI Settings" };
    juce::TextButton sineWaveButton{ "Test Sine Wave" };
    juce::TextButton mapRangeButton{ "Map Samples" };
    juce::Label fileNameLabel;
    juce::Label audioDeviceInfoLabel;
    juce::Label midiDeviceInfoLabel;
    juce::Label cpuUsageLabel;
    
    //==============================================================================
    // Audio components
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;
    
    //==============================================================================
    // MIDI components
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::StringArray midiInputNames;
    juce::String currentMidiDeviceName;
    
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
    };
    
    juce::OwnedArray<MappedSample> samples;
    int selectedSampleIndex = -1;
    
    //==============================================================================
    // Mapping UI components
    std::unique_ptr<SampleListModel> sampleListModel;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};