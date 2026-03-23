#pragma once

#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <memory>

//==============================================================================
// Audio Preview Component
//==============================================================================
class AudioPreviewComponent : public juce::FilePreviewComponent
{
public:
    AudioPreviewComponent(juce::AudioFormatManager& fm);
    ~AudioPreviewComponent() override;

    void selectedFileChanged(const juce::File& newFile) override;

    void setMasterVolumeRef(std::atomic<float>& ref) { masterVolPtr = &ref; }

    // Called by MainComponent to stop preview (e.g. panic reset)
    void stopPreview();

    // Notified when this component is about to be deleted (so owner can null its pointer)
    std::function<void()> onDestroy;

private:
    void playPreview();
    void ensureAudioInitialized();
    
    juce::AudioFormatManager& formatManager;
    juce::File currentFile;
    juce::TextButton playButton{"Play"}, stopButton{"Stop"};
    juce::Label infoLabel;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    bool audioInitialized = false;
    std::atomic<float>* masterVolPtr = nullptr;
};

//==============================================================================
// MIDI Selector Component
//==============================================================================
class MidiSelectorComponent : public juce::Component,
                              public juce::ComboBox::Listener
{
public:
    class OwnerInterface
    {
    public:
        virtual ~OwnerInterface() = default;
        virtual juce::String getCurrentMidiDeviceName() const = 0;
        virtual void setCurrentMidiDeviceName(const juce::String& deviceName) = 0;
        virtual void stopMidiInput() = 0;
        virtual void startMidiInput(const juce::String& deviceName) = 0;
        virtual void updateDeviceInfo() = 0;
        virtual void midiDeviceChanged(const juce::String& newDevice) = 0;
    };
    
    MidiSelectorComponent(OwnerInterface& owner, const juce::StringArray& devices);
    ~MidiSelectorComponent() override = default;
    
    void resized() override;
    void comboBoxChanged(juce::ComboBox* combo) override;
    
private:
    void updateStatusLabel();
    
    OwnerInterface& owner;
    juce::StringArray deviceNames;
    juce::Label instructionLabel;
    juce::ComboBox deviceCombo;
    juce::Label statusLabel;
};

//==============================================================================
// Sample List Model
//==============================================================================
class MainComponent; // Forward declaration

class SampleListModel : public juce::ListBoxModel
{
public:
    SampleListModel(MainComponent& owner);
    ~SampleListModel() override = default;
    
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    
private:
    MainComponent& mainOwner;
};

//==============================================================================
// Mapping Component
//==============================================================================
class MappingComponent : public juce::Component,
                         public juce::Button::Listener
{
public:
    class OwnerInterface
    {
    public:
        virtual ~OwnerInterface() = default;
        virtual juce::ListBox& getSampleListBox() = 0;
        virtual juce::TextButton& getAddSampleButton() = 0;
        virtual juce::TextButton& getRemoveSampleButton() = 0;
        virtual juce::TextButton& getClearAllButton() = 0;
        virtual juce::Slider& getLowNoteSlider() = 0;
        virtual juce::Slider& getHighNoteSlider() = 0;
        virtual juce::Slider& getRootNoteSlider() = 0;
        virtual juce::Label& getLowNoteLabel() = 0;
        virtual juce::Label& getHighNoteLabel() = 0;
        virtual juce::Label& getRootNoteLabel() = 0;
        virtual juce::Label& getMappingInstructions() = 0;
        virtual void updateMappingUI() = 0;
        virtual void addSampleToMap() = 0;
        virtual void removeSelectedSample() = 0;
        virtual void clearAllSamples() = 0;
    };
    
    MappingComponent(OwnerInterface& owner);
    ~MappingComponent() override = default;
    
    void resized() override;
    void buttonClicked(juce::Button* button) override;
    
private:
    OwnerInterface& owner;
};

