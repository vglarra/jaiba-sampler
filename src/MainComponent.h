#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

class MainComponent : public juce::AudioAppComponent,
                      public juce::Button::Listener,
                      public juce::ChangeListener,
                      public juce::MidiInputCallback
{
public:
    MainComponent();
    ~MainComponent() override;
    
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    
    void buttonClicked(juce::Button* button) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

private:
    void loadSampleFile(const juce::File& file);
    void showAudioDeviceSettings();
    void showMidiDeviceSettings();
    void updateDeviceInfo();
    void updateMidiDeviceList();
    void toggleSineWave();  // New method for sine wave

    
    // UI Components
    juce::TextButton loadButton{ "Load Sample" };
    juce::TextButton audioSettingsButton{ "Audio Settings" };
    juce::TextButton midiSettingsButton{ "MIDI Settings" };
    juce::TextButton sineWaveButton{ "Test Sine Wave" };  // NEW BUTTON
    juce::Label fileNameLabel;
    juce::Label audioDeviceInfoLabel;
    juce::Label midiDeviceInfoLabel;
    juce::Label cpuUsageLabel;
    
    // Audio components
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;
    
    // MIDI components
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::StringArray midiInputNames;
    juce::String currentMidiDeviceName;
    
    // Sine wave generation
    bool sineWaveActive = false;
    double sineWavePhase = 0.0;
    double sineWaveFrequency = 440.0;  // A440
    float sineWaveAmplitude = 0.2f;    // Reduced amplitude to avoid clipping
    
    // Timer for updates
    class CpuTimer : public juce::Timer
    {
    public:
        CpuTimer(MainComponent& owner) : mainOwner(owner) {}
        void timerCallback() override { mainOwner.updateDeviceInfo(); }
    private:
        MainComponent& mainOwner;
    };
    
    CpuTimer cpuTimer{ *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};