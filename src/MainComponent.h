#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

class MainComponent : public juce::AudioAppComponent,
                      public juce::Button::Listener,
                      public juce::ChangeListener  // Add this to listen for device changes
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
    
    // Change listener callback for audio device changes
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    void loadSampleFile(const juce::File& file);
    void showAudioDeviceSettings();
    void updateDeviceInfo();
    
    // UI Components
    juce::TextButton loadButton{ "Load Sample" };
    juce::TextButton audioSettingsButton{ "Audio Settings" };
    juce::Label fileNameLabel;
    juce::Label audioDeviceInfoLabel;
    juce::Label cpuUsageLabel;
    
    // Audio components
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;
    
    // Timer for updating CPU usage
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