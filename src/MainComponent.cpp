#include "MainComponent.h"

MainComponent::MainComponent()
{
    setSize(800, 600);
    
    // Initialize audio device manager with default settings
    deviceManager.initialiseWithDefaultDevices(0, 2);
}

MainComponent::~MainComponent()
{
    shutdownAudio();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    // This will be called when audio starts - we'll add sampler code here later
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion();
}

void MainComponent::releaseResources()
{
    // Clean up when audio stops
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);
    g.setColour(juce::Colours::white);
    g.drawText("My Sampler - Ready to Build!", getLocalBounds(), 
               juce::Justification::centred, true);
}

void MainComponent::resized()
{
    // Layout components here later
}