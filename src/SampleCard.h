#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <vector>
#include "KnobLookAndFeel.h"

class SampleCard : public juce::Component
{
public:
    SampleCard(juce::AudioFormatManager& formatManager)
        : formatManager(formatManager)
    {
        // Top row buttons - font size 14px to match pitch controls
        addButton.setButtonText("+");
        addButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        addButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        addAndMakeVisible(addButton);
        
        // Configure Prev button (top right)
        prevButton.setButtonText("Prev");
        prevButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        prevButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        addAndMakeVisible(prevButton);
        
        // Configure Next button (top right)
        nextButton.setButtonText("Next");
        nextButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        nextButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        addAndMakeVisible(nextButton);
        
        // Learn button - font size 14px
        learnButton.setButtonText("Learn");
        learnButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        learnButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        learnButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFE25A00));
        learnButton.onClick = [this] { toggleLearnMode(); };
        addAndMakeVisible(learnButton);
        
        // Configure waveform area with viewport
        waveformComponent = std::make_unique<WaveformComponent>(
            formatManager, currentAudioFile, pitchOffset);
        
        // Create a container for the waveform that can be larger than the viewport
        waveformContainer = std::make_unique<juce::Component>();
        waveformContainer->addAndMakeVisible(waveformComponent.get());
        
        // Set up the viewport
        waveformViewport.setViewedComponent(waveformContainer.get(), false);
        waveformViewport.setScrollBarsShown(true, false); // Show vertical scroll bar? false, show horizontal? true
        waveformViewport.setScrollOnDragEnabled(false); // Disabled so waveform can capture mouse for marker drag
        addAndMakeVisible(waveformViewport);
        
        // Set up fixed info labels
        topInfoLabel.setJustificationType(juce::Justification::centred);
        topInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFCECECE));
        topInfoLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(topInfoLabel);
        
        bottomInfoLabel.setJustificationType(juce::Justification::centred);
        bottomInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD4A017));
        bottomInfoLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(bottomInfoLabel);
        
        // MIDI Note display - font size 14px (reduced from 16px)
        midiNoteLabel.setJustificationType(juce::Justification::centred);
        midiNoteLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFE25A00));
        midiNoteLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF363636));
        updateMidiNoteDisplay();
        addAndMakeVisible(midiNoteLabel);
        
        // MIDI Channel controls - font size 14px
        channelDownButton.setButtonText("-");
        channelDownButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelDownButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        channelDownButton.onClick = [this] { adjustMidiChannel(-1); };
        channelDownButton.setTooltip("Previous MIDI channel");
        addAndMakeVisible(channelDownButton);
        
        midiChannelLabel.setJustificationType(juce::Justification::centred);
        midiChannelLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9DC95C));
        midiChannelLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF363636));
        updateMidiChannelDisplay();
        addAndMakeVisible(midiChannelLabel);
        
        channelUpButton.setButtonText("+");
        channelUpButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        channelUpButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        channelUpButton.onClick = [this] { adjustMidiChannel(1); };
        channelUpButton.setTooltip("Next MIDI channel");
        addAndMakeVisible(channelUpButton);
        
        // Pitch adjustment controls - keep font size 14px (already correct)
        pitchDownButton.setButtonText("Down");
        pitchDownButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        pitchDownButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        pitchDownButton.onClick = [this] { adjustPitchDown(); };
        pitchDownButton.setTooltip("Lower pitch (longer duration)");
        addAndMakeVisible(pitchDownButton);
        
        pitchLabel.setJustificationType(juce::Justification::centred);
        pitchLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF8CBCDC));
        pitchLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF363636));
        updatePitchDisplay(pitchOffset);
        addAndMakeVisible(pitchLabel);
        
        pitchUpButton.setButtonText("Up");
        pitchUpButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
        pitchUpButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        pitchUpButton.onClick = [this] { adjustPitchUp(); };
        pitchUpButton.setTooltip("Higher pitch (shorter duration)");
        addAndMakeVisible(pitchUpButton);

        // Volume knob — neutral gray, same compact style as Master Vol
        volumeKnob.setLookAndFeel(&compactKnobLaf);
        volumeKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        volumeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volumeKnob.setRange(0.0, 1.0, 0.01);
        volumeKnob.setValue(1.0, juce::dontSendNotification);
        volumeKnob.setTooltip("Volume (drag up/down)");
        volumeKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFCECECE)); // neutral gray fill
        volumeKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        volumeKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFF1E1E1E)); // dark indicator on gray
        volumeKnob.onValueChange = [this] {
            listeners.call([this](Listener& l) { l.volumeChanged((float)volumeKnob.getValue()); });
        };
        addAndMakeVisible(volumeKnob);

        volumeLabel.setText("Vol", juce::dontSendNotification);
        volumeLabel.setJustificationType(juce::Justification::centred);
        volumeLabel.setFont(juce::Font(11.0f));
        volumeLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(volumeLabel);

        // Start point knob — deep red matching the start marker (#CC0000), white indicator
        startKnob.setLookAndFeel(&compactKnobLaf);
        startKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        startKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        startKnob.setRange(0.0, 1.0, 0.001);
        startKnob.setValue(0.0, juce::dontSendNotification);
        startKnob.setTooltip("Start point (drag to set sample start)");
        startKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFCC0000)); // matches start marker
        startKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        startKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFFFFF)); // white indicator on red
        startKnob.onValueChange = [this] {
            double seconds = startKnob.getValue() * originalDuration;
            seconds = applyGridSnap(seconds, true);
            float norm = (float)juce::jlimit(0.0, 1.0,
                originalDuration > 0.0 ? seconds / originalDuration : startKnob.getValue());
            startPointNormalized = norm;
            startKnob.setValue(norm, juce::dontSendNotification);
            if (waveformComponent != nullptr)
                waveformComponent->setStartMarker(norm);
            notifyStartPointChanged();
        };
        addAndMakeVisible(startKnob);

        startKnobLabel.setText("Start", juce::dontSendNotification);
        startKnobLabel.setJustificationType(juce::Justification::centred);
        startKnobLabel.setFont(juce::Font(11.0f));
        startKnobLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(startKnobLabel);

        // Wire start marker drag → knob + listeners
        waveformComponent->onMarkerDragged = [this](float newNorm) {
            if (gridSnapEnabled && originalDuration > 0.0)
            {
                double seconds = newNorm * originalDuration;
                seconds = applyGridSnap(seconds, true);
                newNorm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
            }
            startPointNormalized = newNorm;
            startKnob.setValue(newNorm, juce::dontSendNotification);
            notifyStartPointChanged();
        };

        // End point knob — cyan matching the end marker (#00AACC), white indicator
        endKnob.setLookAndFeel(&compactKnobLaf);
        endKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        endKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        endKnob.setRange(0.0, 1.0, 0.001);
        endKnob.setValue(1.0, juce::dontSendNotification);
        endKnob.setTooltip("End point (drag to set sample end)");
        endKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFF00AACC)); // matches end marker
        endKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        endKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFFFFF)); // white indicator on cyan
        endKnob.onValueChange = [this] {
            double seconds = endKnob.getValue() * originalDuration;
            seconds = applyGridSnap(seconds, false);
            float norm = (float)juce::jlimit(0.0, 1.0,
                originalDuration > 0.0 ? seconds / originalDuration : endKnob.getValue());
            endPointNormalized = norm;
            endKnob.setValue(norm, juce::dontSendNotification);
            if (waveformComponent != nullptr)
                waveformComponent->setEndMarker(norm);
            notifyEndPointChanged();
        };
        addAndMakeVisible(endKnob);

        endKnobLabel.setText("End", juce::dontSendNotification);
        endKnobLabel.setJustificationType(juce::Justification::centred);
        endKnobLabel.setFont(juce::Font(11.0f));
        endKnobLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(endKnobLabel);

        // Wire end marker drag → knob + listeners
        waveformComponent->onEndMarkerDragged = [this](float newNorm) {
            if (gridSnapEnabled && originalDuration > 0.0)
            {
                double seconds = newNorm * originalDuration;
                seconds = applyGridSnap(seconds, false);
                newNorm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
            }
            endPointNormalized = newNorm;
            endKnob.setValue(newNorm, juce::dontSendNotification);
            notifyEndPointChanged();
        };

        // Loop toggle button — left of Start knob in pitch row
        loopButton.setButtonText("Loop");
        loopButton.setClickingTogglesState(true);
        loopButton.setToggleState(false, juce::dontSendNotification);
        loopButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
        loopButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF00FF88));
        loopButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        loopButton.setColour(juce::TextButton::textColourOnId,  juce::Colour(0xFF111111));
        loopButton.onClick = [this] {
            bool isOn = loopButton.getToggleState();
            if (waveformComponent != nullptr)
                waveformComponent->setLoopHighlight(isOn);
            listeners.call([isOn](Listener& l) { l.loopEnabledChanged(isOn); });
        };
        addAndMakeVisible(loopButton);

        // Freeze button — left of Loop button in pitch row
        // Manual toggle state (setClickingTogglesState false) — we manage isFreezeActive ourselves
        // so the double-tap can override the toggle without any JUCE state fighting us.
        freezeButton.setButtonText("Freeze");
        freezeButton.setClickingTogglesState(false);
        applyFreezeButtonStyle(false);
        freezeButton.onClick = [this]
        {
            const juce::int64 now = static_cast<juce::int64>(juce::Time::getMillisecondCounter());
            if (lastFreezeTapMs != 0 && (now - lastFreezeTapMs) < 400)
            {
                // Double tap — panic reset: turn off both freeze and loop
                lastFreezeTapMs = 0;
                const bool wasFreeze = isFreezeActive;
                isFreezeActive = false;
                loopWasOnBeforeFreeze = false;

                // Flash white, then restore OFF style after 200 ms
                freezeButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFFFFFFFF));
                freezeButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF000000));
                juce::Timer::callAfterDelay(200, [this] { applyFreezeButtonStyle(false); });

                // Turn loop off
                setLoopEnabled(false);
                listeners.call([](Listener& l) { l.loopEnabledChanged(false); });

                // Notify freeze off (only if it was actually on — avoids redundant allNotesOff)
                if (wasFreeze)
                    listeners.call([](Listener& l) { l.freezeChanged(false); });
            }
            else
            {
                lastFreezeTapMs = now;
                toggleFreeze();
            }
        };
        addAndMakeVisible(freezeButton);

        // Grid snap toggle — snaps markers to nearest grid division
        gridSnapButton.setClickingTogglesState(true);
        gridSnapButton.setToggleState(false, juce::dontSendNotification);
        gridSnapButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF4A4A4A));
        gridSnapButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFFAAFF00));
        gridSnapButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCECECE));
        gridSnapButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFF111111));
        gridSnapButton.setTooltip("Snap markers to grid time divisions");
        gridSnapButton.onClick = [this]
        {
            gridSnapEnabled = gridSnapButton.getToggleState();
            updateGridResolutionButtonState();
            if (waveformComponent != nullptr)
                waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
            listeners.call([this](Listener& l) { l.gridSnapChanged(gridSnapEnabled); });
        };
        addAndMakeVisible(gridSnapButton);

        // Grid resolution cycling button — to the right of Grid button
        updateGridResolutionButton();  // sets text + colors based on initial state
        gridResolutionButton.setTooltip("Grid resolution — click to cycle: 1ms \xe2\x86\x92 10ms \xe2\x86\x92 50ms \xe2\x86\x92 100ms \xe2\x86\x92 500ms \xe2\x86\x92 1s");
        gridResolutionButton.onClick = [this]
        {
            gridResolutionIndex = (gridResolutionIndex + 1) % numGridResolutions;
            updateGridResolutionButton();
            if (waveformComponent != nullptr)
                waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
            listeners.call([this](Listener& l) { l.gridResolutionChanged(gridResolutionIndex); });
        };
        addAndMakeVisible(gridResolutionButton);

        // Tune button — auto pitch detection
        tuneButton.setButtonText("Tune");
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        tuneButton.setTooltip("Detect fundamental pitch and auto-tune (analyzes Start-End region)");
        tuneButton.onClick = [this] { startTuneAnalysis(); };
        addAndMakeVisible(tuneButton);

        // Transient detection toggle — enables/disables the whole transient subsystem
        detectionToggleButton.setClickingTogglesState(true);
        detectionToggleButton.setToggleState(true, juce::dontSendNotification);
        detectionToggleButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF8B2500)); // dark red = active
        detectionToggleButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF8B2500));
        detectionToggleButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFFFFFFF));
        detectionToggleButton.setColour(juce::TextButton::textColourOnId,   juce::Colour(0xFFFFFFFF));
        detectionToggleButton.setTooltip("Enable / disable transient detection");
        detectionToggleButton.onClick = [this]
        {
            transientDetectionEnabled = detectionToggleButton.getToggleState();
            detectionToggleButton.setColour(juce::TextButton::buttonColourId,
                transientDetectionEnabled ? juce::Colour(0xFF8B2500) : juce::Colour(0xFF4A4A4A));
            detectionToggleButton.setColour(juce::TextButton::textColourOffId,
                transientDetectionEnabled ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF7A7A7A));
            updateTransientControlsState();
            // FIX 3: notify MainComponent so it can save to disk
            listeners.call([this](Listener& l) { l.transientDetectionEnabledChanged(transientDetectionEnabled); });
        };
        addAndMakeVisible(detectionToggleButton);

        // Transient snap buttons — snap start marker to nearest transient (orange-red)
        prevTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
        prevTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        prevTransientButton.setTooltip("Snap start to previous transient");
        prevTransientButton.onClick = [this] { snapToPrevTransient(); };
        addAndMakeVisible(prevTransientButton);

        nextTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
        nextTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        nextTransientButton.setTooltip("Snap start to next transient");
        nextTransientButton.onClick = [this] { snapToNextTransient(); };
        addAndMakeVisible(nextTransientButton);

        // End marker transient snap buttons (cyan-blue)
        prevEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
        prevEndTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        prevEndTransientButton.setTooltip("Snap end to previous transient");
        prevEndTransientButton.onClick = [this] { snapEndToPrevTransient(); };
        addAndMakeVisible(prevEndTransientButton);

        nextEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
        nextEndTransientButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
        nextEndTransientButton.setTooltip("Snap end to next transient");
        nextEndTransientButton.onClick = [this] { snapEndToNextTransient(); };
        addAndMakeVisible(nextEndTransientButton);

        // Sensitivity knob — orange accent, controls transient detection threshold
        sensKnob.setLookAndFeel(&compactKnobLaf);
        sensKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        sensKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        sensKnob.setRange(1.5, 10.0, 0.1);
        sensKnob.setValue(4.0, juce::dontSendNotification);
        sensKnob.setTooltip("Transient sensitivity: low=many transients, high=only strong hits");
        sensKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFF8B2500)); // dark red matching CRA
        sensKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
        sensKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFFFFFFFF));
        sensKnob.onValueChange = [this] {
            transientThreshold = sensKnob.getValue();
            if (currentAudioFile.existsAsFile())
                detectTransients(currentAudioFile);
        };
        addAndMakeVisible(sensKnob);

        sensLabel.setText("Sens", juce::dontSendNotification);
        sensLabel.setJustificationType(juce::Justification::centred);
        sensLabel.setFont(juce::Font(11.0f));
        sensLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(sensLabel);

        transientCountLabel.setText("T: 0", juce::dontSendNotification);
        transientCountLabel.setJustificationType(juce::Justification::centred);
        transientCountLabel.setFont(juce::Font(10.0f));
        transientCountLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
        addAndMakeVisible(transientCountLabel);

        // Update pitch button labels with tooltips
        updatePitchButtonLabels();
        
        // Configure sample name label (bottom left)
        sampleNameLabel.setJustificationType(juce::Justification::centredLeft);
        sampleNameLabel.setFont(juce::Font(11.0f, juce::Font::bold));  // Matches pitch indicator
        sampleNameLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF8CBCDC));
        addAndMakeVisible(sampleNameLabel);

        // Configure duration label (bottom right)
        durationLabel.setJustificationType(juce::Justification::centredRight);
        durationLabel.setFont(juce::Font(11.0f, juce::Font::bold));  // Matches other indicators
        durationLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9DC95C));
        addAndMakeVisible(durationLabel);
        
    }
    
    ~SampleCard() override
    {
        // Stop background tune analysis before destruction
        if (tuneThread != nullptr)
        {
            tuneThread->stopThread(2000);
            tuneThread = nullptr;
        }
        volumeKnob.setLookAndFeel(nullptr);
        startKnob.setLookAndFeel(nullptr);
        endKnob.setLookAndFeel(nullptr);
        sensKnob.setLookAndFeel(nullptr);
    }

    void resetViewport()
    {
        waveformViewport.setViewPosition(0, 0);
    }
    
    void resized() override
    {
        auto area = getLocalBounds();
        // Add margin around the entire card content (10px on each side)
        area.reduce(10, 10);
        
        // ===== CRITICAL FIX: Change top row height from 40px to 30px =====
        // Top row: All controls (height 30px to match pitch controls)
        auto topRow = area.removeFromTop(30);  // ← CHANGED from 40 to 30
        
        // + button on left (40px wide, 30px tall)
        addButton.setBounds(topRow.removeFromLeft(40).reduced(2));
        
        // Leave some space between + button and MIDI controls
        topRow.removeFromLeft(10);
        
        // Learn button (60px wide, 30px tall)
        learnButton.setBounds(topRow.removeFromLeft(60).reduced(2));
        
        // MIDI Note display (100px wide, 30px tall)
        midiNoteLabel.setBounds(topRow.removeFromLeft(100).reduced(2));
        
        // Space between note and channel controls
        topRow.removeFromLeft(10);
        
        // MIDI Channel controls (total 120px: 30 + 60 + 30)
        channelDownButton.setBounds(topRow.removeFromLeft(30).reduced(2));
        midiChannelLabel.setBounds(topRow.removeFromLeft(60).reduced(2));
        channelUpButton.setBounds(topRow.removeFromLeft(30).reduced(2));
        
        // Space before Prev/Next buttons
        topRow.removeFromLeft(10);
        
        // Prev/Next buttons on right: Next rightmost, Prev to its left (L→R: Prev | Next)
        nextButton.setBounds(topRow.removeFromRight(60).reduced(2));
        prevButton.setBounds(topRow.removeFromRight(60).reduced(2));
        
        // Add 5px margin between top row and waveform
        area.removeFromTop(5);
        
        // Calculate waveform height based on 4cm at 96 DPI (fixed height)
        const int waveformHeight = static_cast<int>(4 * 37.8); // ~151px
        
        // ===== CRITICAL FIX #1: Reserve scrollbar space in viewport bounds =====
        // Viewport needs extra height to accommodate scrollbar without squishing content
        auto waveformRect = area.removeFromTop(waveformHeight + SCROLLBAR_HEIGHT);
        waveformViewport.setBounds(waveformRect);
        fixedViewportWidth = waveformRect.getWidth();
        
        // Position fixed info labels within the viewport area
        auto labelArea = waveformRect;
        topInfoLabel.setBounds(labelArea.removeFromTop(20).reduced(2));
        
        // ===== CRITICAL FIX #2: Remove bottomInfoLabel from waveform viewport =====
        // It will now appear in the bottom row with the duration indicator
        // bottomInfoLabel.setBounds(labelArea.removeFromBottom(25).reduced(2));  // REMOVE THIS
        
        // ===== CRITICAL FIX #3: Always show scrollbar (disable when not needed) =====
        // This prevents height changes when scrollbar appears/disappears
        waveformViewport.setScrollBarsShown(false, true); // false for vertical, ALWAYS true for horizontal
        
        // ADD SAFETY CHECK HERE
        if (waveformComponent != nullptr && waveformContainer != nullptr)
        {
            // Update the waveform container and component size
            updateWaveformSize();
            waveformViewport.setViewPosition(0, 0);  // Reset scroll position
        }
        
        // Add 5px margin between waveform and controls
        area.removeFromTop(5);

        // ===== ROW 1: Playback controls (44px) — Down | Pitch | Up | Freeze | Loop =====
        auto row1 = area.removeFromTop(44);

        {
            // Pitch controls: Down | Pitch display | Up  (180px, 30px tall, vertically centred)
            const int pitchCtrlW = 180;
            auto pitchArea = row1.removeFromLeft(pitchCtrlW);
            auto pitchControlArea = pitchArea.withSizeKeepingCentre(pitchCtrlW, 30);
            pitchDownButton.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
            pitchLabel.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
            pitchUpButton.setBounds(pitchControlArea.removeFromLeft(60).reduced(2));
        }
        row1.removeFromLeft(8); // gap between pitch and toggle buttons
        {
            // Tune button — pitch detection (right of pitch controls)
            auto col = row1.removeFromLeft(62);
            tuneButton.setBounds(col.withSizeKeepingCentre(58, 30));
        }
        {
            // Freeze button
            auto col = row1.removeFromLeft(62);
            freezeButton.setBounds(col.withSizeKeepingCentre(58, 30));
        }
        {
            // Loop button
            auto col = row1.removeFromLeft(62);
            loopButton.setBounds(col.withSizeKeepingCentre(58, 30));
        }
        {
            // Grid snap button
            auto col = row1.removeFromLeft(62);
            gridSnapButton.setBounds(col.withSizeKeepingCentre(58, 30));
        }
        {
            // Grid resolution cycling button (compact, right of Grid)
            auto col = row1.removeFromLeft(52);
            gridResolutionButton.setBounds(col.withSizeKeepingCentre(50, 30));
        }

        // 5px gap between rows
        area.removeFromTop(5);

        // ===== ROW 2: Marker controls (60px) — <T | T> | Start | End | <T | T> | Sens | Vol =====
        auto row2 = area.removeFromTop(60);
        const int labelH = 13;

        {
            // "CRA" — transient detection toggle
            auto col = row2.removeFromLeft(44);
            detectionToggleButton.setBounds(col.withSizeKeepingCentre(42, 30));
        }
        {
            // "< T" — snap start to PREV transient
            auto col = row2.removeFromLeft(38);
            prevTransientButton.setBounds(col.withSizeKeepingCentre(36, 30));
        }
        {
            // "T >" — snap start to NEXT transient
            auto col = row2.removeFromLeft(38);
            nextTransientButton.setBounds(col.withSizeKeepingCentre(36, 30));
        }
        {
            // Start knob
            auto col = row2.removeFromLeft(60);
            startKnobLabel.setBounds(col.removeFromBottom(labelH).reduced(2, 0));
            startKnob.setBounds(col.reduced(2));
        }
        {
            // End knob
            auto col = row2.removeFromLeft(60);
            endKnobLabel.setBounds(col.removeFromBottom(labelH).reduced(2, 0));
            endKnob.setBounds(col.reduced(2));
        }
        {
            // "< T" — snap end to PREV transient
            auto col = row2.removeFromLeft(38);
            prevEndTransientButton.setBounds(col.withSizeKeepingCentre(36, 30));
        }
        {
            // "T >" — snap end to NEXT transient
            auto col = row2.removeFromLeft(38);
            nextEndTransientButton.setBounds(col.withSizeKeepingCentre(36, 30));
        }
        {
            // Sens knob — two label rows: count (T:N) above "Sens"
            auto col = row2.removeFromLeft(60);
            sensLabel.setBounds(col.removeFromBottom(labelH).reduced(2, 0));
            transientCountLabel.setBounds(col.removeFromBottom(labelH).reduced(2, 0));
            sensKnob.setBounds(col.reduced(2));
        }
        {
            // Vol knob
            auto col = row2.removeFromLeft(60);
            volumeLabel.setBounds(col.removeFromBottom(labelH).reduced(2, 0));
            volumeKnob.setBounds(col.reduced(2));
        }

        // Add margin before bottom info row
        area.removeFromTop(5);

        // ===== Bottom info row: filename | duration | pitch indicator =====
        auto bottomRow = area.removeFromTop(22);

        int filenameWidth = static_cast<int>(bottomRow.getWidth() * 0.65);
        sampleNameLabel.setBounds(bottomRow.removeFromLeft(filenameWidth).reduced(3, 0));

        int pitchIndicatorWidth = static_cast<int>(bottomRow.getWidth() * 0.50);
        bottomInfoLabel.setBounds(bottomRow.removeFromRight(pitchIndicatorWidth).reduced(3, 0));
        
        // ===== CRITICAL FIX #6: Duration on far right (remaining space) =====
        durationLabel.setBounds(bottomRow.reduced(3, 0));
        
        // Make duration label use smaller font to fit better
        durationLabel.setFont(juce::Font(11.0f));
        
        // Make pitch indicator use yellow color and bold font
        bottomInfoLabel.setFont(juce::Font(11.0f, juce::Font::bold));
        bottomInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD4A017));
    }
    
    void paint(juce::Graphics& g) override
    {
        // Draw card background
        g.setColour(juce::Colour(0xFF636363));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);

        // Draw card border
        g.setColour(juce::Colour(0xFF5A5A5A));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 8.0f, 1.5f);

        // Draw dark outline around every TextButton child
        g.setColour(juce::Colour(0xFF0A0A0A));
        for (auto* child : getChildren())
            if (dynamic_cast<juce::TextButton*>(child) != nullptr)
                g.drawRect(child->getBounds(), 1);
    }
    
    // Public methods
    juce::TextButton& getAddButton() { return addButton; }
    juce::TextButton& getPrevButton() { return prevButton; }
    juce::TextButton& getNextButton() { return nextButton; }
    
    int getMidiNote() const { return currentMidiNote; }
    int getMidiChannel() const { return currentMidiChannel; }
    int getPitchOffset() const { return pitchOffset; }
    float getVolume() const { return (float)volumeKnob.getValue(); }

    void setVolume(float volume)
    {
        volumeKnob.setValue(juce::jlimit(0.0f, 1.0f, volume), juce::dontSendNotification);
    }

    // Start point — in seconds. Pass 0 to reset to the beginning.
    double getStartPointSeconds() const
    {
        if (originalDuration <= 0.0) return 0.0;
        return startPointNormalized * originalDuration;
    }

    void setStartPoint(double seconds)
    {
        if (originalDuration <= 0.0) return;
        float norm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
        startPointNormalized = norm;
        startKnob.setValue(norm, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setStartMarker(norm);
    }

    void resetStartPoint()
    {
        startPointNormalized = 0.0f;
        startKnob.setValue(0.0, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setStartMarker(0.0f);
    }

    // End point — in seconds. Pass a value ≥ duration to snap to the very end.
    double getEndPointSeconds() const
    {
        if (originalDuration <= 0.0) return 0.0;
        return endPointNormalized * originalDuration;
    }

    void setEndPoint(double seconds)
    {
        if (originalDuration <= 0.0) return;
        float norm = (float)juce::jlimit(0.0, 1.0, seconds / originalDuration);
        endPointNormalized = norm;
        endKnob.setValue(norm, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setEndMarker(norm);
    }

    void resetEndPoint()
    {
        endPointNormalized = 1.0f;
        endKnob.setValue(1.0, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setEndMarker(1.0f);
    }

    double getTransientThreshold() const { return transientThreshold; }

    void setTransientThreshold(double threshold)
    {
        transientThreshold = juce::jlimit(1.5, 10.0, threshold);
        sensKnob.setValue(transientThreshold, juce::dontSendNotification);
        if (currentAudioFile.existsAsFile())
            detectTransients(currentAudioFile);
    }

    // Auto-tune detection result — read by persistence, written on detection or restore
    juce::String getDetectedNoteName() const { return detectedNoteName; }
    double       getDetectedFreqHz()   const { return detectedFreqHz; }
    int          getBasePitchOffset()  const { return basePitchOffset; }

    // Quietly restore the hidden base offset on session load — does NOT fire any listener.
    void setBasePitchOffset(int base)
    {
        basePitchOffset = juce::jlimit(-48, 48, base);
    }

    // Restore a previously-detected note (called on session load — does NOT fire the listener).
    void setDetectedNoteName(const juce::String& name, double freqHz)
    {
        detectedNoteName = name;
        detectedFreqHz   = freqHz;
        if (name.isNotEmpty())
        {
            tuneButton.setButtonText(name);
            tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF1A5A1A)); // dark green
            tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF00FF88)); // bright green text
            // Build bottomInfoLabel text
            juce::String info = name;
            if (freqHz > 0.0)
                info += " \xc2\xb7 " + juce::String((int)std::round(freqHz)) + " Hz";
            bottomInfoLabel.setText(info, juce::dontSendNotification);
        }
        else
        {
            tuneButton.setButtonText("Tune");
            tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
            tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
            updatePitchDisplay(pitchOffset); // restore normal bottom label
        }
    }

    bool isTransientDetectionEnabled() const { return transientDetectionEnabled; }

    // Quietly restore transient detection state on startup — does NOT fire listener.
    void setTransientDetectionEnabled(bool enabled)
    {
        transientDetectionEnabled = enabled;
        detectionToggleButton.setToggleState(enabled, juce::dontSendNotification);
        detectionToggleButton.setColour(juce::TextButton::buttonColourId,
            enabled ? juce::Colour(0xFF8B2500) : juce::Colour(0xFF4A4A4A));
        detectionToggleButton.setColour(juce::TextButton::textColourOffId,
            enabled ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF7A7A7A));
        updateTransientControlsState();
    }

    bool isGridSnapEnabled() const { return gridSnapEnabled; }

    int getGridResolutionIndex() const { return gridResolutionIndex; }

    double getGridInterval() const
    {
        static const double vals[] = { 0.001, 0.01, 0.05, 0.1, 0.5, 1.0 };
        return vals[juce::jlimit(0, numGridResolutions - 1, gridResolutionIndex)];
    }

    void setGridResolutionIndex(int index)
    {
        gridResolutionIndex = juce::jlimit(0, numGridResolutions - 1, index);
        userHasSetGridResolution = true; // marks a saved preference — blocks auto-select override
        updateGridResolutionButton();
        if (waveformComponent != nullptr)
            waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
    }

    void setGridSnapEnabled(bool enabled)
    {
        gridSnapEnabled = enabled;
        gridSnapButton.setToggleState(enabled, juce::dontSendNotification);
        updateGridResolutionButtonState();
        if (waveformComponent != nullptr)
            waveformComponent->setGridResolution(enabled, getGridInterval());
    }

    bool isLoopEnabled() const { return loopButton.getToggleState(); }

    void setLoopEnabled(bool enabled)
    {
        loopButton.setToggleState(enabled, juce::dontSendNotification);
        if (waveformComponent != nullptr)
            waveformComponent->setLoopHighlight(enabled);
    }

    bool isFreezeEnabled() const { return isFreezeActive; }

    // Called by MainComponent when a new sample is loaded.
    // Always resets freeze (it never persists). Fires freezeChanged(false) only if was active.
    void resetFreeze()
    {
        lastFreezeTapMs = 0;
        loopWasOnBeforeFreeze = false;
        if (isFreezeActive)
        {
            isFreezeActive = false;
            applyFreezeButtonStyle(false);
            listeners.call([](Listener& l) { l.freezeChanged(false); });
        }
    }

    void setPitchOffset(int offset)
    {
        // Constrain to reasonable range: ±48 semitones (4 octaves)
        if (offset >= -48 && offset <= 48 && offset != pitchOffset)
        {
            pitchOffset = offset;
            updatePitchDisplay(pitchOffset);
            
            printf("Pitch offset set to: %+d semitones\n", pitchOffset);
        }
    }

    void setMidiNote(int note)
    {
        if (note >= 0 && note <= 127 && note != currentMidiNote)
        {
            currentMidiNote = note;
            updateMidiNoteDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            printf("MIDI note set to: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
    }
    
    void setMidiChannel(int channel)
    {
        if (channel >= 0 && channel <= 16 && channel != currentMidiChannel)
        {
            currentMidiChannel = channel;
            updateMidiChannelDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiChannelChanged(currentMidiChannel); });
            
            printf("MIDI channel set to: %s\n", 
                   currentMidiChannel == 0 ? "All Channels" : juce::String(currentMidiChannel).toRawUTF8());
        }
    }
    
    void setSampleName(const juce::String& name)
    {
        sampleNameLabel.setText(name, juce::dontSendNotification);
        repaint();
    }

    void setDuration(double seconds)
    {
        originalDuration = seconds;
        
        // Apply current pitch factor to the displayed duration
        double adjustedDuration = seconds / currentPitchFactor;
        durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
        
        // Force repaint
        durationLabel.repaint();
    }
    
    void setPitchFactor(double semitones)
    {
        // Convert semitones to pitch factor (2^(semitones/12))
        currentPitchFactor = std::pow(2.0, semitones / 12.0);
        
        // Update duration display based on pitch factor
        // Pitch up = shorter duration, Pitch down = longer duration
        if (originalDuration > 0.0)
        {
            double adjustedDuration = originalDuration / currentPitchFactor;
            durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
        }
        
        // Trigger a repaint to show the "stretched" waveform visually
        // Note: We can't actually stretch the thumbnail, but we can indicate pitch change
        // by changing the waveform color or adding an overlay
        repaint();
        if (waveformComponent != nullptr)
            waveformComponent->repaint();
    }
    
    void updatePitchDisplay(int semitones)
    {
        // Show actual semitone value
        juce::String displayText;
        if (semitones == 0)
            displayText = "0";
        else if (semitones > 0)
            displayText = "+" + juce::String(semitones);
        else
            displayText = juce::String(semitones);
        
        pitchLabel.setText(displayText + " st", juce::dontSendNotification);
        
        // Calculate pitch factor correctly
        double pitchFactor = std::pow(2.0, semitones / 12.0);
        currentPitchFactor = pitchFactor;
        
        if (waveformComponent != nullptr)
            waveformComponent->setPitchFactor(pitchFactor, semitones);
        
        // Update duration display
        if (originalDuration > 0)
        {
            double adjustedDuration = originalDuration / pitchFactor;
            durationLabel.setText(juce::String(adjustedDuration, 2) + " s", juce::dontSendNotification);
            printf("Pitch: %+d, Factor: %.3f, Original: %.2f, Adjusted: %.2f\n",
                semitones, pitchFactor, originalDuration, adjustedDuration);
        }
        
        // ===== CRITICAL FIX: Always show pitch indicator (even at 0) =====
        if (semitones > 0)
            topInfoLabel.setText("COMPRESSED", juce::dontSendNotification);
        else if (semitones < 0)
            topInfoLabel.setText("EXPANDED", juce::dontSendNotification);
        else
            topInfoLabel.setText("", juce::dontSendNotification);

        // Only update bottomInfoLabel when no tune detection result is displayed
        if (detectedNoteName.isEmpty())
        {
            if (semitones > 0)
                bottomInfoLabel.setText("PITCH UP: +" + juce::String(semitones), juce::dontSendNotification);
            else if (semitones < 0)
                bottomInfoLabel.setText("PITCH DOWN: " + juce::String(semitones), juce::dontSendNotification);
            else
                bottomInfoLabel.setText("ORIGINAL WAVE", juce::dontSendNotification);
        }
        
        // Force label updates
        topInfoLabel.repaint();
        bottomInfoLabel.repaint();
        durationLabel.repaint();
        
        updateWaveformSize();
    }
        
    void setWaveform(const juce::File& audioFile)
    {
        // Reset tune detection state for the new file
        detectedNoteName = "";
        detectedFreqHz   = 0.0;
        basePitchOffset  = 0;
        tuneButton.setButtonText("Tune");
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
        tuneButton.setEnabled(true);

        // CRITICAL FIX #1: Store file FIRST
        currentAudioFile = audioFile;
        
        if (audioFile.existsAsFile())
        {
            std::unique_ptr<juce::AudioFormatReader> reader(
                formatManager.createReaderFor(audioFile));
            
            if (reader != nullptr)
            {
                originalLengthInSamples = reader->lengthInSamples;
                originalSampleRate = reader->sampleRate;
                
                // CRITICAL FIX #2: Reset scroll position BEFORE anything else
                waveformViewport.setViewPosition(0, 0);
                
                // CRITICAL FIX #3: Force waveform component to invalidate ALL cache
                if (waveformComponent != nullptr)
                {
                    waveformComponent->setFile(audioFile);  // This clears cachedTotalLength, cachedNumChannels
                }
                
                // CRITICAL FIX #4: Update container size AFTER file is set
                updateWaveformSize();

                // Reset start/end points when a new file is loaded
                resetStartPoint();
                resetEndPoint();

                // Run transient detection on the new file — only if CRA is enabled
                if (transientDetectionEnabled)
                    detectTransients(audioFile);
                else
                {
                    // CRA is OFF — clear any stale markers without running detection
                    transientPositionsSeconds.clear();
                    transientCountLabel.setText("T: 0", juce::dontSendNotification);
                    if (waveformComponent != nullptr)
                        waveformComponent->setTransients({});
                }

                // Auto-select grid resolution based on the new sample's duration
                {
                    double sampleDur = (originalSampleRate > 0.0 && originalLengthInSamples > 0)
                                       ? (double)originalLengthInSamples / originalSampleRate
                                       : 0.0;
                    if (sampleDur > 0.0)
                        autoSelectGridResolution(sampleDur);
                }

                printf("Waveform set for: %s (sample rate: %.1f kHz, length: %lld samples)\n",
                    audioFile.getFileName().toRawUTF8(),
                    originalSampleRate / 1000.0,
                    originalLengthInSamples);
            }
            else
            {
                printf("ERROR: Could not create reader for file: %s\n",
                    audioFile.getFileName().toRawUTF8());
                
                if (waveformComponent != nullptr)
                    waveformComponent->setFile(juce::File());
            }
        }
        else
        {
            originalLengthInSamples = 0;
            originalSampleRate = 0.0;
            
            if (waveformComponent != nullptr)
                waveformComponent->setFile(juce::File());
            
            repaint();
        }
    }
    
    void clearWaveform()
    {
        repaint();
        if (waveformComponent != nullptr)
            waveformComponent->repaint();
    }
    
    juce::Rectangle<int> getWaveformArea() const 
    { 
        if (waveformComponent != nullptr)
            return waveformComponent->getBounds();
        return juce::Rectangle<int>();
    }

    // Add listener for MIDI note changes
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void midiNoteChanged(int newNote) = 0;
        virtual void midiChannelChanged(int newChannel) = 0;
        virtual void learningModeChanged(bool isLearning) = 0;
        virtual void pitchOffsetChanged(int pitchOffset) = 0;
        virtual void volumeChanged(float volume) = 0;
        virtual void startPointChanged(double startPointSeconds) = 0;
        virtual void endPointChanged(double endPointSeconds) = 0;
        virtual void loopEnabledChanged(bool isLooping) = 0;
        virtual void freezeChanged(bool isFrozen) = 0;
        virtual void gridSnapChanged(bool isEnabled) = 0;
        virtual void gridResolutionChanged(int index) = 0;
        virtual void detectedNoteChanged(const juce::String& noteName, double freqHz) = 0;
        virtual void transientDetectionEnabledChanged(bool enabled) = 0;
    };
    
    void addListener(Listener* listener)
    {
        listeners.add(listener);
    }
    
    void removeListener(Listener* listener)
    {
        listeners.remove(listener);
    }
    
    // MIDI Learn public methods
    void setLearningMode(bool learning)
    {
        isLearning = learning;
        learnButton.setToggleState(learning, juce::dontSendNotification);
        if (isLearning)
        {
            learnButton.setButtonText("Learning...");
            listeners.call([this](Listener& l) { l.learningModeChanged(true); });
        }
        else
        {
            learnButton.setButtonText("Learn");
            listeners.call([this](Listener& l) { l.learningModeChanged(false); });
        }
    }
    
    bool isInLearningMode() const { return isLearning; }
    
    void setMidiNoteFromLearn(int note)
    {
        if (isLearning && note != currentMidiNote)
        {
            currentMidiNote = note;
            updateMidiNoteDisplay();
            
            // Notify listeners of the new note
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            // Automatically exit learn mode after setting the note
            if (isLearning)
            {
                isLearning = false;
                learnButton.setToggleState(false, juce::dontSendNotification);
                learnButton.setButtonText("Learn");
                listeners.call([this](Listener& l) { l.learningModeChanged(false); });
            }
            
            printf("MIDI note learned: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
    }

    private:

        // Background thread that runs YIN pitch detection
        class TuneAnalyzerThread : public juce::Thread
        {
        public:
            TuneAnalyzerThread(SampleCard& owner)
                : juce::Thread("TuneAnalyzer"), owner(owner) {}
            void run() override { owner.runTuneAnalysis(); }
        private:
            SampleCard& owner;
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TuneAnalyzerThread)
        };

        // Scrollbar height constant (Windows default ~14px, macOS ~15px)
        static constexpr int SCROLLBAR_HEIGHT = 14;
        // High-resolution WaveformComponent that renders directly from audio data
        // In SampleCard.h - Replace the WaveformComponent class with this fixed version

        class WaveformComponent : public juce::Component,
                                private juce::Timer
        {
        public:
            WaveformComponent(juce::AudioFormatManager& formatManager,
                            juce::File& currentAudioFile,
                            int& pitchOffsetRef)
                : formatManager(formatManager),
                currentAudioFile(currentAudioFile),
                pitchOffset(pitchOffsetRef)
            {
                setOpaque(true);
                startTimer(100);
            }

            void setFile(const juce::File& newFile)
            {
                if (currentAudioFile != newFile)
                {
                    currentAudioFile = newFile;
                    
                    // CRITICAL FIX: Clear ALL cached data, not just the reader
                    cachedReader.reset();
                    cachedTotalLength = 0;      // CLEAR THIS!
                    cachedNumChannels = 0;      // CLEAR THIS!
                    lastFile = juce::File();    // Force cache regeneration on next paint()
                    
                    repaint();
                }
            }

            void paint(juce::Graphics& g) override
            {
                auto bounds = getLocalBounds();
                
                // Fill background — light gray
                g.setColour(juce::Colour(0xFFE0E0E0));
                g.fillRect(bounds);

                // Draw border
                g.setColour(juce::Colour(0xFFB0B0B0));
                g.drawRect(bounds, 2);

                if (!currentAudioFile.existsAsFile())
                {
                    g.setColour(juce::Colour(0xFF888888));
                    g.setFont(juce::Font(14.0f, juce::Font::italic));
                    g.drawText("No waveform", bounds, juce::Justification::centred, true);
                    return;
                }
                
                // ===== CRITICAL FIX #1: Regenerate reader if cache is invalid =====
                if (currentAudioFile != lastFile || cachedReader == nullptr || cachedTotalLength <= 0)
                {
                    cachedReader.reset(formatManager.createReaderFor(currentAudioFile));
                    lastFile = currentAudioFile;
                    
                    if (cachedReader != nullptr)
                    {
                        cachedTotalLength = cachedReader->lengthInSamples;
                        cachedNumChannels = cachedReader->numChannels;
                        printf("WaveformComponent: NEW READER for %s (%lld samples, %d ch)\n",
                            currentAudioFile.getFileName().toRawUTF8(),
                            cachedTotalLength, cachedNumChannels);
                    }
                    else
                    {
                        printf("WaveformComponent: FAILED to create reader for %s\n",
                            currentAudioFile.getFileName().toRawUTF8());
                    }
                }
                
                if (cachedReader == nullptr || cachedTotalLength <= 0)
                {
                    g.setColour(juce::Colour(0xFF888888));
                    g.setFont(juce::Font(14.0f, juce::Font::italic));
                    g.drawText("Cannot read audio file", bounds, juce::Justification::centred, true);
                    return;
                }
                
                // Split inner area: top strip = time ruler, remaining = waveform
                const int rulerHeight = 16;
                auto innerBounds = bounds.reduced(2);
                if (innerBounds.isEmpty())
                    return;

                // Original duration for time ruler (use reader sample rate)
                double originalDuration = 0.0;
                if (cachedReader != nullptr && cachedReader->sampleRate > 0)
                    originalDuration = (double)cachedTotalLength / cachedReader->sampleRate;

                auto rulerBounds = innerBounds.removeFromTop(rulerHeight);
                auto waveformBounds = innerBounds;  // remaining area below ruler

                int renderWidth = waveformBounds.getWidth();
                int renderHeight = waveformBounds.getHeight();
                int renderTop = waveformBounds.getY();
                int renderBottom = waveformBounds.getBottom();
                int renderCenter = waveformBounds.getCentreY();
                
                // Use double for totalLength to preserve precision
                double totalLength = static_cast<double>(cachedTotalLength);
                int numChannels = cachedNumChannels;
                
                if (totalLength <= 0 || numChannels <= 0)
                {
                    g.setColour(juce::Colour(0xFF888888));
                    g.setFont(juce::Font(14.0f, juce::Font::italic));
                    g.drawText("Invalid audio data", bounds, juce::Justification::centred, true);
                    return;
                }
                
                // ===== CRITICAL FIX #2: Calculate samplesPerPixel correctly =====
                double samplesPerPixel;
                double expansionFactor = 1.0;
                
                if (pitchOffset < 0) // Pitch DOWN - EXPANDED view
                {
                    expansionFactor = std::pow(2.0, std::abs(pitchOffset) / 12.0);
                    expansionFactor = juce::jmin(expansionFactor, 16.0);

                    // renderWidth is already viewportWidth * expansionFactor (set in updateWaveformSize)
                    // so simply divide totalLength by renderWidth to get samples per pixel
                    samplesPerPixel = totalLength / renderWidth;

                    printf("EXPANDED: pitch=%d, expansion=%.2fx, renderWidth=%d, samplesPerPixel=%.4f, totalSamples=%lld\n",
                        pitchOffset, expansionFactor, renderWidth, samplesPerPixel, cachedTotalLength);
                }
                else if (pitchOffset > 0) // Pitch UP - COMPRESSED view
                {
                    double compressionFactor = std::pow(2.0, pitchOffset / 12.0);
                    compressionFactor = juce::jmin(compressionFactor, 16.0);
                    samplesPerPixel = (totalLength / renderWidth) * compressionFactor;
                }
                else // Normal view
                {
                    samplesPerPixel = totalLength / renderWidth;
                }
                
                // Buffer for reading audio data
                int bufferSize = 4096;
                juce::AudioBuffer<float> tempBuffer(numChannels, bufferSize);
                
                // ===== DRAW MAIN WAVEFORM =====
                if (numChannels > 1)
                {
                    // STEREO - Draw channels in separate vertical spaces
                    juce::Path leftPath, rightPath;
                    
                    int halfHeight = renderHeight / 2;
                    int leftTop = renderTop;
                    int leftBottom = renderTop + halfHeight;
                    int rightTop = renderTop + halfHeight;
                    int rightBottom = renderBottom;
                    
                    for (int x = 0; x < renderWidth; ++x)
                    {
                        double pixelStartSample = x * samplesPerPixel;
                        double pixelEndSample = pixelStartSample + samplesPerPixel;
                        
                        if (pixelStartSample >= totalLength)
                            break;
                        
                        pixelEndSample = juce::jmin(pixelEndSample, totalLength);
                        
                        // ===== CRITICAL FIX #3: Ensure at least 1 sample is read =====
                        double sampleRange = pixelEndSample - pixelStartSample;
                        int numSamples = juce::jmax(1, static_cast<int>(std::ceil(sampleRange)));

                        // Ensure we don't read past the end of the file
                        if (pixelStartSample + numSamples > totalLength)
                            numSamples = static_cast<int>(totalLength - pixelStartSample);

                        // Cap to buffer size — at high pitch-up the compression factor
                        // makes samplesPerPixel very large; capping is safe because we
                        // only need a min/max representative sample for each pixel.
                        numSamples = juce::jmin(numSamples, bufferSize);

                        if (numSamples <= 0)
                            continue;

                        // Read audio data
                        bool readSuccess = cachedReader->read(&tempBuffer, 0, numSamples,
                                                            static_cast<juce::int64>(pixelStartSample),
                                                            true, true);
                        
                        if (!readSuccess)
                            continue;
                        
                        float leftMin = 1.0f, leftMax = -1.0f;
                        float rightMin = 1.0f, rightMax = -1.0f;
                        
                        for (int s = 0; s < numSamples; ++s)
                        {
                            float leftVal = tempBuffer.getSample(0, s);
                            leftMin = std::min(leftMin, leftVal);
                            leftMax = std::max(leftMax, leftVal);
                            
                            if (numChannels > 1)
                            {
                                float rightVal = tempBuffer.getSample(1, s);
                                rightMin = std::min(rightMin, rightVal);
                                rightMax = std::max(rightMax, rightVal);
                            }
                        }
                        
                        float xPos = waveformBounds.getX() + x;
                        
                        // Left channel
                        float leftCenterY = leftTop + halfHeight * 0.5f;
                        float leftHalfHeight = halfHeight * 0.5f;
                        float leftYMin = leftCenterY - (leftMin * leftHalfHeight);
                        float leftYMax = leftCenterY - (leftMax * leftHalfHeight);
                        float leftYTop = std::min(leftYMin, leftYMax);
                        float leftYBottom = std::max(leftYMin, leftYMax);
                        leftYTop = juce::jlimit((float)leftTop + 1.0f, (float)leftBottom - 1.0f, leftYTop);
                        leftYBottom = juce::jlimit((float)leftTop + 1.0f, (float)leftBottom - 1.0f, leftYBottom);
                        
                        leftPath.startNewSubPath(xPos, leftYTop);
                        leftPath.lineTo(xPos, leftYBottom);
                        
                        // Right channel
                        float rightCenterY = rightTop + halfHeight * 0.5f;
                        float rightHalfHeight = halfHeight * 0.5f;
                        float rightYMin = rightCenterY - (rightMin * rightHalfHeight);
                        float rightYMax = rightCenterY - (rightMax * rightHalfHeight);
                        float rightYTop = std::min(rightYMin, rightYMax);
                        float rightYBottom = std::max(rightYMin, rightYMax);
                        rightYTop = juce::jlimit((float)rightTop + 1.0f, (float)rightBottom - 1.0f, rightYTop);
                        rightYBottom = juce::jlimit((float)rightTop + 1.0f, (float)rightBottom - 1.0f, rightYBottom);
                        
                        rightPath.startNewSubPath(xPos, rightYTop);
                        rightPath.lineTo(xPos, rightYBottom);
                    }
                    
                    // Left channel — dark charcoal gradient, lighter at edges
                    {
                        juce::ColourGradient grad(juce::Colour(0xFF606060), 0.0f, (float)leftTop,
                                                  juce::Colour(0xFF606060), 0.0f, (float)leftBottom, false);
                        grad.addColour(0.5, juce::Colour(0xFF1A1A1A));
                        g.setGradientFill(grad);
                        g.strokePath(leftPath, juce::PathStrokeType(1.5f));
                    }

                    // Right channel — same gradient in its vertical range
                    {
                        juce::ColourGradient grad(juce::Colour(0xFF606060), 0.0f, (float)rightTop,
                                                  juce::Colour(0xFF606060), 0.0f, (float)rightBottom, false);
                        grad.addColour(0.5, juce::Colour(0xFF1A1A1A));
                        g.setGradientFill(grad);
                        g.strokePath(rightPath, juce::PathStrokeType(1.5f));
                    }

                    // Channel divider
                    g.setColour(juce::Colour(0xFF555555));
                    g.drawHorizontalLine(renderTop + halfHeight,
                                        waveformBounds.getX(), waveformBounds.getRight());

                    g.setColour(juce::Colour(0xFF555555));
                    g.setFont(juce::Font(10.0f));
                    g.drawText("L", waveformBounds.getX() + 5, leftTop + 2, 20, 15,
                            juce::Justification::left);
                    g.drawText("R", waveformBounds.getX() + 5, rightTop + 2, 20, 15,
                            juce::Justification::left);
                }
                else
                {
                    // MONO - draw single waveform
                    juce::Path waveformPath;

                    for (int x = 0; x < renderWidth; ++x)
                    {
                        double pixelStartSample = x * samplesPerPixel;
                        double pixelEndSample = pixelStartSample + samplesPerPixel;

                        if (pixelStartSample >= totalLength)
                            break;

                        pixelEndSample = juce::jmin(pixelEndSample, totalLength);

                        // ===== CRITICAL FIX #3: Ensure at least 1 sample is read =====
                        double sampleRange = pixelEndSample - pixelStartSample;
                        int numSamples = juce::jmax(1, static_cast<int>(std::ceil(sampleRange)));

                        if (pixelStartSample + numSamples > totalLength)
                            numSamples = static_cast<int>(totalLength - pixelStartSample);

                        // Cap to buffer size — prevents overflow at high pitch-up compression
                        numSamples = juce::jmin(numSamples, bufferSize);

                        if (numSamples <= 0)
                            continue;

                        bool readSuccess = cachedReader->read(&tempBuffer, 0, numSamples,
                                                            static_cast<juce::int64>(pixelStartSample),
                                                            true, true);

                        if (!readSuccess)
                            continue;

                        float minVal = 1.0f;
                        float maxVal = -1.0f;

                        for (int s = 0; s < numSamples; ++s)
                        {
                            for (int ch = 0; ch < numChannels; ++ch)
                            {
                                float val = tempBuffer.getSample(ch, s);
                                minVal = std::min(minVal, val);
                                maxVal = std::max(maxVal, val);
                            }
                        }

                        float xPos = waveformBounds.getX() + x;
                        float centerY = renderTop + renderHeight * 0.5f;
                        float halfHeight = renderHeight * 0.5f;
                        float yMin = centerY - (minVal * halfHeight);
                        float yMax = centerY - (maxVal * halfHeight);
                        float yTop = std::min(yMin, yMax);
                        float yBottom = std::max(yMin, yMax);
                        yTop = juce::jlimit(renderTop + 1.0f, renderBottom - 1.0f, yTop);
                        yBottom = juce::jlimit(renderTop + 1.0f, renderBottom - 1.0f, yBottom);

                        // Draw vertical bar from min to max for each pixel (full waveform)
                        waveformPath.startNewSubPath(xPos, yTop);
                        waveformPath.lineTo(xPos, yBottom);
                    }
                    
                    // Dark charcoal gradient — lighter at top/bottom edges, darkest in center
                    {
                        juce::ColourGradient grad(juce::Colour(0xFF606060), 0.0f, (float)renderTop,
                                                  juce::Colour(0xFF606060), 0.0f, (float)renderBottom, false);
                        grad.addColour(0.5, juce::Colour(0xFF1A1A1A));
                        g.setGradientFill(grad);
                        g.strokePath(waveformPath, juce::PathStrokeType(1.5f));
                    }
                }
                
                // Draw center line
                g.setColour(juce::Colour(0xFF555555).withAlpha(0.5f));
                g.drawHorizontalLine(renderCenter, waveformBounds.getX(), waveformBounds.getRight());

                // ===== TIME RULER AND GRID LINES =====
                float actualWaveformWidth = getActualWaveformWidth(renderWidth);

                if (originalDuration > 0.0 && actualWaveformWidth > 0.0f)
                {
                    float pixelsPerSecond = actualWaveformWidth / (float)originalDuration;
                    double majorInterval, minorInterval;
                    getTickIntervals(pixelsPerSecond, majorInterval, minorInterval);

                    // --- Grid lines over waveform — use selected resolution when snap is ON ---
                    double gridMajor, gridMinor;
                    if (gridSnapActive && gridResolutionSeconds > 0.0)
                    {
                        gridMajor = gridResolutionSeconds;
                        gridMinor = gridResolutionSeconds / 2.0;
                    }
                    else
                    {
                        gridMajor = majorInterval;
                        gridMinor = minorInterval;
                    }

                    float majorPx = (gridMajor > 0.0 && originalDuration > 0.0)
                                    ? (float)(gridMajor / originalDuration) * actualWaveformWidth
                                    : 0.0f;
                    float minorPx = majorPx * 0.5f;

                    if (majorPx >= 2.0f)
                    {
                        for (double t = gridMinor; t < originalDuration; t += gridMinor)
                        {
                            int n = (int)std::round(t / gridMinor);
                            bool isMajor = (n % 2 == 0);
                            if (minorPx < 2.0f && !isMajor) continue; // skip minor when too dense
                            float x = waveformBounds.getX() + (float)(t / originalDuration * actualWaveformWidth);
                            if (x >= waveformBounds.getRight()) break;
                            g.setColour(juce::Colour(0xFF000000).withAlpha(isMajor ? 0.15f : 0.08f));
                            g.drawVerticalLine((int)(x + 0.5f), waveformBounds.getY(), waveformBounds.getBottom());
                        }
                    }

                    // --- Ruler background — slightly darker than waveform ---
                    g.setColour(juce::Colour(0xFFC8C8C8));
                    g.fillRect(rulerBounds);
                    g.setColour(juce::Colour(0xFFAAAAAA));
                    g.drawRect(rulerBounds, 1);

                    g.setFont(juce::Font(9.0f));

                    // "0" at origin
                    g.setColour(juce::Colour(0xFF2A2A2A));
                    g.drawVerticalLine(rulerBounds.getX(),
                                       rulerBounds.getBottom() - 8, rulerBounds.getBottom() - 1);
                    g.drawText("0", rulerBounds.getX() + 2, rulerBounds.getY(),
                               20, rulerHeight - 2, juce::Justification::centredLeft, false);

                    // Minor and major ticks + labels
                    for (double t = minorInterval; t < originalDuration; t += minorInterval)
                    {
                        int n = (int)std::round(t / minorInterval);
                        bool isMajor = (n % 2 == 0);
                        float x = rulerBounds.getX() + (float)(t / originalDuration * actualWaveformWidth);
                        if (x >= rulerBounds.getRight()) break;

                        g.setColour(isMajor ? juce::Colour(0xFF2A2A2A) : juce::Colour(0xFF555555));
                        int tickH = isMajor ? 8 : 4;
                        g.drawVerticalLine((int)(x + 0.5f),
                                           rulerBounds.getBottom() - tickH, rulerBounds.getBottom() - 1);

                        if (isMajor)
                        {
                            double rounded = std::round(t * 1000.0) / 1000.0;
                            juce::String label;
                            if (rounded >= 10.0 || rounded == (double)(int)(rounded + 0.5))
                                label = juce::String((int)(rounded + 0.5));
                            else
                                label = juce::String(rounded, majorInterval < 0.1 ? 2 : 1);

                            g.setColour(juce::Colour(0xFF2A2A2A));
                            g.drawText(label, (int)(x + 0.5f) - 15, rulerBounds.getY(),
                                       30, rulerHeight - 4, juce::Justification::centred, false);
                        }
                    }
                }

                // Draw transient positions as small tick marks at waveform top
                if (!transientPositionsNormalized.empty())
                {
                    g.setColour(juce::Colour(0xFFE84A1A).withAlpha(0.75f));
                    for (float tn : transientPositionsNormalized)
                    {
                        float tx = waveformBounds.getX() + tn * actualWaveformWidth;
                        if (tx >= waveformBounds.getRight()) break;
                        g.drawVerticalLine((int)(tx + 0.5f),
                                           waveformBounds.getY(),
                                           waveformBounds.getY() + 6);
                        // Small downward triangle cap
                        juce::Path tick;
                        tick.addTriangle(tx - 2.5f, (float)waveformBounds.getY(),
                                         tx + 2.5f, (float)waveformBounds.getY(),
                                         tx,         (float)waveformBounds.getY() + 5.0f);
                        g.fillPath(tick);
                    }
                }

                // Draw loop region highlight (semi-transparent overlay between start and end)
                if (loopHighlightEnabled)
                {
                    float hlStartX = waveformBounds.getX() + startMarkerNormalized * actualWaveformWidth;
                    float hlEndX   = waveformBounds.getX() + endMarkerNormalized   * actualWaveformWidth;
                    g.setColour(juce::Colour(0xFFB4FF00).withAlpha(0.20f)); // rgba(180,255,0,0.20)
                    g.fillRect(juce::Rectangle<float>(hlStartX, (float)waveformBounds.getY(),
                                                      hlEndX - hlStartX, (float)waveformBounds.getHeight()));
                }

                // Draw start point marker — deep red normally, flash orange on transient snap
                // Flash alternates bright/dim each 100ms tick for a pulse effect
                bool flashOn = (markerFlashCountdown > 0) && (markerFlashCountdown % 2 != 0);
                juce::Colour startMarkerColour = flashOn
                    ? juce::Colour(0xFFE84A1A)  // orange-red flash matching start snap buttons
                    : juce::Colour(0xFFCC0000); // normal deep red
                float markerX = waveformBounds.getX() + startMarkerNormalized * actualWaveformWidth;
                g.setColour(startMarkerColour.withAlpha(0.9f));
                g.drawLine(markerX, (float)rulerBounds.getY(),
                           markerX, (float)waveformBounds.getBottom(), 2.0f);
                {
                    juce::Path handle;
                    handle.addTriangle(markerX - 5, (float)rulerBounds.getY(),
                                       markerX + 5, (float)rulerBounds.getY(),
                                       markerX,     (float)rulerBounds.getY() + 8);
                    g.fillPath(handle);
                }

                // Draw end point marker — cyan normally, flash orange on transient snap
                bool endFlashOn = (endMarkerFlashCountdown > 0) && (endMarkerFlashCountdown % 2 != 0);
                juce::Colour endMarkerColour = endFlashOn
                    ? juce::Colour(0xFF00B4D8)  // cyan flash matching end snap buttons
                    : juce::Colour(0xFF00AACC); // normal cyan
                float endMarkerX = waveformBounds.getX() + endMarkerNormalized * actualWaveformWidth;
                g.setColour(endMarkerColour.withAlpha(0.9f));
                g.drawLine(endMarkerX, (float)rulerBounds.getY(),
                           endMarkerX, (float)waveformBounds.getBottom(), 2.0f);
                {
                    juce::Path endHandle;
                    endHandle.addTriangle(endMarkerX - 5, (float)waveformBounds.getBottom(),
                                          endMarkerX + 5, (float)waveformBounds.getBottom(),
                                          endMarkerX,     (float)waveformBounds.getBottom() - 8);
                    g.fillPath(endHandle);
                }
            }

            void setPitchFactor(double factor, int semitones)
            {
                currentPitchFactor = factor;
                currentSemitones = semitones;
                repaint();
            }

            // Set start marker position (0.0 = start, 1.0 = end)
            void setStartMarker(float normalized)
            {
                startMarkerNormalized = juce::jlimit(0.0f, endMarkerNormalized - 0.001f, normalized);
                repaint();
            }

            float getStartMarker() const { return startMarkerNormalized; }

            // Set end marker position (0.0 = start, 1.0 = end)
            void setEndMarker(float normalized)
            {
                endMarkerNormalized = juce::jlimit(startMarkerNormalized + 0.001f, 1.0f, normalized);
                repaint();
            }

            float getEndMarker() const { return endMarkerNormalized; }

            void setLoopHighlight(bool enabled)
            {
                loopHighlightEnabled = enabled;
                repaint();
            }

            // Push snap-grid state so paint() draws lines at the chosen resolution.
            void setGridResolution(bool snapActive, double resolutionSec)
            {
                gridSnapActive       = snapActive;
                gridResolutionSeconds = resolutionSec;
                repaint();
            }

            // Transient markers — normalized positions [0,1]
            void setTransients(const std::vector<float>& positions)
            {
                transientPositionsNormalized = positions;
                repaint();
            }

            // Briefly flash the start marker orange to show a transient snap occurred
            void flashStartMarker()
            {
                markerFlashCountdown = 6; // ~600ms at 100ms timer intervals
                repaint();
            }

            // Briefly flash the end marker orange to show a transient snap occurred
            void flashEndMarker()
            {
                endMarkerFlashCountdown = 6;
                repaint();
            }

            // Callbacks: invoked when user drags either marker
            std::function<void(float)> onMarkerDragged;
            std::function<void(float)> onEndMarkerDragged;

            void mouseDown(const juce::MouseEvent& event) override
            {
                // Pick the marker closest to the click position
                auto bounds = getLocalBounds().reduced(2);
                float actualWidth = getActualWaveformWidth(bounds.getWidth());
                float startX = bounds.getX() + startMarkerNormalized * actualWidth;
                float endX   = bounds.getX() + endMarkerNormalized   * actualWidth;
                float dStart = std::abs((float)event.x - startX);
                float dEnd   = std::abs((float)event.x - endX);
                currentDragTarget = (dStart <= dEnd) ? DragTarget::Start : DragTarget::End;
                updateMarkerFromMouse(event.x);
            }

            void mouseDrag(const juce::MouseEvent& event) override
            {
                if (currentDragTarget != DragTarget::None)
                    updateMarkerFromMouse(event.x);
            }

            void mouseUp(const juce::MouseEvent& event) override
            {
                currentDragTarget = DragTarget::None;
            }

        private:
            // Choose "nice" tick intervals so major ticks are at least 40px apart.
            static void getTickIntervals(float pixelsPerSecond, double& majorOut, double& minorOut)
            {
                static constexpr double niceVals[] = {
                    0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0
                };
                majorOut = 300.0;
                for (auto v : niceVals)
                {
                    if (pixelsPerSecond * (float)v >= 40.0f)
                    {
                        majorOut = v;
                        break;
                    }
                }
                minorOut = majorOut / 2.0;
            }

            // Returns the pixel width actually covered by waveform data.
            // For pitch-up the waveform only fills the left portion of renderWidth;
            // for pitch-down/normal the waveform fills the full renderWidth.
            float getActualWaveformWidth(int renderWidth) const
            {
                if (pitchOffset > 0)
                {
                    double compressionFactor = std::pow(2.0, pitchOffset / 12.0);
                    compressionFactor = juce::jmin(compressionFactor, 16.0);
                    return (float)(renderWidth / compressionFactor);
                }
                return (float)renderWidth;
            }

            void updateMarkerFromMouse(int mouseX)
            {
                auto bounds = getLocalBounds().reduced(2);
                if (bounds.getWidth() <= 0) return;
                float actualWidth = getActualWaveformWidth(bounds.getWidth());
                float rawNorm = (mouseX - bounds.getX()) / actualWidth;

                const float minGap = 0.001f;

                if (currentDragTarget == DragTarget::Start)
                {
                    float newNorm = juce::jlimit(0.0f, endMarkerNormalized - minGap, rawNorm);
                    startMarkerNormalized = newNorm;
                    repaint();
                    if (onMarkerDragged)
                        onMarkerDragged(newNorm);
                }
                else if (currentDragTarget == DragTarget::End)
                {
                    float newNorm = juce::jlimit(startMarkerNormalized + minGap, 1.0f, rawNorm);
                    endMarkerNormalized = newNorm;
                    repaint();
                    if (onEndMarkerDragged)
                        onEndMarkerDragged(newNorm);
                }
            }

            void timerCallback() override
            {
                if (currentAudioFile != lastFile)
                {
                    cachedReader.reset();
                    lastFile = currentAudioFile;
                    repaint();
                }
                if (markerFlashCountdown > 0)
                {
                    --markerFlashCountdown;
                    repaint();
                }
                if (endMarkerFlashCountdown > 0)
                {
                    --endMarkerFlashCountdown;
                    repaint();
                }
            }

            juce::AudioFormatManager& formatManager;
            juce::File& currentAudioFile;
            int& pitchOffset;
            double currentPitchFactor = 1.0;
            int currentSemitones = 0;
            juce::File lastFile;
            std::unique_ptr<juce::AudioFormatReader> cachedReader;
            juce::int64 cachedTotalLength = 0;
            int cachedNumChannels = 0;
            float startMarkerNormalized = 0.0f;
            float endMarkerNormalized   = 1.0f;
            bool loopHighlightEnabled   = false;

            // Grid snap display state
            bool   gridSnapActive        = false;
            double gridResolutionSeconds = 1.0;

            // Transient detection display
            std::vector<float> transientPositionsNormalized;
            int markerFlashCountdown    = 0; // start marker flash countdown (100ms ticks)
            int endMarkerFlashCountdown = 0; // end marker flash countdown

            enum class DragTarget { None, Start, End };
            DragTarget currentDragTarget = DragTarget::None;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
        };
    
    
    
    void adjustMidiNote(int delta)
    {
        int newNote = currentMidiNote + delta;
        
        // Constrain to valid MIDI range (0-127)
        if (newNote >= 0 && newNote <= 127)
        {
            currentMidiNote = newNote;
            updateMidiNoteDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiNoteChanged(currentMidiNote); });
            
            printf("MIDI note changed to: %d (%s)\n", 
                   currentMidiNote, 
                   juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true).toRawUTF8());
        }
    }
    
    void adjustMidiChannel(int delta)
    {
        int newChannel = currentMidiChannel + delta;
        
        // Allow channel 0 for "All Channels", then 1-16
        if (delta > 0) // Incrementing
        {
            if (currentMidiChannel == 16)
                newChannel = 0; // Wrap to All Channels
            else if (currentMidiChannel == 0)
                newChannel = 1; // From All to channel 1
        }
        else if (delta < 0) // Decrementing
        {
            if (currentMidiChannel == 1)
                newChannel = 0; // Wrap to All Channels
            else if (currentMidiChannel == 0)
                newChannel = 16; // From All to channel 16
        }
        
        // Ensure valid range
        if (newChannel >= 0 && newChannel <= 16)
        {
            currentMidiChannel = newChannel;
            updateMidiChannelDisplay();
            
            // Notify listeners
            listeners.call([this](Listener& l) { l.midiChannelChanged(currentMidiChannel); });
            
            printf("MIDI channel changed to: %s\n", 
                   currentMidiChannel == 0 ? "All Channels" : juce::String(currentMidiChannel).toRawUTF8());
        }
    }
    
    void updateMidiNoteDisplay()
    {
        juce::String noteName = juce::MidiMessage::getMidiNoteName(currentMidiNote, true, true, true);
        midiNoteLabel.setText(juce::String(currentMidiNote) + " (" + noteName + ")", 
                              juce::dontSendNotification);
    }
    
    void updateMidiChannelDisplay()
    {
        if (currentMidiChannel == 0)
            midiChannelLabel.setText("Ch All", juce::dontSendNotification);
        else
            midiChannelLabel.setText("Ch " + juce::String(currentMidiChannel), 
                                     juce::dontSendNotification);
    }
    
    void toggleLearnMode()
    {
        isLearning = !isLearning;
        learnButton.setToggleState(isLearning, juce::dontSendNotification);
        
        if (isLearning)
        {
            learnButton.setButtonText("Learning...");
            // Notify listeners that we're entering learn mode
            listeners.call([this](Listener& l) { l.learningModeChanged(true); });
        }
        else
        {
            learnButton.setButtonText("Learn");
            // Notify listeners that we're exiting learn mode
            listeners.call([this](Listener& l) { l.learningModeChanged(false); });
        }
    }
    
    void notifyStartPointChanged()
    {
        double seconds = (originalDuration > 0.0) ? startPointNormalized * originalDuration : 0.0;
        listeners.call([&](Listener& l) { l.startPointChanged(seconds); });
    }

    void notifyEndPointChanged()
    {
        double seconds = (originalDuration > 0.0) ? endPointNormalized * originalDuration : originalDuration;
        listeners.call([&](Listener& l) { l.endPointChanged(seconds); });
    }

    // Helper method to update pitch button tooltips
    void updatePitchButtonLabels()
    {
        // Update button tooltips to reflect the new behavior
        pitchDownButton.setTooltip("Lower pitch (longer duration)");
        pitchUpButton.setTooltip("Higher pitch (shorter duration)");
    }
    
void adjustPitchDown() 
{
    int newOffset = pitchOffset - 1;  // CORRECT: subtract for pitch down
    
    if (newOffset >= -48 && newOffset <= 48)
    {
        pitchOffset = newOffset;
        updatePitchDisplay(pitchOffset);
        listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });
        
        // Just report the value without "unexpected" warnings
        if (pitchOffset < 0)
            printf("✓ Pitch DOWN: %d semitones (lower pitch, longer duration)\n", pitchOffset);
        else if (pitchOffset > 0)
            printf("→ Pitch DOWN: +%d semitones (moving toward zero)\n", pitchOffset);
        else
            printf("Pitch reset to 0\n");
    }
}

void adjustPitchUp() 
{
    int newOffset = pitchOffset + 1;  // CORRECT: add for pitch up
    
    if (newOffset >= -48 && newOffset <= 48)
    {
        pitchOffset = newOffset;
        updatePitchDisplay(pitchOffset);
        listeners.call([this](Listener& l) { l.pitchOffsetChanged(pitchOffset); });
        
        if (pitchOffset > 0)
            printf("✓ Pitch UP: +%d semitones (higher pitch, shorter duration)\n", pitchOffset);
        else if (pitchOffset < 0)
            printf("→ Pitch UP: %d semitones (moving toward zero)\n", pitchOffset);
        else
            printf("Pitch reset to 0\n");
    }
}
    
    
    // =========================================================================
    // Transient detection and snapping

    // Analyse the audio file and build the transientPositionsSeconds list.
    // Uses a 512-sample sliding window; marks onset when RMS jumps > 4× previous window.
    // NOTE: duration is computed from the reader — does NOT depend on originalDuration so
    // this is safe to call from setWaveform() before setDuration() has been called.
    void detectTransients(const juce::File& audioFile)
    {
        transientPositionsSeconds.clear();
        if (waveformComponent != nullptr)
            waveformComponent->setTransients({});

        if (!audioFile.existsAsFile())
            return;

        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(audioFile));
        if (reader == nullptr)
            return;

        const double sr           = reader->sampleRate;
        const juce::int64 totalSamples = reader->lengthInSamples;

        // Compute duration directly from the reader — independent of originalDuration
        const double duration = (sr > 0.0 && totalSamples > 0)
                                    ? (double)totalSamples / sr
                                    : 0.0;
        if (duration <= 0.0)
            return;

        const int    windowSize    = 512;
        const double threshold     = transientThreshold;  // controlled by Sens knob
        const double minGapSeconds = 0.02;  // ignore transients closer than 20 ms

        const int numCh = juce::jmin((int)reader->numChannels, 2);

        juce::AudioBuffer<float> buf(numCh, windowSize);
        double prevRMS        = 0.0;
        double lastTransientT = -1.0;

        for (juce::int64 pos = 0; pos + windowSize <= totalSamples; pos += windowSize)
        {
            reader->read(&buf, 0, windowSize, pos, true, true);

            double sumSq = 0.0;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* data = buf.getReadPointer(ch);
                for (int i = 0; i < windowSize; ++i)
                    sumSq += (double)data[i] * data[i];
            }
            double rms = std::sqrt(sumSq / (windowSize * numCh));

            if (prevRMS > 0.001 && rms > prevRMS * threshold)
            {
                double t = (double)pos / sr;
                if (t - lastTransientT >= minGapSeconds)
                {
                    transientPositionsSeconds.push_back(t);
                    lastTransientT = t;
                }
            }
            prevRMS = rms;
        }

        const int transientCount = (int)transientPositionsSeconds.size();
        printf("[TRANSIENT] Startup/load complete — %d transients detected in '%s' (%.2fs, thresh=%.1fx)\n",
               transientCount,
               audioFile.getFileName().toRawUTF8(),
               duration,
               threshold);

        transientCountLabel.setText("T: " + juce::String(transientCount), juce::dontSendNotification);

        // Normalise against the reader-derived duration (not originalDuration)
        if (waveformComponent != nullptr)
        {
            std::vector<float> normalised;
            normalised.reserve(transientPositionsSeconds.size());
            for (double t : transientPositionsSeconds)
                normalised.push_back((float)(t / duration));
            waveformComponent->setTransients(normalised);
        }
    }

    // Move end marker to the nearest transient BEFORE current end (but after start marker).
    void snapEndToPrevTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentEnd   = endPointNormalized   * originalDuration;
        double currentStart = startPointNormalized  * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
        {
            // Must be before current end AND strictly after start marker
            if (t < currentEnd - epsilon && t > currentStart + epsilon)
                if (t > best) best = t;
        }

        if (best >= 0.0)
        {
            setEndPoint(best);
            notifyEndPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashEndMarker();
            printf("[TRANSIENT] End snap prev → %.3fs\n", best);
        }
        else
        {
            // Nothing valid — flash button briefly to indicate no target
            prevEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                prevEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
            });
        }
    }

    // Move end marker to the nearest transient AFTER current end.
    void snapEndToNextTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentEnd = endPointNormalized * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
        {
            if (t > currentEnd + epsilon && (best < 0.0 || t < best))
                best = t;
        }

        if (best >= 0.0 && best < originalDuration)
        {
            setEndPoint(best);
            notifyEndPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashEndMarker();
            printf("[TRANSIENT] End snap next → %.3fs\n", best);
        }
        else
        {
            nextEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                nextEndTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF00B4D8));
            });
        }
    }

    // Move start marker to the nearest transient BEFORE current start.
    void snapToPrevTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentStart = startPointNormalized * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
            if (t < currentStart - epsilon && t > best)
                best = t;

        if (best >= 0.0)
        {
            setStartPoint(best);
            notifyStartPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashStartMarker();
            printf("[TRANSIENT] Start snap prev → %.3fs\n", best);
        }
        else
        {
            prevTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                prevTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
            });
        }
    }

    // Move start marker to the nearest transient AFTER current start (but before end marker).
    void snapToNextTransient()
    {
        if (transientPositionsSeconds.empty() || originalDuration <= 0.0)
            return;

        double currentStart = startPointNormalized * originalDuration;
        double currentEnd   = endPointNormalized   * originalDuration;
        const double epsilon = 0.005;

        double best = -1.0;
        for (double t : transientPositionsSeconds)
        {
            // Must be after current start AND strictly before end marker
            if (t > currentStart + epsilon && t < currentEnd - epsilon)
                if (best < 0.0 || t < best) best = t;
        }

        if (best >= 0.0)
        {
            setStartPoint(best);
            notifyStartPointChanged();
            if (waveformComponent != nullptr)
                waveformComponent->flashStartMarker();
            printf("[TRANSIENT] Start snap next → %.3fs\n", best);
        }
        else
        {
            nextTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF884400));
            juce::Timer::callAfterDelay(300, [this]
            {
                nextTransientButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE84A1A));
            });
        }
    }

/*     void updateWaveformSize()
    {
        if (waveformComponent == nullptr || waveformContainer == nullptr)
            return;
        
        auto viewportBounds = waveformViewport.getLocalBounds();
        int containerWidth = viewportBounds.getWidth();
        
        if (pitchOffset != 0)
        {

            double pitchFactor = std::pow(2.0, pitchOffset / 12.0);
            containerWidth = (int)(viewportBounds.getWidth() * pitchFactor);
            // containerWidth = (int)(viewportBounds.getWidth() / pitchFactor);

        }
        
        // Ensure minimum width
        containerWidth = juce::jmax(containerWidth, viewportBounds.getWidth());   

        // Set container size
        waveformContainer->setBounds(0, 0, containerWidth, viewportBounds.getHeight());
        
        // Set waveform component to fill the container
        waveformComponent->setBounds(waveformContainer->getLocalBounds().reduced(2));

        // Always anchor to left side
        waveformViewport.setViewPosition(0, 0);
    }  */
    
    void updateWaveformSize()
    {
        if (waveformComponent == nullptr || waveformContainer == nullptr)
            return;
        
        auto viewportBounds = waveformViewport.getLocalBounds();
        if (viewportBounds.getWidth() <= 0 || viewportBounds.getHeight() <= 0)
            return;
        
        // ===== CRITICAL FIX: Account for scrollbar height in container =====
        // Container height = viewport height - scrollbar height (so waveform isn't squished)
        int containerHeight = viewportBounds.getHeight() - SCROLLBAR_HEIGHT;
        containerHeight = juce::jmax(1, containerHeight);  // Ensure positive
        
        int containerWidth;
        if (pitchOffset < 0) // Pitch DOWN - EXPAND
        {
            double expansionFactor = std::pow(2.0, std::abs(pitchOffset) / 12.0);
            expansionFactor = juce::jmin(expansionFactor, 16.0);
            containerWidth = (int)(viewportBounds.getWidth() * expansionFactor);
        }
        else
        {
            containerWidth = viewportBounds.getWidth();
        }
        
        // Cap maximum width
        const int maxWidth = 11200;
        containerWidth = juce::jmin(containerWidth, maxWidth);
        containerWidth = juce::jmax(containerWidth, viewportBounds.getWidth());
        
        // Set container bounds with adjusted height
        waveformContainer->setBounds(0, 0, containerWidth, containerHeight);
        waveformComponent->setBounds(waveformContainer->getLocalBounds().reduced(2));
        
        // ===== CRITICAL FIX #3: Always show scrollbar (true = always visible) =====
        waveformViewport.setScrollBarsShown(false, true);
        
        // Reset scroll position
        waveformViewport.setViewPosition(0, 0);
        waveformComponent->repaint();
    }

    // UI Components
    juce::TextButton addButton{"+"};
    juce::TextButton prevButton{"Prev"};
    juce::TextButton nextButton{"Next"};
    juce::TextButton learnButton{"Learn"};  // MIDI Learn button
    juce::Viewport waveformViewport;
    std::unique_ptr<juce::Component> waveformContainer;
    std::unique_ptr<WaveformComponent> waveformComponent;
    
    // Fixed info labels for waveform display (don't scroll with viewport)
    juce::Label topInfoLabel;      // For compression/stretching percentage
    juce::Label bottomInfoLabel;   // For pitch information
    
    // MIDI Note controls
    juce::Label midiNoteLabel;
    int currentMidiNote = 60;  // Default to Middle C
    
    // MIDI Channel controls
    juce::TextButton channelDownButton{"-"};
    juce::Label midiChannelLabel;
    juce::TextButton channelUpButton{"+"};
    int currentMidiChannel = 1;  // Default to channel 1
    
    juce::Label sampleNameLabel;
    juce::Label durationLabel;
    
    // Pitch adjustment controls
    juce::TextButton pitchDownButton{"Down"};  // Lower pitch = negative semitones = longer duration
    juce::Label pitchLabel;
    juce::TextButton pitchUpButton{"Up"};      // Higher pitch = positive semitones = shorter duration
    int pitchOffset = 0;  // Pitch offset in semitones

    // Loop / Freeze / Grid / Tune buttons
    juce::TextButton loopButton;
    juce::TextButton freezeButton { "Freeze" };
    juce::TextButton gridSnapButton { "Grid" };
    juce::TextButton gridResolutionButton { "1s" };
    juce::TextButton tuneButton { "Tune" };
    bool gridSnapEnabled = false;
    int  gridResolutionIndex = 5;   // default = 1s (index into the 6-step table)
    bool userHasSetGridResolution = false; // true once user manually clicks or a saved value is restored
    static constexpr int numGridResolutions = 6;

    // Auto-tune / pitch detection state
    juce::String detectedNoteName;               // e.g. "D3" — empty until Tune is run
    double       detectedFreqHz   = 0.0;         // detected fundamental frequency in Hz
    int          basePitchOffset  = 0;           // hidden correction from Tune; user sees 0 st = this note
    std::atomic<bool> isTuneRunning { false };
    // Analysis parameters captured on message thread before background thread starts
    juce::File   tuneFile;
    double       tuneStartSec     = 0.0;
    double       tuneEndSec       = 0.0;
    int          tuneRootMidiNote = 60;
    std::unique_ptr<TuneAnalyzerThread> tuneThread;

    // Transient detection toggle
    juce::TextButton detectionToggleButton { "Tra" };
    bool transientDetectionEnabled = true;

    // Transient snap buttons — Start marker (left of Start knob)
    juce::TextButton prevTransientButton { "< T" };
    juce::TextButton nextTransientButton { "T >" };

    // Transient snap buttons — End marker (right of End knob)
    juce::TextButton prevEndTransientButton { "< T" };
    juce::TextButton nextEndTransientButton { "T >" };

    // Detected transient positions in seconds (populated on each sample load)
    std::vector<double> transientPositionsSeconds;
    double transientThreshold = 4.0;  // RMS multiplier for detection (1.5–10)

    // Sensitivity knob + labels
    juce::Slider sensKnob;
    juce::Label  sensLabel;
    juce::Label  transientCountLabel;

    // Freeze state — never persisted, always starts false
    bool isFreezeActive          = false;
    bool loopWasOnBeforeFreeze   = false;   // true if loop was ON when freeze was pressed
    juce::int64 lastFreezeTapMs  = 0;       // timestamp of last freeze tap for double-tap detection

    // Compact knob LookAndFeel shared by all three SamplerPad knobs
    CompactKnobLookAndFeel compactKnobLaf;

    // Volume knob
    juce::Slider volumeKnob;
    juce::Label volumeLabel;

    // Start point knob
    juce::Slider startKnob;
    juce::Label startKnobLabel;
    float startPointNormalized = 0.0f;

    // End point knob
    juce::Slider endKnob;
    juce::Label endKnobLabel;
    float endPointNormalized = 1.0f;
    
    // Pitch factor tracking for visual feedback
    double currentPitchFactor = 1.0;  // 1.0 = no pitch change
    double originalDuration = 0.0;     // Store original duration
    
    // MIDI Learn state
    bool isLearning = false;
    
    // Audio components
    juce::AudioFormatManager& formatManager;
    
    // Audio file info for resampling
    juce::File currentAudioFile;
    juce::int64 originalLengthInSamples = 0;
    double originalSampleRate = 0.0;
    
    // Listener list
    juce::ListenerList<Listener> listeners;
    int fixedViewportWidth = 700;  // Will be updated in resized()

    //==============================================================================
    // Transient detection enable/disable

    void updateTransientControlsState()
    {
        const bool on = transientDetectionEnabled;
        const auto grayBg   = juce::Colour(0xFF555555);
        const auto grayText = juce::Colour(0xFF555555);

        // Start snap buttons: orange-red when enabled; gray when disabled
        for (auto* btn : { &prevTransientButton, &nextTransientButton })
        {
            btn->setEnabled(on);
            btn->setColour(juce::TextButton::buttonColourId,
                           on ? juce::Colour(0xFFE84A1A) : grayBg);
            btn->setColour(juce::TextButton::textColourOffId,
                           on ? juce::Colour(0xFFFFFFFF) : grayText);
        }

        // End snap buttons: cyan-blue when enabled; gray when disabled
        for (auto* btn : { &prevEndTransientButton, &nextEndTransientButton })
        {
            btn->setEnabled(on);
            btn->setColour(juce::TextButton::buttonColourId,
                           on ? juce::Colour(0xFF00B4D8) : grayBg);
            btn->setColour(juce::TextButton::textColourOffId,
                           on ? juce::Colour(0xFFFFFFFF) : grayText);
        }

        // Sens knob: dark red fill when enabled; gray when disabled
        sensKnob.setEnabled(on);
        sensKnob.setColour(juce::Slider::rotarySliderFillColourId,
                           on ? juce::Colour(0xFF8B2500) : juce::Colour(0xFF555555));
        sensKnob.setColour(juce::Slider::thumbColourId,
                           on ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF777777));

        // Count label: white when enabled; gray when disabled
        transientCountLabel.setColour(juce::Label::textColourId,
                                      on ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFF555555));

        repaint();
    }

    //==============================================================================
    // Grid snap helpers

    // Pixel width actually covered by waveform data (mirrors WaveformComponent::getActualWaveformWidth).
    float computeActualWaveformWidth() const
    {
        if (waveformComponent == nullptr) return 0.0f;
        int rw = waveformComponent->getWidth();
        if (pitchOffset > 0)
        {
            double cf = juce::jmin(std::pow(2.0, pitchOffset / 12.0), 16.0);
            return (float)(rw / cf);
        }
        return (float)rw;
    }

    // Resolution label string for a given index (0-5).
    static juce::String gridResolutionLabel(int index)
    {
        static const char* labels[] = { "1ms", "10ms", "50ms", "100ms", "500ms", "1s" };
        return (index >= 0 && index < numGridResolutions) ? labels[index] : "?";
    }

    // Refresh the resolution button text and colors to match current state.
    void updateGridResolutionButton()
    {
        gridResolutionButton.setButtonText(gridResolutionLabel(gridResolutionIndex));
        updateGridResolutionButtonState();
    }

    // Enable/disable + color the resolution button based on gridSnapEnabled.
    void updateGridResolutionButtonState()
    {
        gridResolutionButton.setEnabled(gridSnapEnabled);
        gridResolutionButton.setColour(juce::TextButton::buttonColourId,
            gridSnapEnabled ? juce::Colour(0xFFAAFF00) : juce::Colour(0xFF3A3A3A));
        gridResolutionButton.setColour(juce::TextButton::textColourOffId,
            gridSnapEnabled ? juce::Colour(0xFF111111) : juce::Colour(0xFF7A7A7A));
    }

    // Choose the best default resolution for a given sample duration and apply it.
    // Called on every sample load; fires the listener so the choice is persisted.
    void autoSelectGridResolution(double durationSeconds)
    {
        // Only auto-select when the user has no saved preference.
        // Once a preference exists (saved to disk and restored via setGridResolutionIndex),
        // the auto-select is skipped so the user's choice is not overridden on each load.
        if (userHasSetGridResolution)
            return;

        int newIndex;
        if      (durationSeconds < 0.5)  newIndex = 1; // 10ms
        else if (durationSeconds < 2.0)  newIndex = 2; // 50ms
        else if (durationSeconds < 5.0)  newIndex = 3; // 100ms
        else if (durationSeconds < 15.0) newIndex = 4; // 500ms
        else                             newIndex = 5; // 1s

        gridResolutionIndex = newIndex;
        updateGridResolutionButton();
        if (waveformComponent != nullptr)
            waveformComponent->setGridResolution(gridSnapEnabled, getGridInterval());
        // Does NOT fire listener — auto-select never saves to disk
    }

    // Snap `seconds` to the nearest grid line if within 10 pixels (requirement 8).
    // Flashes the corresponding marker when a snap occurs (requirement 9).
    // isStart: true = start marker, false = end marker.
    double applyGridSnap(double seconds, bool isStart)
    {
        if (!gridSnapEnabled || originalDuration <= 0.0)
            return seconds;

        double interval = getGridInterval();
        float actualWidth = computeActualWaveformWidth();
        if (actualWidth <= 1.0f)
            return seconds;

        double pixelsPerSecond = actualWidth / originalDuration;
        double thresholdSeconds = 10.0 / pixelsPerSecond;

        // Find nearest grid line
        double nearest = std::round(seconds / interval) * interval;
        nearest = juce::jlimit(0.0, originalDuration, nearest);

        if (std::abs(seconds - nearest) <= thresholdSeconds)
        {
            if (std::abs(seconds - nearest) > 1e-9 && waveformComponent != nullptr)
            {
                if (isStart)
                    waveformComponent->flashStartMarker();
                else
                    waveformComponent->flashEndMarker();
            }
            return nearest;
        }
        return seconds;
    }

    //==============================================================================
    // Freeze helpers

    void applyFreezeButtonStyle(bool on)
    {
        if (on)
        {
            freezeButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF00CFFF)); // ice blue
            freezeButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF111111)); // dark text
        }
        else
        {
            freezeButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A)); // dark inactive
            freezeButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE)); // off-white text
        }
    }

    //==============================================================================
    // Auto pitch detection (YIN algorithm on background thread)

    // Kick off a background pitch analysis for the current file + marker region.
    void startTuneAnalysis()
    {
        if (isTuneRunning.load()) return;
        if (!currentAudioFile.existsAsFile()) return;

        // Capture analysis params on the message thread
        tuneFile          = currentAudioFile;
        tuneStartSec      = startPointNormalized * originalDuration;
        tuneEndSec        = endPointNormalized   * originalDuration;
        tuneRootMidiNote  = currentMidiNote;

        // Visual: "Analyzing..." state
        isTuneRunning.store(true);
        tuneButton.setEnabled(false);
        tuneButton.setButtonText("...");
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF2A4A2A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF00FF88));

        // Stop any previous thread
        if (tuneThread != nullptr)
        {
            tuneThread->stopThread(500);
            tuneThread = nullptr;
        }

        tuneThread = std::make_unique<TuneAnalyzerThread>(*this);
        tuneThread->startThread();
    }

    // Runs on the background thread (called by TuneAnalyzerThread::run).
    void runTuneAnalysis()
    {
        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(tuneFile));

        if (reader == nullptr || juce::Thread::currentThreadShouldExit())
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        const double sr = reader->sampleRate;
        const juce::int64 totalSamples = reader->lengthInSamples;

        if (sr <= 0.0 || totalSamples <= 0)
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        // Derive sample indices for the analysis region
        juce::int64 startSample = (juce::int64)(tuneStartSec * sr);
        juce::int64 endSample   = (tuneEndSec > 0.0 && tuneEndSec < totalSamples / sr)
                                  ? (juce::int64)(tuneEndSec * sr)
                                  : totalSamples;
        startSample = juce::jlimit((juce::int64)0, totalSamples, startSample);
        endSample   = juce::jlimit(startSample + 1, totalSamples, endSample);

        juce::int64 numSamples = endSample - startSample;

        // Limit to 0.5 s for speed; take from start of region
        const juce::int64 maxBuf = (juce::int64)(sr * 0.5);
        numSamples = juce::jmin(numSamples, maxBuf);

        if (numSamples < 512)
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        if (juce::Thread::currentThreadShouldExit())
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        // Read audio into a multi-channel buffer then mix to mono
        const int numCh = juce::jmin((int)reader->numChannels, 2);
        juce::AudioBuffer<float> buf(numCh, (int)numSamples);
        reader->read(&buf, 0, (int)numSamples, startSample, true, true);

        std::vector<float> mono((size_t)numSamples, 0.0f);
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* data = buf.getReadPointer(ch);
            for (int i = 0; i < (int)numSamples; ++i)
                mono[(size_t)i] += data[i] / (float)numCh;
        }

        if (juce::Thread::currentThreadShouldExit())
        {
            juce::MessageManager::callAsync([this] { onTuneAnalysisComplete(-1.0); });
            return;
        }

        const double freq = yinDetectPitch(mono.data(), (int)numSamples, sr);

        juce::MessageManager::callAsync([this, freq] { onTuneAnalysisComplete(freq); });
    }

    // Called back on the message thread with the detection result.
    void onTuneAnalysisComplete(double freqHz)
    {
        isTuneRunning.store(false);
        tuneButton.setEnabled(true);

        if (freqHz <= 0.0)
        {
            // No reliable pitch found
            detectedNoteName = "";
            detectedFreqHz   = 0.0;

            tuneButton.setButtonText("Tune");
            tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF4A4A4A));
            tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));

            bottomInfoLabel.setText("No pitch detected", juce::dontSendNotification);
            printf("[TUNE] No pitch detected\n");

            listeners.call([this](Listener& l) { l.detectedNoteChanged("", 0.0); });
            return;
        }

        // Map frequency to nearest MIDI note
        const double exactNote = 69.0 + 12.0 * std::log2(freqHz / 440.0);
        int nearestMidi = (int)std::round(exactNote);
        nearestMidi = juce::jlimit(0, 127, nearestMidi);

        // Build note name string
        static const char* noteNames[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        const int octave  = nearestMidi / 12 - 1;
        const int noteIdx = nearestMidi % 12;
        detectedNoteName = juce::String(noteNames[noteIdx]) + juce::String(octave);
        detectedFreqHz   = freqHz;

        // Store correction as the hidden base offset so "0 st" = this note.
        // User pitch resets to 0; MainComponent adds basePitchOffset when applying to the engine.
        const int correction = juce::jlimit(-48, 48, tuneRootMidiNote - nearestMidi);
        basePitchOffset = correction;
        pitchOffset     = 0;
        updatePitchDisplay(0);  // display shows "0 st" = in tune with detected note
        listeners.call([this](Listener& l) { l.pitchOffsetChanged(0); }); // MainComponent adds base

        // Now update bottomInfoLabel with detection result (overrides what updatePitchDisplay left)
        const int freqInt = (int)std::round(freqHz);
        bottomInfoLabel.setText(
            detectedNoteName + " \xc2\xb7 " + juce::String(freqInt) + " Hz",
            juce::dontSendNotification);

        // Button shows detected note in bright green
        tuneButton.setButtonText(detectedNoteName);
        tuneButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF1A5A1A));
        tuneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF00FF88));

        printf("[TUNE] Detected: %s = %.1f Hz  (MIDI %d) → correction %+d st\n",
               detectedNoteName.toRawUTF8(), freqHz, nearestMidi, correction);

        // Notify listeners for persistence
        listeners.call([this](Listener& l) { l.detectedNoteChanged(detectedNoteName, detectedFreqHz); });
    }

    // YIN pitch detection algorithm.
    // Returns fundamental frequency in Hz, or -1.0 if not detected reliably.
    // Range: minFreqHz (C1=27Hz) to maxFreqHz (C8=4186Hz).
    static double yinDetectPitch(const float* mono, int numSamples, double sampleRate,
                                 double minFreqHz = 27.0, double maxFreqHz = 4186.0)
    {
        const int minPeriod = juce::jmax(2, (int)(sampleRate / maxFreqHz));
        const int maxPeriod = juce::jmin(numSamples / 2 - 1, (int)(sampleRate / minFreqHz));

        if (maxPeriod <= minPeriod) return -1.0;

        const int bufLen = numSamples - maxPeriod;  // usable window
        if (bufLen <= 0) return -1.0;

        // Step 1+2: Difference function d[tau] and Cumulative Mean Normalized Difference d'[tau]
        std::vector<double> cmnd((size_t)(maxPeriod + 1), 0.0);
        cmnd[0] = 1.0;
        double runningSum = 0.0;

        for (int tau = 1; tau <= maxPeriod; ++tau)
        {
            double sumSq = 0.0;
            for (int t = 0; t < bufLen; ++t)
            {
                const double diff = (double)mono[t] - (double)mono[t + tau];
                sumSq += diff * diff;
            }
            runningSum += sumSq;
            cmnd[(size_t)tau] = (runningSum > 0.0) ? sumSq * tau / runningSum : 0.0;
        }

        // Step 3: Absolute threshold — find first local minimum below 0.10
        const double absThreshold    = 0.10;
        const double fallbackThresh  = 0.30;
        int bestTau = -1;

        for (int tau = minPeriod; tau <= maxPeriod - 1; ++tau)
        {
            if (cmnd[(size_t)tau] < absThreshold && cmnd[(size_t)tau] <= cmnd[(size_t)(tau + 1)])
            {
                bestTau = tau;
                break;
            }
        }

        // If no absolute threshold minimum: use global minimum (with looser threshold)
        if (bestTau < 0)
        {
            double minVal = 1e9;
            for (int tau = minPeriod; tau <= maxPeriod; ++tau)
            {
                if (cmnd[(size_t)tau] < minVal)
                {
                    minVal = cmnd[(size_t)tau];
                    bestTau = tau;
                }
            }
            if (minVal > fallbackThresh)
                return -1.0;   // no reliable pitch
        }

        // Step 4: Parabolic interpolation for sub-sample accuracy
        double refinedTau = (double)bestTau;
        if (bestTau > minPeriod && bestTau < maxPeriod)
        {
            const double s0 = cmnd[(size_t)(bestTau - 1)];
            const double s1 = cmnd[(size_t) bestTau];
            const double s2 = cmnd[(size_t)(bestTau + 1)];
            const double denom = 2.0 * (2.0 * s1 - s0 - s2);
            if (std::abs(denom) > 1e-9)
                refinedTau = bestTau + (s2 - s0) / denom;
        }

        if (refinedTau <= 0.0) return -1.0;
        return sampleRate / refinedTau;
    }

    void toggleFreeze()
    {
        if (!isFreezeActive)
        {
            // Turning freeze ON
            loopWasOnBeforeFreeze = loopButton.getToggleState();
            isFreezeActive = true;
            applyFreezeButtonStyle(true);

            if (!loopWasOnBeforeFreeze)
            {
                // Loop was off — turn it on silently then notify
                setLoopEnabled(true);
                listeners.call([](Listener& l) { l.loopEnabledChanged(true); });
            }
            listeners.call([](Listener& l) { l.freezeChanged(true); });
        }
        else
        {
            // Turning freeze OFF
            isFreezeActive = false;
            applyFreezeButtonStyle(false);

            if (!loopWasOnBeforeFreeze)
            {
                // Loop was off before freeze — restore that state
                setLoopEnabled(false);
                listeners.call([](Listener& l) { l.loopEnabledChanged(false); });
            }
            // else: loop was already on — leave it on

            listeners.call([](Listener& l) { l.freezeChanged(false); });
        }
    }
};