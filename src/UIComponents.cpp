#include "UIComponents.h"
#include "MainComponent.h"
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <vfw.h>
#endif

//==============================================================================
// AudioPreviewComponent implementation
//==============================================================================
AudioPreviewComponent::AudioPreviewComponent(juce::AudioFormatManager& fm) 
    : formatManager(fm)
{
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(infoLabel);
    
    playButton.onClick = [this] { playPreview(); };
    stopButton.onClick = [this] { stopPreview(); };
    
    // Audio initialization will be done lazily when needed
    audioInitialized = false;
    
    // Start audio initialization immediately in background to reduce first-play delay
    juce::MessageManager::callAsync([this] {
        if (!audioInitialized) {
            ensureAudioInitialized();
        }
    });
}

void AudioPreviewComponent::ensureAudioInitialized()
{
    if (!audioInitialized)
    {
        deviceManager.initialiseWithDefaultDevices(0, 2);
        deviceManager.addAudioCallback(&audioSourcePlayer);
        audioSourcePlayer.setSource(&transportSource);
        audioInitialized = true;
    }
}

void AudioPreviewComponent::selectedFileChanged(const juce::File& newFile)
{
    currentFile = newFile;
    if (newFile.existsAsFile())
    {
        // Get file info without full reader
        auto inputStream = newFile.createInputStream();
        if (inputStream != nullptr)
        {
            auto* reader = formatManager.createReaderFor(newFile);
            if (reader)
            {
                double lengthInSeconds = reader->lengthInSamples / reader->sampleRate;
                infoLabel.setText(
                    newFile.getFileName() + "\n" +
                    juce::String(reader->sampleRate / 1000.0, 1) + " kHz, " +
                    juce::String(reader->numChannels) + " ch, " +
                    juce::String(lengthInSeconds, 1) + " s",
                    juce::dontSendNotification
                );
                delete reader;
            }
        }
        
        // AUTO-PLAY when file is selected!
        playPreview();
    }
}

void AudioPreviewComponent::playPreview()
{
    ensureAudioInitialized();  // Lazy initialization
    stopPreview();  // Stop any currently playing preview
    
    auto* reader = formatManager.createReaderFor(currentFile);
    if (reader)
    {
        auto* source = new juce::AudioFormatReaderSource(reader, true);
        transportSource.setSource(source, 0, nullptr, reader->sampleRate);
        transportSource.start();
    }
}

void AudioPreviewComponent::stopPreview()
{
    transportSource.stop();
    transportSource.setSource(nullptr);
}

//==============================================================================
// MidiSelectorComponent implementation
//==============================================================================
MidiSelectorComponent::MidiSelectorComponent(OwnerInterface& owner, const juce::StringArray& devices)
    : owner(owner)
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
    juce::String current = owner.getCurrentMidiDeviceName();
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

void MidiSelectorComponent::resized()
{
    auto area = getLocalBounds().reduced(10);
    instructionLabel.setBounds(area.removeFromTop(25));
    deviceCombo.setBounds(area.removeFromTop(30));
    statusLabel.setBounds(area.removeFromTop(50));
}

void MidiSelectorComponent::comboBoxChanged(juce::ComboBox* combo)
{
    int selectedId = combo->getSelectedId();
    
    if (selectedId == 1)
    {
        // Disable MIDI input
        owner.setCurrentMidiDeviceName(juce::String());
        owner.stopMidiInput();
        statusLabel.setText("MIDI Input: Disabled", juce::dontSendNotification);
        
        // Notify session manager
        owner.midiDeviceChanged(juce::String());
        owner.updateDeviceInfo();
    }
    else
    {
        int index = selectedId - 2;
        if (index >= 0 && index < deviceNames.size())
        {
            juce::String deviceName = deviceNames[index];
            owner.setCurrentMidiDeviceName(deviceName);
            statusLabel.setText("MIDI Input: " + deviceName, juce::dontSendNotification);
            
            // Stop any existing input first
            owner.stopMidiInput();
            
            // Open device synchronously (we're already on the message thread)
            owner.startMidiInput(deviceName);
            owner.midiDeviceChanged(deviceName);
            owner.updateDeviceInfo();
        }
    }
    updateStatusLabel();
}

void MidiSelectorComponent::updateStatusLabel()
{
    juce::String statusText;
    // In JUCE 8, if the unique_ptr is not null, the device is open
    if (!owner.getCurrentMidiDeviceName().isEmpty())
        statusText = "Status: Connected and ready";
    else
        statusText = "Status: Not connected";
    
    statusLabel.setText(statusText, juce::dontSendNotification);
}

//==============================================================================
// SampleListModel implementation
//==============================================================================
SampleListModel::SampleListModel(MainComponent& owner) : mainOwner(owner) {}

int SampleListModel::getNumRows()
{
    return mainOwner.getSamplesCount();
}

void SampleListModel::paintListBoxItem(int rowNumber, juce::Graphics& g, 
                                       int width, int height, bool rowIsSelected)
{
    if (rowNumber >= 0 && rowNumber < mainOwner.getSamplesCount())
    {
        juce::String name = mainOwner.getSampleName(rowNumber);
        int lowNote = mainOwner.getSampleLowNote(rowNumber);
        int highNote = mainOwner.getSampleHighNote(rowNumber);
        int rootNote = mainOwner.getSampleRootNote(rowNumber);
        
        if (rowIsSelected)
            g.fillAll(juce::Colours::lightblue);
        
        juce::String text = name;
        text += " [" + juce::String(lowNote) + "-" + 
                juce::String(highNote) + "] root:" + 
                juce::String(rootNote);
        
        g.setColour(juce::Colours::black);
        g.drawText(text, 2, 0, width - 4, height, 
                  juce::Justification::centredLeft, true);
    }
}

void SampleListModel::selectedRowsChanged(int lastRowSelected)
{
    mainOwner.setSelectedSampleIndex(lastRowSelected);
}

//==============================================================================
// MappingComponent implementation
//==============================================================================
MappingComponent::MappingComponent(OwnerInterface& owner) : owner(owner)
{
    // Set up the UI
    addAndMakeVisible(owner.getSampleListBox());
    owner.getSampleListBox().setColour(juce::ListBox::backgroundColourId, juce::Colours::white);
    
    addAndMakeVisible(owner.getAddSampleButton());
    owner.getAddSampleButton().addListener(this);
    
    addAndMakeVisible(owner.getRemoveSampleButton());
    owner.getRemoveSampleButton().addListener(this);
    
    addAndMakeVisible(owner.getClearAllButton());
    owner.getClearAllButton().addListener(this);
    
    // Note range controls
    addAndMakeVisible(owner.getLowNoteLabel());
    owner.getLowNoteLabel().attachToComponent(&owner.getLowNoteSlider(), true);
    
    addAndMakeVisible(owner.getLowNoteSlider());
    owner.getLowNoteSlider().setSliderStyle(juce::Slider::LinearHorizontal);
    owner.getLowNoteSlider().setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    
    addAndMakeVisible(owner.getHighNoteLabel());
    owner.getHighNoteLabel().attachToComponent(&owner.getHighNoteSlider(), true);
    
    addAndMakeVisible(owner.getHighNoteSlider());
    owner.getHighNoteSlider().setSliderStyle(juce::Slider::LinearHorizontal);
    owner.getHighNoteSlider().setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    
    addAndMakeVisible(owner.getRootNoteLabel());
    owner.getRootNoteLabel().attachToComponent(&owner.getRootNoteSlider(), true);
    
    addAndMakeVisible(owner.getRootNoteSlider());
    owner.getRootNoteSlider().setSliderStyle(juce::Slider::LinearHorizontal);
    owner.getRootNoteSlider().setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    
    addAndMakeVisible(owner.getMappingInstructions());
    owner.getMappingInstructions().setText(
        "Instructions:\n"
        "• Click 'Add Sample' to load audio files\n"
        "• Select a sample from the list\n"
        "• Adjust the note range sliders\n"
        "• Low/High notes determine which keys trigger this sample\n"
        "• Root note is the original pitch of the sample\n"
        "• Multiple samples can be loaded for different key ranges",
        juce::dontSendNotification);
    owner.getMappingInstructions().setJustificationType(juce::Justification::topLeft);
    
    setSize(700, 500);
    
    // Update UI with current selection
    owner.updateMappingUI();
}

void MappingComponent::resized()
{
    auto area = getLocalBounds().reduced(10);
    
    // Sample list on left (40% width)
    auto listArea = area.removeFromLeft(area.getWidth() * 0.4f);
    owner.getSampleListBox().setBounds(listArea.reduced(5));
    
    // Buttons below list
    auto buttonArea = listArea.removeFromBottom(80).reduced(5);
    owner.getAddSampleButton().setBounds(buttonArea.removeFromTop(25).reduced(2));
    owner.getRemoveSampleButton().setBounds(buttonArea.removeFromTop(25).reduced(2));
    owner.getClearAllButton().setBounds(buttonArea.removeFromTop(25).reduced(2));
    
    // Controls on right (60% width)
    auto controlsArea = area.reduced(10);
    
    // Sliders
    auto sliderArea = controlsArea.removeFromTop(200);
    owner.getLowNoteSlider().setBounds(sliderArea.removeFromTop(50).withTrimmedLeft(70));
    owner.getHighNoteSlider().setBounds(sliderArea.removeFromTop(50).withTrimmedLeft(70));
    owner.getRootNoteSlider().setBounds(sliderArea.removeFromTop(50).withTrimmedLeft(70));
    
    // Instructions at bottom
    owner.getMappingInstructions().setBounds(controlsArea.reduced(5));
}

void MappingComponent::buttonClicked(juce::Button* button)
{
    if (button == &owner.getAddSampleButton())
        owner.addSampleToMap();
    else if (button == &owner.getRemoveSampleButton())
        owner.removeSelectedSample();
    else if (button == &owner.getClearAllButton())
        owner.clearAllSamples();
}

