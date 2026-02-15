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
                      public juce::MidiInputCallback  // Add this for MIDI input
{
public:
    MainComponent();
    ~MainComponent() override;
    
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;
    
    void paint(juce::Graphics& g) override;
    void resized() override;
    
    // Button listener callback
    void buttonClicked(juce::Button* button) override;
    
    // Change listener callback
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    
    // MIDI input callback
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

private:
    void loadSampleFile(const juce::File& file);
    void showAudioDeviceSettings();
    void showMidiDeviceSettings();  // New method for MIDI settings
    void updateDeviceInfo();
    void updateMidiDeviceList();    // New method to scan MIDI devices
    
    // UI Components
    juce::TextButton loadButton{ "Load Sample" };
    juce::TextButton audioSettingsButton{ "Audio Settings" };
    juce::TextButton midiSettingsButton{ "MIDI Settings" };  // New button
    juce::Label fileNameLabel;
    juce::Label audioDeviceInfoLabel;
    juce::Label midiDeviceInfoLabel;  // New label for MIDI info
    juce::Label cpuUsageLabel;
    
    // Audio components
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;
    
    // MIDI components
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::StringArray midiInputNames;
    juce::String currentMidiDeviceName;
    
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