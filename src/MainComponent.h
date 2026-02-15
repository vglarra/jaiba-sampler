#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

class MainComponent : public juce::AudioAppComponent,
                      public juce::Button::Listener
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

private:
    void loadSampleFile(const juce::File& file);
    
    // UI Components
    juce::TextButton loadButton{ "Load Sample" };
    juce::Label fileNameLabel;
    
    // Audio components
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};