#include "MainComponent.h"
#include "UIComponents.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <vfw.h>  // Add this for GetOpenFileNamePreviewA
#endif


//==============================================================================
MainComponent::MainComponent()
    : sampleListBox("samples", nullptr),
      cpuTimer(*this),
      oneShotTailTimer(*this)
{
    printf("DEBUG: MainComponent constructor started\n");
    fflush(stdout);
    
    // Create configuration manager for session persistence
    configManager = std::make_unique<ConfigurationManager>();
    printf("Settings file: %s\n", configManager->getSettingsFilePath().toRawUTF8());
    
    // Create the model
    sampleListModel = std::make_unique<SampleListModel>(*this);
    sampleListBox.setModel(sampleListModel.get());
    
    setSize(900, 700);
    
    formatManager.registerBasicFormats();
    
    // Add looping sampler voices with note stealing enabled
    for (int i = 0; i < 16; ++i)
        sampler.addVoice(new LoopingSamplerVoice());
    
    // Enable note stealing for better voice management
    sampler.setNoteStealingEnabled(true);
    
    // Configure buttons
    menuButton.setButtonText("Menu");
    menuButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    menuButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
    addAndMakeVisible(menuButton);
    menuButton.addListener(this);

    // Reset / Panic button — dark red, signals STOP/DANGER
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF8B0000));
    resetButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFFFFFFF));
    addAndMakeVisible(resetButton);
    resetButton.addListener(this);

    setWantsKeyboardFocus(true);

    // Base tuning frequency label — draggable, default 440.0 Hz
    baseTuningLabel.setText("440.0 Hz", juce::dontSendNotification);
    baseTuningLabel.setJustificationType(juce::Justification::centred);
    baseTuningLabel.setColour(juce::Label::textColourId,       juce::Colour(0xFFCECECE));
    baseTuningLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF363636));
    baseTuningLabel.setFont(juce::Font(11.0f));
    baseTuningLabel.setTooltip("Base tuning: A4 reference frequency. Drag up/down (Shift = coarse 1 Hz steps)");
    baseTuningLabel.onHzChanged = [this](double newHz)
    {
        // Propagate to SampleCard (Tune button uses this)
        sampleCard.setBaseTuningHz(newHz);
        // Propagate to all live sounds
        float ratio = (float)(newHz / 440.0);
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                snd->baseTuningRatioAtomic.store(ratio);
        // Save
        if (configManager != nullptr)
            configManager->saveBaseTuningHz(newHz);
        printf("[TUNING] Base tuning changed to %.1f Hz\n", newHz);
    };
    addAndMakeVisible(baseTuningLabel);

      testToneButton.setButtonText("Test tone");
      testToneButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
      testToneButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
      addAndMakeVisible(testToneButton);
      testToneButton.addListener(this);

    // Master volume knob — use compact LookAndFeel so knob circle fills its bounding box
    masterVolumeKnob.setLookAndFeel(&compactKnobLaf);
    masterVolumeKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    masterVolumeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    masterVolumeKnob.setRange(0.0, 1.0, 0.01);
    masterVolumeKnob.setValue(0.7, juce::dontSendNotification);
    masterVolumeKnob.setTooltip("Master Volume");
    masterVolumeKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFFCECECE));
    masterVolumeKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xFF0A0A0A));
    masterVolumeKnob.setColour(juce::Slider::thumbColourId, juce::Colour(0xFF1E1E1E)); // dark indicator on light fill
    masterVolumeKnob.setDoubleClickReturnValue(true, 1.0); // double-click resets to 100%
    masterVolumeKnob.onValueChange = [this] {
        float v = (float)masterVolumeKnob.getValue();
        masterVolumeGain.store(v);
        masterVolValueLabel.setText(juce::String(juce::roundToInt(v * 100)) + "%",
                                    juce::dontSendNotification);
        if (configManager != nullptr)
            configManager->saveMasterVolume(v);
    };
    masterVolumeKnob.addMouseListener(this, false); // MainComponent::mouseDoubleClick flashes label
    addAndMakeVisible(masterVolumeKnob);

    masterVolumeLabel.setText("Master Vol", juce::dontSendNotification);
    masterVolumeLabel.setJustificationType(juce::Justification::centredRight);
    masterVolumeLabel.setFont(juce::Font(12.0f));
    masterVolumeLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
    addAndMakeVisible(masterVolumeLabel);

    masterVolValueLabel.setText("70%", juce::dontSendNotification);
    masterVolValueLabel.setJustificationType(juce::Justification::centredLeft);
    masterVolValueLabel.setFont(juce::Font(10.0f));
    masterVolValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
    addAndMakeVisible(masterVolValueLabel);

      // Add MIDI activity light to the title area
      addAndMakeVisible(midiActivityLight);
      midiActivityLight.setAlwaysOnTop(true); // Ensure it's visible

      // Add the sample card
      addAndMakeVisible(sampleCard);

      // Add listeners for the card buttons
      sampleCard.getAddButton().addListener(this);
      sampleCard.getPrevButton().addListener(this);
      sampleCard.getNextButton().addListener(this);

      // Add listener for MIDI note changes
      sampleCard.addListener(this);

      // Save zoom level and scroll position whenever they change interactively
      sampleCard.onZoomStateChanged = [this](double zoomLevel, float normalizedScroll)
      {
          if (configManager != nullptr)
          {
              configManager->saveZoomLevel((float)zoomLevel);
              configManager->saveZoomScrollPosition(normalizedScroll);
          }
      };

      // Wire playhead position and direction — read 60fps from SampleCard's PlayheadTimer
      sampleCard.getPlayheadPosition = [this] { return getPlayheadPositionNormalized(); };
      sampleCard.getPlayheadDirection = [this] {
          for (int i = 0; i < sampler.getNumVoices(); ++i)
              if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
                  if (v->playheadPositionAtomic.load() >= 0)
                      return v->playDirectionAtomic.load();
          return 1;
      };
      sampleCard.onTrimRequested = [this] { performTrimAsync(); };

      // Set initial sample name
      sampleCard.setSampleName("No sample loaded");

      addAndMakeVisible(audioDeviceInfoLabel);
      audioDeviceInfoLabel.setJustificationType(juce::Justification::left);
      audioDeviceInfoLabel.setFont(juce::Font(12.0f));
      audioDeviceInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF7A7A7A));

      addAndMakeVisible(midiDeviceInfoLabel);
      midiDeviceInfoLabel.setJustificationType(juce::Justification::left);
      midiDeviceInfoLabel.setFont(juce::Font(12.0f));
      midiDeviceInfoLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF7A7A7A));

      addAndMakeVisible(cpuUsageLabel);
      cpuUsageLabel.setJustificationType(juce::Justification::right);
      cpuUsageLabel.setFont(juce::Font(12.0f, juce::Font::bold));
      cpuUsageLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9DC95C));
    
    // Configure sliders
    lowNoteSlider.setRange(0, 127, 1);
    highNoteSlider.setRange(0, 127, 1);
    rootNoteSlider.setRange(0, 127, 1);
    
    lowNoteSlider.addListener(this);
    highNoteSlider.addListener(this);
    rootNoteSlider.addListener(this);
    
    // Set up audio
    setAudioChannels(0, 2);
    
    // IMPORTANT: Load audio settings BEFORE applying the default buffer size
    // This ensures saved settings override the default
    loadAudioSettings();
    
    // Note: Don't set a default buffer size here - it will override saved settings
    // The loadAudioSettings() method above will handle restoring the saved buffer
    
    // Start CPU timer for updates every 500ms
    cpuTimer.startTimer(500);
    
    // Add change listener for audio device changes
    deviceManager.addChangeListener(this);
    
    // Load last session (sample, MIDI settings, directory)
    loadLastSession();
    
    // Start heartbeat — prints every 100ms on its own thread so we can see
    // exactly which intervals the message thread is blocked during sample loads.
    heartbeatThread.startThread(juce::Thread::Priority::low);

    printf("DEBUG: MainComponent constructor completed\n");
    fflush(stdout);
}

MainComponent::~MainComponent()
{
    heartbeatThread.stopThread(500);
    masterVolumeKnob.setLookAndFeel(nullptr);
    cpuTimer.stopTimer();
    navSaveTimer.stopTimer();
    transientDetectionTimer.stopTimer();
    navDebounceTimer.stopTimer();
    pitchSaveTimer.stopTimer();
    eqSaveTimer.stopTimer();
    markerSaveTimer.stopTimer();
    volSaveTimer.stopTimer();
    adsrSaveTimer.stopTimer();
    normSaveTimer.stopTimer();
    filterModeSaveTimer.stopTimer();
    loopSaveTimer.stopTimer();
    deviceManager.removeChangeListener(this);

    // OPT 1: Stop FFT worker thread before audio shutdown to prevent use-after-free
    // on fft/fftCircularBuffer when the audio device is torn down.
    if (fftThread != nullptr)
    {
        fftThread->stopThread(200);
        fftThread.reset();
    }

    // Kill any active freeze/loop voices before audio shutdown
    muteOutput.store(true);
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
        {
            s->freezeActive.store(false);
            s->loopEnabled.store(false);
        }
    for (int i = 0; i < sampler.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
            v->forceStop();
    sampler.clearSounds();
    sampler.allNotesOff(0, false);

    // Shutdown save — flush current state to disk before the app closes.
    // Also covers the case where navSaveTimer was still pending (user closed the app
    // quickly after navigation before the 500ms timer fired).
    if (configManager != nullptr)
    {
        configManager->savePitchOffset(sampleCard.getPitchOffset());
        configManager->saveMasterVolume(masterVolumeGain.load());
        saveCurrentSampleState();
        configManager->saveNow();
        printf("[PITCH] Shutdown save: pitch=%+d\n", sampleCard.getPitchOffset());
        fflush(stdout);
    }

    shutdownAudio();
}

//==============================================================================
void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    sampler.setCurrentPlaybackSampleRate(sampleRate);
    midiCollector.reset(sampleRate);

    // ── FFT + EQ initialization ───────────────────────────────────────────────────
    // OPT 1: Stop FFT worker thread before recreating the FFT object to prevent
    // use-after-free if prepareToPlay is called while the thread is running.
    if (fftThread != nullptr)
    {
        fftThread->stopThread(200);
        fftThread.reset();
    }

    fft = std::make_unique<juce::dsp::FFT>(kFFTOrder);

    // Precompute Hann window — reduces spectral leakage.
    for (int i = 0; i < kFFTSize; ++i)
        fftWindow[i] = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::twoPi * i / (kFFTSize - 1)));

    // Reset filter state and circular buffer (fresh start).
    resetEqState();
    fftAbstractFifo.reset();
    std::fill(std::begin(fftCircularBuffer), std::end(fftCircularBuffer), 0.0f);

    // Inform EQDisplay of the current sample rate so biquad response rendering is correct.
    sampleCard.setEqSampleRate(sampleRate);

    // OPT 4: Wire spectrum callback — returns true only when new FFT data is ready.
    sampleCard.setEqSpectrumCallback([this](float* dest, int numBins) -> bool {
        return getSpectrumSnapshot(dest, numBins);
    });

    // OPT 1: Start FFT worker thread (wakes every 33ms — 30fps).
    fftThread = std::make_unique<FftWorkerThread>(*this);
    fftThread->startThread();

    printf("[EQ] prepareToPlay: sampleRate=%.1f  fftSize=%d  FFT worker thread started\n", sampleRate, kFFTSize);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // muteOutput is set true by the message thread during sample-change to guarantee a
    // silent, zeroed buffer while the sampler is being rebuilt.  Checked atomically so
    // the audio thread sees it within one block (~6 ms) with no locks required.
    if (muteOutput.load())
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    bufferToFill.clearActiveBufferRegion();
    
    juce::MidiBuffer midiMessages;
    juce::MidiBuffer incomingMidi;
    midiCollector.removeNextBlockOfMessages(incomingMidi, bufferToFill.numSamples);
    midiMessages.addEvents(incomingMidi, 0, bufferToFill.numSamples, 0);
    
    sampler.renderNextBlock(*bufferToFill.buffer, midiMessages, 0, bufferToFill.numSamples);

    // Apply normalization gain (non-destructive, before per-pad volume)
    {
        float ng = normGain.load();
        if (ng != 1.0f)
            bufferToFill.buffer->applyGain(ng);
    }

    // Apply volume gain
    float gain = volumeGain.load();
    if (gain != 1.0f)
        bufferToFill.buffer->applyGain(gain);

    // ── Parametric EQ (3 biquad bands) — OPT 2+3: single-pass cascade, double-buffer coeffs ──
    if (eqActive.load())
    {
        const int numSamples  = bufferToFill.numSamples;
        const int numChannels = bufferToFill.buffer->getNumChannels();

        // OPT 2: swap in freshly computed coefficients if UI thread wrote them (nanoseconds).
        const auto* c = eqCoeffDB.swapIfUpdated();

        // OPT 3: single pass — apply all 3 bands per sample (better cache utilization vs.
        // three separate passes over the buffer).
        for (int ch = 0; ch < juce::jmin(numChannels, 2); ++ch)
        {
            float* data = bufferToFill.buffer->getWritePointer(ch);
            // Keep all six state variables in registers for the inner loop.
            double z1_0 = eqZ1[0][ch], z2_0 = eqZ2[0][ch];
            double z1_1 = eqZ1[1][ch], z2_1 = eqZ2[1][ch];
            double z1_2 = eqZ1[2][ch], z2_2 = eqZ2[2][ch];

            for (int i = 0; i < numSamples; ++i)
            {
                // Direct Form II Transposed cascade — all 3 bands per sample
                double x  = static_cast<double>(data[i]);
                // Band 0
                double y0 = c[0].b0 * x  + z1_0;
                z1_0 = c[0].b1 * x  - c[0].a1 * y0 + z2_0;
                z2_0 = c[0].b2 * x  - c[0].a2 * y0;
                // Band 1
                double y1 = c[1].b0 * y0 + z1_1;
                z1_1 = c[1].b1 * y0 - c[1].a1 * y1 + z2_1;
                z2_1 = c[1].b2 * y0 - c[1].a2 * y1;
                // Band 2
                double y2 = c[2].b0 * y1 + z1_2;
                z1_2 = c[2].b1 * y1 - c[2].a1 * y2 + z2_2;
                z2_2 = c[2].b2 * y1 - c[2].a2 * y2;
                data[i] = static_cast<float>(y2);
            }

            eqZ1[0][ch] = z1_0; eqZ2[0][ch] = z2_0;
            eqZ1[1][ch] = z1_1; eqZ2[1][ch] = z2_1;
            eqZ1[2][ch] = z1_2; eqZ2[2][ch] = z2_2;
        }
    }

    // ── FFT spectrum analyzer (post-EQ) — OPT 1: push to circular buffer only ────
    // The FFT worker thread reads from fftCircularBuffer every 33ms and does all the
    // heavy processing there.  Audio thread cost here is < 1 microsecond.
    {
        const int numSamples  = bufferToFill.numSamples;
        const int numChannels = bufferToFill.buffer->getNumChannels();
        const int toWrite     = juce::jmin(numSamples, fftAbstractFifo.getFreeSpace());

        if (toWrite > 0)
        {
            int start1, size1, start2, size2;
            fftAbstractFifo.prepareToWrite(toWrite, start1, size1, start2, size2);

            for (int i = 0; i < size1; ++i)
            {
                float mono = 0.0f;
                for (int ch = 0; ch < juce::jmin(numChannels, 2); ++ch)
                    mono += bufferToFill.buffer->getReadPointer(ch)[i];
                if (numChannels > 0) mono /= static_cast<float>(numChannels);
                fftCircularBuffer[start1 + i] = mono;
            }
            for (int i = 0; i < size2; ++i)
            {
                float mono = 0.0f;
                for (int ch = 0; ch < juce::jmin(numChannels, 2); ++ch)
                    mono += bufferToFill.buffer->getReadPointer(ch)[size1 + i];
                if (numChannels > 0) mono /= static_cast<float>(numChannels);
                fftCircularBuffer[start2 + i] = mono;
            }

            fftAbstractFifo.finishedWrite(size1 + size2);
        }
    }

    if (sineWaveActive)
    {
        const double sampleRate = sampler.getSampleRate();
        if (sampleRate > 0)
        {
            const double phaseIncrement = sineWaveFrequency * juce::MathConstants<double>::twoPi / sampleRate;
            
            for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
            {
                float* channelData = bufferToFill.buffer->getWritePointer(channel);
                
                for (int i = 0; i < bufferToFill.numSamples; ++i)
                {
                    channelData[i] += (float)(std::sin(sineWavePhase + i * phaseIncrement) * sineWaveAmplitude);
                }
            }
            
            sineWavePhase += phaseIncrement * bufferToFill.numSamples;
            while (sineWavePhase >= juce::MathConstants<double>::twoPi)
                sineWavePhase -= juce::MathConstants<double>::twoPi;
        }
    }

    // Apply master volume last so it scales the entire output
    float masterGain = masterVolumeGain.load();
    if (masterGain != 1.0f)
        bufferToFill.buffer->applyGain(masterGain);
}

void MainComponent::releaseResources()
{
}

//==============================================================================
void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF1E1E1E));
    
    // Draw title centred in the 40px top bar, between Menu button (left) and right controls
    // Left trim: 20px margin + 60px menu = 80; Right trim: 20px margin + 250px right group = 270
    g.setColour(juce::Colour(0xFFCECECE));
    g.setFont(juce::Font(18.0f, juce::Font::bold));
    auto titleArea = getLocalBounds().withTrimmedLeft(80).withTrimmedRight(348).removeFromTop(40);
    g.drawText("JAIVA-SAMPLER||1.0", titleArea, juce::Justification::centred, true);

    // Line above footer
    g.setColour(juce::Colour(0xFF404040));
    auto footerY = getHeight() - 70;
    g.drawHorizontalLine(footerY, 20, getWidth() - 20);

    // Draw dark outline around top-level buttons
    g.setColour(juce::Colour(0xFF0A0A0A));
    g.drawRect(menuButton.getBounds(), 1);
    g.drawRect(resetButton.getBounds(), 1);
    g.drawRect(baseTuningLabel.getBounds(), 1);
    g.drawRect(testToneButton.getBounds(), 1);
    g.drawRect(masterVolumeKnob.getBounds(), 1);
}

void MainComponent::resized()
{
    // Add a flag to prevent recursive resizing
    static bool isResizing = false;
    if (isResizing) return;
    isResizing = true;
    
    // No top margin — content starts flush at the top edge
    auto area = getLocalBounds().withTrimmedLeft(20).withTrimmedRight(20).withTrimmedBottom(20);

    // Top bar: strict 40px — all elements on one horizontal line, nothing expands this
    auto topBar = area.removeFromTop(40);

    // Menu button — vertically centred in 40px bar
    menuButton.setBounds(topBar.removeFromLeft(60).withSizeKeepingCentre(56, 30));

    // Right: Reset(52)+gap(6)+HzLabel(70)+gap(6)+MasterVolLabel(62)+gap(4)+knob(28)+gap(4)+value%(32)+gap(4)+MIDI(22)+gap(6)+TestTone(62) = 358px
    auto rightSide = topBar.removeFromRight(52 + 6 + 70 + 6 + 62 + 4 + 28 + 4 + 32 + 4 + 22 + 6 + 62);
    resetButton.setBounds(rightSide.removeFromLeft(52).withSizeKeepingCentre(48, 30));
    rightSide.removeFromLeft(6);
    baseTuningLabel.setBounds(rightSide.removeFromLeft(70).withSizeKeepingCentre(68, 24));
    rightSide.removeFromLeft(6);

    // Master Vol: label on left, 28x28 knob, then percentage value label
    masterVolumeLabel.setBounds(rightSide.removeFromLeft(62).withSizeKeepingCentre(62, 16));
    rightSide.removeFromLeft(4);
    masterVolumeKnob.setBounds(rightSide.removeFromLeft(28).withSizeKeepingCentre(28, 28));
    rightSide.removeFromLeft(4);
    masterVolValueLabel.setBounds(rightSide.removeFromLeft(32).withSizeKeepingCentre(30, 16));
    rightSide.removeFromLeft(4);

    // MIDI light — vertically centred
    midiActivityLight.setBounds(rightSide.removeFromLeft(22).withSizeKeepingCentre(20, 20));
    rightSide.removeFromLeft(6);

    // Test tone button — vertically centred
    testToneButton.setBounds(rightSide.removeFromLeft(62).withSizeKeepingCentre(58, 30));
    
    // Body area - this is where the card goes
    auto bodyArea = area.reduced(10, 5);
    
    // Make the card take most of the body width, but with max width to maintain proportions
    const int cardMaxWidth = 720;  // Maximum width to keep card from getting too wide
    const int cardHeight = 410;     // 10+24+5+165+5+24+4+139(tabContent)+24+10 — 139px for EQ display
    
    int cardWidth = (bodyArea.getWidth() - 40 < cardMaxWidth) ? (bodyArea.getWidth() - 40) : cardMaxWidth;
    
    // Ensure card width is positive
    if (cardWidth < 100) cardWidth = 100;
    
    auto cardBounds = bodyArea.withWidth(cardWidth)
                              .withHeight(cardHeight)
                              .withCentre(bodyArea.getCentre());
    
    // Ensure card bounds are valid before setting
    if (cardBounds.getWidth() > 0 && cardBounds.getHeight() > 0)
    {
        sampleCard.setBounds(cardBounds);
    }
    
    // Footer area at bottom - reduced by 40% (from 80px to 48px, using 50px for clean math)
    auto footerArea = getLocalBounds().reduced(20).removeFromBottom(50);
    
    // Left side: Device info - two rows with vertical centering
    // Use all available space before CPU indicator
    auto leftFooter = footerArea.withTrimmedRight(130); // Reserve space for CPU label + padding
    
    // Calculate row height for two rows with vertical centering
    const int rowHeight = 20; // Each row gets 20px
    const int totalRowsHeight = rowHeight * 2;
    const int verticalPadding = (footerArea.getHeight() - totalRowsHeight) / 2;
    
    // Audio info row (top)
    auto audioRow = leftFooter.removeFromTop(rowHeight).translated(0, verticalPadding);
    audioRow.removeFromLeft(5); // Left margin
    audioDeviceInfoLabel.setBounds(audioRow);
    audioDeviceInfoLabel.setFont(juce::Font(10.0f));
    audioDeviceInfoLabel.setJustificationType(juce::Justification::left);
    
    // MIDI info row (bottom)
    auto midiRow = leftFooter.removeFromTop(rowHeight).translated(0, verticalPadding);
    midiRow.removeFromLeft(5); // Left margin
    midiDeviceInfoLabel.setBounds(midiRow);
    midiDeviceInfoLabel.setFont(juce::Font(10.0f));
    midiDeviceInfoLabel.setJustificationType(juce::Justification::left);
    
    // Right side: CPU usage - maintain current positioning
    cpuUsageLabel.setBounds(footerArea.removeFromRight(120).reduced(5));
    cpuUsageLabel.setFont(juce::Font(11.0f, juce::Font::bold));
    cpuUsageLabel.setJustificationType(juce::Justification::right);
    
    isResizing = false;
}

//==============================================================================
void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &menuButton)
    {
        showSettingsMenu();
    }
    else if (button == &resetButton)
    {
        performPanicReset();
    }
    else if (button == &testToneButton)
    {
        toggleSineWave();
    }
    else if (button == &sampleCard.getAddButton())
    {
        auto* previewComp = new ::AudioPreviewComponent(formatManager);
        previewComp->setMasterVolumeRef(masterVolumeGain);
        activePreviewComp = previewComp;
        previewComp->onDestroy = [this] { activePreviewComp = nullptr; };
        
        // Use last saved directory or default
        juce::File startingDirectory = configManager->getLastDirectory();
        
        fileChooser = std::make_unique<juce::FileChooser>(
            "Select sample",
            startingDirectory,
            "*.wav;*.aiff;*.mp3"
        );
        
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode,
            [this](const juce::FileChooser& fc)
            {
                auto results = fc.getResults();
                if (results.size() > 0)
                {
                    auto file = results[0];
                    
                    // Save the directory for next time
                    configManager->saveLastDirectory(file.getParentDirectory());
                    
                    // Set current folder and scan for audio files
                    currentFolder = file.getParentDirectory();
                    scanCurrentFolderForAudioFiles();
                    
                    // Find and set current file index
                    for (int i = 0; i < folderAudioFiles.size(); ++i)
                    {
                        if (folderAudioFiles[i] == file)
                        {
                            currentFileIndex = i;
                            break;
                        }
                    }
                    
                    // Step 1 — save outgoing state before loading new file
                    saveOutgoingSampleState();

                    // + button = intentional new load: pitch always resets to 0.
                    printf("[PITCH] + button load: resetting pitch to 0\n");
                    sampleCard.setPitchOffset(0);
                    {
                        juce::ScopedLock lock(sampleLock);
                        if (!samples.isEmpty())
                            samples[0]->pitchOffset = 0;
                    }
                    for (int i = 0; i < sampler.getNumSounds(); ++i)
                        if (auto* snd = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                            snd->pitchOffsetAtomic.store(0);
                    if (configManager != nullptr)
                        configManager->savePitchOffset(0);

                    // Reset zoom immediately so user sees full waveform as soon as file loads
                    sampleCard.restoreZoomAndScroll(1.0, 0.0f);
                    if (configManager != nullptr) { configManager->saveZoomLevel(1.0f); configManager->saveZoomScrollPosition(0.0f); }

                    loadSampleFileAsync(file, true, /*resetZoom=*/true);
                }
            },
            previewComp
        );
    }
    else if (button == &sampleCard.getPrevButton())
    {
        sampleCard.restoreZoomAndScroll(1.0, 0.0f);
        if (configManager != nullptr) { configManager->saveZoomLevel(1.0f); configManager->saveZoomScrollPosition(0.0f); }
        loadPrevSample();
    }
    else if (button == &sampleCard.getNextButton())
    {
        sampleCard.restoreZoomAndScroll(1.0, 0.0f);
        if (configManager != nullptr) { configManager->saveZoomLevel(1.0f); configManager->saveZoomScrollPosition(0.0f); }
        loadNextSample();
    }
}

void MainComponent::toggleSineWave()
{
    sineWaveActive = !sineWaveActive;
    
    if (sineWaveActive)
    {
        sineWavePhase = 0.0;
        testToneButton.setButtonText("Stop tone");
        testToneButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFE25A00));
    }
    else
    {
        testToneButton.setButtonText("Test tone");
        testToneButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    }
    
    // TEMPORARY TEST: Trigger the light manually
    midiActivityLight.triggerActivity();
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        updateDeviceInfo();
        
        // Save audio settings whenever they change
        saveAudioSettings();
        
        printf("Audio device configuration changed - settings saved\n");
    }
}

//==============================================================================
void MainComponent::showAudioDeviceSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(
        deviceManager,
        0, 256, 0, 2, true, true, false, false
    );
    
    selector->setSize(500, 400);
    
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Audio Device Settings";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    options.launchAsync();
    updateDeviceInfo();
}

void MainComponent::updateDeviceInfo()
{
    // Update CPU usage only if changed significantly
    double newCPU = deviceManager.getCpuUsage() * 100.0;
    if (std::abs(newCPU - lastCPU) > 0.01)  // Only update if changed significantly
    {
        cpuUsageLabel.setText(juce::String::formatted("CPU: %.2f%%", newCPU), 
                              juce::dontSendNotification);
        lastCPU = newCPU;
    }
    
    // Only update device info occasionally or when changed
    cpuUpdateCounter++;
    if (cpuUpdateCounter % 10 == 0)  // Every 5 seconds (500ms * 10)
    {
        juce::String info;
        
        if (auto* currentDevice = deviceManager.getCurrentAudioDevice())
        {
            info += "Device: " + currentDevice->getName() + "\n";
            info += "Sample Rate: " + juce::String(currentDevice->getCurrentSampleRate()) + " Hz\n";
            info += "Buffer Size: " + juce::String(currentDevice->getCurrentBufferSizeSamples()) + " samples\n";
            
            auto activeOutputs = currentDevice->getActiveOutputChannels();
            info += "Outputs: " + juce::String(activeOutputs.countNumberOfSetBits()) + " channels\n";
        }
        else
        {
            info = "No audio device selected";
        }
        
        audioDeviceInfoLabel.setText(info, juce::dontSendNotification);
        
        juce::String midiInfo = "MIDI: ";
        if (midiInput != nullptr && !currentMidiDeviceName.isEmpty())
        {
            midiInfo += currentMidiDeviceName + " (Connected)";
        }
        else
        {
            midiInfo += "No device selected";
        }
        
        midiDeviceInfoLabel.setText(midiInfo, juce::dontSendNotification);
        
        // Reset counter to avoid overflow
        if (cpuUpdateCounter >= 1000) cpuUpdateCounter = 0;
    }
}

//==============================================================================
void MainComponent::saveAudioSettings()
{
    if (configManager == nullptr)
        return;
    
    auto* currentDevice = deviceManager.getCurrentAudioDevice();
    if (currentDevice != nullptr)
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        
        // Save audio settings: buffer size, sample rate, and device name
        // Device type is not needed as we can restore by device name
        configManager->saveAudioSettings(
            setup.bufferSize,
            setup.sampleRate,
            juce::String(), // Empty device type - not needed
            currentDevice->getName()
        );
        
        printf("Saved audio settings: buffer=%d, rate=%.1f, device=%s\n", 
               setup.bufferSize, setup.sampleRate, currentDevice->getName().toRawUTF8());
    }
}

void MainComponent::loadAudioSettings()
{
    if (configManager == nullptr)
        return;
    
    int savedBufferSize = configManager->getAudioBufferSize();
    double savedSampleRate = configManager->getAudioSampleRate();
    juce::String savedDeviceType = configManager->getAudioDeviceType();
    juce::String savedOutputDevice = configManager->getAudioOutputDevice();
    
    printf("Loading audio settings: buffer=%d, rate=%.1f, deviceType=%s, device=%s\n", 
           savedBufferSize, savedSampleRate, 
           savedDeviceType.toRawUTF8(), savedOutputDevice.toRawUTF8());
    
    // Note: We don't try to restore device type as there's no direct API for it
    // The audio device will be whatever the system default or user selects
    
    // Get current setup and modify it
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    
    bool setupChanged = false;
    
    // Apply saved buffer size if different
    if (savedBufferSize > 0 && setup.bufferSize != savedBufferSize)
    {
        setup.bufferSize = savedBufferSize;
        setupChanged = true;
        printf("Restoring buffer size to: %d\n", savedBufferSize);
    }
    
    // Apply saved sample rate if different
    if (savedSampleRate > 0 && setup.sampleRate != savedSampleRate)
    {
        setup.sampleRate = savedSampleRate;
        setupChanged = true;
        printf("Restoring sample rate to: %.1f\n", savedSampleRate);
    }
    
    // Apply the setup if changed
    if (setupChanged)
    {
        juce::String error = deviceManager.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            printf("Error restoring audio setup: %s\n", error.toRawUTF8());
        }
    }
}

//==============================================================================
void MainComponent::loadSampleFile(const juce::File& file)
{
    // For single-sample mode, clear existing samples before loading new one
    // This ensures only one sample is active at a time (consistent with Prev/Next navigation)
    {
        juce::ScopedLock lock(sampleLock);
        samples.clear();
        selectedSampleIndex = 0;
    }
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    
    if (reader != nullptr)
    {
        auto* sample = new MappedSample();
        sample->file = file;
        sample->name = file.getFileName();
        
        // IMPORTANT: Use the current MIDI note from the card as the root note
        sample->rootNote = sampleCard.getMidiNote();  // Use card's current note
        
        // For single-note mode, all notes are the same
        sample->lowNote = sample->rootNote;
        sample->highNote = sample->rootNote;
        
        // Cache the audio data in memory
        sample->sampleRate = reader->sampleRate;
        sample->numChannels = reader->numChannels;
        sample->lengthInSamples = reader->lengthInSamples;
        
        auto buffer = std::make_shared<juce::AudioBuffer<float>>(
            (int)reader->numChannels,
            (int)reader->lengthInSamples
        );

        reader->read(buffer.get(), 0, (int)reader->lengthInSamples, 0, true, true);
        sample->audioData = std::move(buffer);

        {
            juce::ScopedLock lock(sampleLock);
            samples.add(sample);
        }
        
        updateSamplerSounds();
        sampleCard.setSampleName(file.getFileName());
        sampleCard.setWaveform(file);  // Set the waveform
        
        // Calculate duration
        double durationInSeconds = reader->lengthInSamples / reader->sampleRate;
        sampleCard.setDuration(durationInSeconds);
        
        printf("Sample loaded (single mode): %s -> note %d (%lld samples, %.2f s)\n", 
               file.getFileName().toRawUTF8(), 
               sample->rootNote,
               reader->lengthInSamples, 
               durationInSeconds);
    }
}

//==============================================================================
// Add all other method implementations here (showMidiDeviceSettings, 
// handleIncomingMidiMessage, SampleListModel methods, sliderValueChanged,
// updateMappingUI, showMappingInterface, addSampleToMap, removeSelectedSample,
// clearAllSamples, updateSamplerSounds, etc.)

void MainComponent::updateMidiDeviceList()
{
    midiInputNames.clear();
    
    auto devices = juce::MidiInput::getAvailableDevices();
    
    for (auto& device : devices)
    {
        midiInputNames.add(device.name);
    }
    
    printf("Found %d MIDI input devices\n", devices.size());
    fflush(stdout);
}

void MainComponent::showMidiDeviceSettings()
{
    printf("Opening MIDI device settings...\n");
    fflush(stdout);

    updateMidiDeviceList();

    auto* selector = new ::MidiSelectorComponent(*this, midiInputNames);
    selector->setSize(400, 200);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "MIDI Input Settings";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    // IMPORTANT FIX: Don't try to capture the return value of launchAsync()
    // Instead, let the dialog manage itself and just track it with a weak reference
    options.launchAsync();
    
    // We can't store the dialog pointer here because launchAsync returns immediately
    // and the dialog is created asynchronously. Instead, we'll rely on the dialog
    // to clean itself up.
    
    printf("MIDI settings launched\n");
    fflush(stdout);
}


void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    // Filter out background MIDI messages that shouldn't trigger the light
    // Ignore MIDI clock and active sense messages as they're sent continuously
    if (message.isMidiClock() || message.isActiveSense())
    {
        // Don't print anything - just return immediately
        // These messages are ignored completely to avoid performance issues
        return;
    }
    
      // ALWAYS trigger the MIDI activity light for ANY MIDI message
      // This happens before any filtering so it shows activity from all channels
      midiActivityLight.triggerActivity();
      
      // Update MIDI activity light based on message type for more nuanced feedback
      if (message.isNoteOn())
      {
          midiActivityLight.noteOn();
      }
      else if (message.isNoteOff())
      {
          midiActivityLight.noteOff();
      }
      else
      {
          // For other MIDI messages (controllers, etc.) trigger a brief flash
          midiActivityLight.triggerActivity();
      }
      
      // Print ALL incoming MIDI messages for debugging (temporarily)
      // You can comment these out once everything is working
      if (message.isNoteOn())
      {
          printf("RAW MIDI Note On: %d, Vel: %d, Ch: %d\n", 
                 message.getNoteNumber(), 
                 message.getVelocity(),
                 message.getChannel());
      }
      else if (message.isNoteOff())
      {
          printf("RAW MIDI Note Off: %d, Ch: %d\n", 
                 message.getNoteNumber(),
                 message.getChannel());
      }
      else if (message.isController())
      {
          printf("RAW MIDI Controller: %d, Val: %d, Ch: %d\n",
                 message.getControllerNumber(),
                 message.getControllerValue(),
                 message.getChannel());
      }
      else if (message.isPitchWheel())
      {
          printf("RAW MIDI Pitch Wheel: %d, Ch: %d\n",
                 message.getPitchWheelValue(),
                 message.getChannel());
      }
      else if (message.isAftertouch())
      {
          printf("RAW MIDI Aftertouch: %d, Ch: %d\n",
                 message.getAfterTouchValue(),
                 message.getChannel());
      }
      else if (message.isChannelPressure())
      {
          printf("RAW MIDI Channel Pressure: %d, Ch: %d\n",
                 message.getChannelPressureValue(),
                 message.getChannel());
      }
      else if (message.isSysEx())
      {
          printf("RAW MIDI SysEx: %d bytes\n", message.getRawDataSize());
      }
      else if (message.isMidiStart() || message.isMidiStop() || message.isMidiContinue())
      {
          // These are transport messages - print them but they're less frequent
          if (message.isMidiStart()) printf("RAW MIDI Start\n");
          else if (message.isMidiStop()) printf("RAW MIDI Stop\n");
          else if (message.isMidiContinue()) printf("RAW MIDI Continue\n");
      }
    
    // ANTI-FLOOD PROTECTION: Ignore duplicate messages in quick succession
    static juce::uint64 lastMessageTime = 0;
    static int lastNoteNumber = -1;
    static int lastNoteCount = 0;
    static int totalIgnored = 0;
    
    juce::uint64 currentTime = juce::Time::getMillisecondCounter();
    int timeSinceLast = (int)(currentTime - lastMessageTime);
    
    // If we're in learn mode and get a note on message, handle it specially
    // In learn mode, we ignore channel filtering - learn from any channel
    if (isLearningMode && message.isNoteOn())
    {
        int currentNote = message.getNoteNumber();
        handleMidiLearn(currentNote);
        // Still add to collector so user can hear the note
        midiCollector.addMessageToQueue(message);
        
        printf("🎹 LEARN MODE: Captured note %d from channel %d\n", 
               currentNote, message.getChannel());
        return;
    }
    
    // Get the currently selected MIDI channel from the sample card
    int selectedChannel = sampleCard.getMidiChannel();
    
    // For normal operation, filter by selected channel if not "All Channels" (0)
    // Let's assume channel 0 means "All Channels"
    bool channelMatches = (selectedChannel == 0) || (message.getChannel() == selectedChannel);
    
    // If channel doesn't match and we're not in learn mode, ignore the message
    if (!channelMatches && !isLearningMode)
    {
        // The light still shows activity (we triggered it above), but audio is filtered
        return;
    }
    
    // If we're getting the same note message repeatedly within 10ms, ignore it
    if (message.isNoteOn() || message.isNoteOff())
    {
        int currentNote = message.getNoteNumber();
        
        if (currentNote == lastNoteNumber && timeSinceLast < 10)
        {
            lastNoteCount++;
            totalIgnored++;
            
            // If we've seen this note more than 5 times in a row within 10ms, ignore it
            if (lastNoteCount > 5)
            {
                // Only print occasionally to avoid console flood
                if (lastNoteCount % 100 == 0)
                {
                    printf("⚠️ Flood protection: Ignored %d duplicate messages on note %d (last interval: %dms)\n", 
                           totalIgnored, currentNote, timeSinceLast);
                }
                return;  // IGNORE THE MESSAGE
            }
        }
        else
        {
            // New note or timing out - reset counter
            if (lastNoteCount > 5)
            {
                printf("✅ Flood ended - normal playing resumed (ignored %d messages total)\n", totalIgnored);
                totalIgnored = 0;
            }
            lastNoteNumber = currentNote;
            lastNoteCount = 0;
        }
        
        lastMessageTime = currentTime;
    }
    
    // Only add to collector if we passed the flood filter
    midiCollector.addMessageToQueue(message);

    // One-shot tail detection: when a note-off arrives while 1Shot is ON,
    // the audio engine ignores it and plays to completion.  Start polling
    // voice activity so we know when to stop the button pulse.
    if (message.isNoteOff() && sampleCard.isOneShotEnabled() && !isOneShotTailPlaying)
    {
        isOneShotTailPlaying = true;
        juce::MessageManager::callAsync([this] { sampleCard.setOneShotTailActive(true); });
        oneShotTailTimer.startTimer(200);
        printf("[1SHOT] Note-off received — tail playing, polling for completion\n");
    }
    
    // Print human-performed notes normally (no throttling for these)
    if (message.isNoteOn() && lastNoteCount <= 5)
    {
        printf("🎹 Note On: %d, Vel: %d, Ch: %d (interval: %dms)\n", 
               message.getNoteNumber(), 
               message.getVelocity(),
               message.getChannel(),
               timeSinceLast);
    }
    else if (message.isNoteOff() && lastNoteCount <= 5)
    {
        printf("🎹 Note Off: %d, Ch: %d\n", 
               message.getNoteNumber(),
               message.getChannel());
    }
    else if (message.isController())
    {
        int controller = message.getControllerNumber();
        int value = message.getControllerValue();
        printf("MIDI Controller: %d, Value: %d\n", controller, value);
    }
}


void MainComponent::sliderValueChanged(juce::Slider* slider)
{
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        
        if (slider == &lowNoteSlider)
        {
            int newLow = (int)lowNoteSlider.getValue();
            // Manual bounds checking to avoid macro conflict
            if (newLow < 0) newLow = 0;
            if (newLow > 127) newLow = 127;
            
            if (newLow <= sample->highNote)
            {
                sample->lowNote = newLow;
                printf("Sample %s: low note set to %d\n", sample->name.toRawUTF8(), newLow);
            }
            else
            {
                lowNoteSlider.setValue(sample->lowNote);
            }
        }
        else if (slider == &highNoteSlider)
        {
            int newHigh = (int)highNoteSlider.getValue();
            if (newHigh >= sample->lowNote)
            {
                sample->highNote = newHigh;
                printf("Sample %s: high note set to %d\n", sample->name.toRawUTF8(), newHigh);
            }
            else
            {
                highNoteSlider.setValue(sample->highNote);
            }
        }
        else if (slider == &rootNoteSlider)
        {
            sample->rootNote = (int)rootNoteSlider.getValue();
            printf("Sample %s: root note set to %d\n", sample->name.toRawUTF8(), sample->rootNote);
        }
        
        // Update sampler with new mapping
        updateSamplerSounds();
        sampleListBox.updateContent();
    }
    fflush(stdout);
}

void MainComponent::updateMappingUI()
{
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        
        lowNoteSlider.setValue(sample->lowNote, juce::dontSendNotification);
        highNoteSlider.setValue(sample->highNote, juce::dontSendNotification);
        rootNoteSlider.setValue(sample->rootNote, juce::dontSendNotification);
        
        lowNoteSlider.setEnabled(true);
        highNoteSlider.setEnabled(true);
        rootNoteSlider.setEnabled(true);
    }
    else
    {
        lowNoteSlider.setEnabled(false);
        highNoteSlider.setEnabled(false);
        rootNoteSlider.setEnabled(false);
    }
}

// Also make sure you have these methods implemented (they might be missing too):

void MainComponent::showMappingInterface()
{
    printf("Opening sample mapping interface...\n");
    fflush(stdout);

    // Use the separated UI component
    auto* mappingUI = new ::MappingComponent(*this);
    
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(mappingUI);
    options.dialogTitle = "Sample Mapping";
    options.dialogBackgroundColour = juce::Colours::lightgrey;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    
    options.launchAsync();
    
    printf("Mapping interface launched\n");
    fflush(stdout);
}

void MainComponent::addSampleToMap()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select sample files",
        currentFolder.exists() ? currentFolder : juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        formatManager.getWildcardForAllFormats()
    );
    
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | 
                             juce::FileBrowserComponent::canSelectMultipleItems,
                             [this](const juce::FileChooser& fc)
    {
        auto results = fc.getResults();
        for (auto& file : results)
        {
            loadSampleFile(file);
        }
        sampleListBox.updateContent();
        updateMappingUI();
    });
}

void MainComponent::removeSelectedSample()
{
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        juce::String sampleName = samples[selectedSampleIndex]->name;
        samples.remove(selectedSampleIndex);
        selectedSampleIndex = -1;
        updateSamplerSounds();
        sampleListBox.updateContent();
        updateMappingUI();
        
        printf("Sample '%s' removed. Total samples: %d\n", 
               sampleName.toRawUTF8(), samples.size());
        fflush(stdout);
    }
}

void MainComponent::clearAllSamples()
{
    samples.clear();
    selectedSampleIndex = -1;
    sampler.clearSounds();
    sampleListBox.updateContent();
    updateMappingUI();
    sampleCard.clearWaveform();  // Clear the waveform
    sampleCard.setSampleName("No sample loaded");
    sampleCard.setDuration(0.0);
    
    printf("All samples cleared\n");
    fflush(stdout);
}

void MainComponent::updateSamplerSounds()
{
    sampler.clearSounds();
    
    for (auto* sample : samples)
    {
        if (sample == nullptr || sample->audioData == nullptr) 
            continue;
        
        // Tiny silent reader — only needed to satisfy SamplerSound's constructor.
        // LoopingSamplerVoice reads from sound->fullAudioData (the full original buffer) instead.
        class DummyAudioReader : public juce::AudioFormatReader
        {
        public:
            DummyAudioReader(double sr, unsigned int ch)
                : juce::AudioFormatReader(nullptr, "Dummy")
            {
                sampleRate            = sr;
                numChannels           = ch;
                lengthInSamples       = 1;
                bitsPerSample         = 32;
                usesFloatingPointData = true;
            }
            bool readSamples(int* const* dest, int numDest, int startOff,
                             juce::int64 /*startInFile*/, int num) override
            {
                for (int ch = 0; ch < numDest; ++ch)
                    if (dest[ch] != nullptr)
                        memset(reinterpret_cast<float*>(dest[ch]) + startOff, 0,
                               (size_t)num * sizeof(float));
                return true;
            }
        };
        DummyAudioReader dummyReader(sample->sampleRate, (unsigned int)sample->numChannels);

        // Compute start/end absolute sample positions for atomic real-time updates.
        const juce::int64 bufTotal = (juce::int64)sample->audioData->getNumSamples();
        juce::int64 startSample = 0;
        juce::int64 endSample   = juce::jmax((juce::int64)1, bufTotal - 1); // leave 1 guard sample

        if (sample->startPointSeconds > 0.0 && sample->sampleRate > 0)
        {
            startSample = (juce::int64)(sample->startPointSeconds * sample->sampleRate);
            startSample = juce::jlimit((juce::int64)0, endSample - 1, startSample);
        }
        if (sample->endPointSeconds > 0.0 && sample->sampleRate > 0)
        {
            endSample = (juce::int64)(sample->endPointSeconds * sample->sampleRate);
            endSample = juce::jlimit(startSample + 1, bufTotal - 1, endSample);
        }

        juce::BigInteger noteRange;
        noteRange.setRange(0, 128, false);
        noteRange.setBit(sample->rootNote);

        printf("Creating sound: %s -> triggers on note %d, pitch offset %+d\n",
               sample->name.toRawUTF8(), sample->rootNote, sample->pitchOffset);

        // midiRootNote = rootNote (no offset baked in).
        // The voice applies pitchOffsetAtomic per block so it can update in real-time.
        auto* sound = new LoopingSamplerSound(
            sample->name,
            dummyReader,
            noteRange,
            sample->rootNote,
            sample->attack,
            sample->release,
            10.0
        );
        sound->fullAudioData = sample->audioData;       // shared_ptr copy — keeps buffer alive
        sound->startSampleAtomic.store(startSample);
        sound->endSampleAtomic.store(endSample);
        sound->loopEnabled.store(loopEnabled.load());
        sound->pitchOffsetAtomic.store(sample->pitchOffset);
        sound->oneShotEnabled.store(sampleCard.isOneShotEnabled());
        sound->baseTuningRatioAtomic.store((float)(sampleCard.getBaseTuningHz() / 440.0));
        sound->customAdsrEnabled.store(sampleCard.isAdsrEnabled());
        sound->customAdsrAttackMs.store(sampleCard.getAdsrAttackMs());
        sound->customAdsrDecayMs.store(sampleCard.getAdsrDecayMs());
        sound->customAdsrSustain.store(sampleCard.getAdsrSustain());
        sound->customAdsrReleaseMs.store(sampleCard.getAdsrReleaseMs());
        sound->reverseEnabled.store(sampleCard.isReverseEnabled());
        sound->bounceEnabled.store(sampleCard.isBounceEnabled());

        sampler.addSound(sound);

        printf("Added sound: %s -> note %d, pitchOffset %+d\n",
               sample->name.toRawUTF8(), sample->rootNote, sample->pitchOffset);
    }
    
    printf("Sampler updated with %d sounds (each mapped to single note)\n", samples.size());
    fflush(stdout);
}

//==============================================================================
// New UI functionality implementations
void MainComponent::showSettingsMenu()
{
    juce::PopupMenu menu;
    
    menu.addItem(1, "Audio Settings");
    menu.addItem(2, "MIDI Settings");
    
    menu.showMenuAsync(juce::PopupMenu::Options()
                       .withTargetComponent(&menuButton)
                       .withMinimumWidth(150),
                       [this](int result)
                       {
                           if (result == 1)
                           {
                               showAudioDeviceSettings();
                           }
                           else if (result == 2)
                           {
                               showMidiDeviceSettings();
                           }
                       });
}

void MainComponent::scanCurrentFolderForAudioFiles()
{
    if (isScanning.exchange(true))  // If already scanning, return
        return;
    
    backgroundThreads.addJob([this]() {
        juce::Array<juce::File> newFiles;
        
        if (currentFolder.exists())
        {
            juce::Array<juce::File> allFiles;
            currentFolder.findChildFiles(allFiles, juce::File::findFiles, false);
            
            // Filter for audio files
            for (auto& file : allFiles)
            {
                if (formatManager.findFormatForFileExtension(file.getFileExtension()) != nullptr)
                {
                    newFiles.add(file);
                }
            }
            
            newFiles.sort();
        }
        
        // Update UI on message thread
        juce::MessageManager::callAsync([this, newFiles]() {
            // Capture current file before lock so we can find it in the new list
            juce::File currentFile;
            {
                juce::ScopedLock lock(sampleLock);
                if (!samples.isEmpty())
                    currentFile = samples[0]->file;
            }

            {
                juce::ScopedWriteLock lock(folderLock);
                folderAudioFiles = newFiles;
            }
            isScanning = false;

            // Set currentFileIndex to point at the currently loaded sample
            if (currentFile.existsAsFile())
            {
                for (int i = 0; i < folderAudioFiles.size(); ++i)
                {
                    if (folderAudioFiles[i] == currentFile)
                    {
                        currentFileIndex = i;
                        break;
                    }
                }
            }

            printf("Scanned folder: %s, found %d audio files (currentFileIndex=%d)\n",
                   currentFolder.getFullPathName().toRawUTF8(),
                   newFiles.size(), currentFileIndex);
            fflush(stdout);

            // Fire any pending lazy-navigation action (set by loadNextSample/loadPrevSample
            // when the folder hadn't been scanned yet).
            if (postScanAction)
            {
                auto action = std::move(postScanAction);
                postScanAction = nullptr;
                action();
            }
        });
    });
}

void MainComponent::navigateToFile(int index)
{
    if (folderAudioFiles.isEmpty() || index < 0 || index >= folderAudioFiles.size())
        return;

    // Fix 1 — Increment generation: any previously queued job that has not yet started
    // will see a mismatched generation and abort immediately without loading.
    const int myGeneration = navigationGeneration.fetch_add(1) + 1;

    // Record navigation start time for latency measurement
    navStartTimeMs = static_cast<juce::int64>(juce::Time::getMillisecondCounter());
    printf("[LOAD-TIMING] navigateToFile started for '%s'\n",
           folderAudioFiles[index].getFileName().toRawUTF8());
    fflush(stdout);

    // Step 1 — Save current sample state to disk BEFORE anything changes.
    // Must be synchronous here so the card still holds the true values.
    saveOutgoingSampleState();

    // Reset both deferred timers: if user navigates again before they fire, the
    // timers restart — saves and transient detection only run once navigation stops.
    navSaveTimer.stopTimer();
    transientDetectionTimer.stopTimer();

    currentFileIndex = index;
    auto file = folderAudioFiles[index];

    // Step 2 — Show loading indicator immediately (before background thread starts).
    sampleCard.showLoadingState();

    // Pitch is NOT touched during Next/Prev — it stays exactly as the user left it.
    printf("[PITCH] Next/Prev: pitch stays at %+d (not changed)\n", sampleCard.getPitchOffset());

    // Steps 3-7 happen inside loadSampleFileAsync on the background thread.
    // addJob() returns immediately — the message thread is NOT blocked.
    backgroundThreads.addJob([this, file, myGeneration]() {
        // Fix 1 — Stale job check: if a newer navigation has been triggered since this
        // job was queued, discard it immediately without reading any audio data.
        if (myGeneration != navigationGeneration.load())
        {
            printf("[NAV] Cancelled pending job (gen %d), starting new load for '%s'\n",
                   myGeneration, file.getFileName().toRawUTF8());
            fflush(stdout);
            return;
        }
        loadSampleFileAsync(file, true, false, /*deferTransients=*/true);
    });
    printf("[ASYNC] Background load started for '%s' — returning to UI immediately\n",
           file.getFileName().toRawUTF8());
    fflush(stdout);
}

void MainComponent::loadSampleFileAsync(const juce::File& file, bool autoPlay, bool resetZoom, bool deferTransients)
{
    // ── Background thread: read audio data ────────────────────────────────────────
    const juce::int64 tBgStart = (juce::int64)juce::Time::getMillisecondCounter();
    printf("[LOAD-TIMING] background job started for '%s'\n",
           file.getFileName().toRawUTF8());
    fflush(stdout);

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr)
    {
        printf("[PERSIST] ERROR: Cannot read file: %s\n", file.getFileName().toRawUTF8());
        fflush(stdout);
        return;
    }

    printf("[PERSIST] Reading audio: %s  (%lld samples, %d ch, %.1f kHz)\n",
           file.getFileName().toRawUTF8(),
           reader->lengthInSamples, reader->numChannels, reader->sampleRate / 1000.0);

    auto* sample = new MappedSample();
    sample->file   = file;
    sample->name   = file.getFileName();
    int currentNote = sampleCard.getMidiNote();
    sample->rootNote = currentNote;
    sample->lowNote  = currentNote;
    sample->highNote = currentNote;
    sample->sampleRate      = reader->sampleRate;
    sample->numChannels     = reader->numChannels;
    sample->lengthInSamples = reader->lengthInSamples;
    sample->attack  = 0.01;
    sample->release = 0.1;

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(
        (int)reader->numChannels, (int)reader->lengthInSamples);

    if (!reader->read(buffer.get(), 0, (int)reader->lengthInSamples, 0, true, true))
    {
        printf("[PERSIST] ERROR: Failed to decode audio: %s\n", file.getFileName().toRawUTF8());
        delete sample;
        return;
    }
    sample->audioData = std::move(buffer);

    printf("[LOAD-TIMING] file read complete: %lldms\n",
           (juce::int64)juce::Time::getMillisecondCounter() - tBgStart);
    fflush(stdout);

    // ── Pre-compute waveform peaks on background thread ───────────────────────────
    // This eliminates all disk I/O from WaveformComponent::paint() — the bottleneck
    // that was causing the 7-second message-thread freeze on every sample load.
    const juce::int64 tPeakStart = (juce::int64)juce::Time::getMillisecondCounter();
    const int    peakNumCh  = (int)reader->numChannels;
    const juce::int64 peakNSamples = reader->lengthInSamples;
    const double peakSR     = reader->sampleRate;

    auto peaks = std::make_unique<WaveformPeakBin[]>(kWaveformPeakBins);
    {
        const float* left  = sample->audioData->getReadPointer(0);
        const float* right = (peakNumCh > 1) ? sample->audioData->getReadPointer(1) : left;
        for (int bin = 0; bin < kWaveformPeakBins; ++bin)
        {
            const juce::int64 sStart = (juce::int64)((double)bin       / kWaveformPeakBins * peakNSamples);
            const juce::int64 sEnd   = (juce::int64)((double)(bin + 1) / kWaveformPeakBins * peakNSamples);
            const juce::int64 safeEnd = juce::jmin(sEnd, peakNSamples);

            WaveformPeakBin& pb = peaks[bin];
            pb.minL = pb.minR = 1.0f;
            pb.maxL = pb.maxR = -1.0f;

            for (juce::int64 s = sStart; s < safeEnd; ++s)
            {
                pb.minL = juce::jmin(pb.minL, left[s]);
                pb.maxL = juce::jmax(pb.maxL, left[s]);
                pb.minR = juce::jmin(pb.minR, right[s]);
                pb.maxR = juce::jmax(pb.maxR, right[s]);
            }
        }
    }
    printf("[LOAD-TIMING] peak computation done: %lldms  (%lld samples → %d bins)\n",
           (juce::int64)juce::Time::getMillisecondCounter() - tPeakStart,
           peakNSamples, kWaveformPeakBins);
    fflush(stdout);

    // ── Message thread: stop audio, restore state, rebuild sampler ────────────────
    juce::MessageManager::callAsync([this, sample, file, autoPlay, resetZoom, deferTransients,
                                      peaks = std::move(peaks), peakNumCh, peakNSamples, peakSR,
                                      tBgStart]() mutable {

        const juce::int64 tMsgStart = (juce::int64)juce::Time::getMillisecondCounter();
        printf("[LOAD-TIMING] callAsync lambda started: %lldms after bg job\n",
               tMsgStart - tBgStart);
        fflush(stdout);

        // ── Step 2: Stop all audio completely ─────────────────────────────────────
        muteOutput.store(true);   // audio thread bails immediately

        // Stop any one-shot tail poll (new sample cancels the old tail)
        if (isOneShotTailPlaying)
        {
            isOneShotTailPlaying = false;
            oneShotTailTimer.stopTimer();
            sampleCard.setOneShotTailActive(false);
        }

        // Clear freeze/loop/oneshot flags so stopNote() fires correctly
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                s->freezeActive.store(false);
                s->loopEnabled.store(false);
                s->oneShotEnabled.store(false);
            }
        // Force-stop every voice: releases currentlyPlayingSound ref-count directly
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
                v->forceStop();
        sampler.clearSounds();
        sampler.allNotesOff(0, false);

        // ── Step 3: Reset card to neutral state ───────────────────────────────────
        sampleCard.resetStartPoint();
        sampleCard.resetEndPoint();
        sampleCard.setLoopEnabled(false);
        sampleCard.resetFreeze();
        sampleCard.resetBounce();

        // ── Step 4: Install new sample ────────────────────────────────────────────
        {
            juce::ScopedLock lock(sampleLock);
            samples.clear();
            selectedSampleIndex = 0;
            samples.add(sample);
        }

        sampleCard.setSampleName(file.getFileName());

        // Deliver pre-computed peaks BEFORE setWaveform() so the very first repaint
        // uses the peak data — zero disk I/O in paint() from this point forward.
        printf("[LOAD-TIMING] setting waveform peaks + waveform: %lldms\n",
               (juce::int64)juce::Time::getMillisecondCounter() - tMsgStart);
        sampleCard.setWaveformPeaks(std::move(peaks), peakNumCh, peakNSamples, peakSR);

        // Pass sample metadata already read on the background thread — this avoids
        // a second createReaderFor() call on the message thread (was the 7s freeze for MP3).
        // During navigation (deferTransients=true) skip the blocking detectTransients()
        // call — the deferred timer will run it 800ms after navigation stops.
        sampleCard.setWaveform(file, /*skipTransients=*/deferTransients,
                               /*knownTotalSamples=*/peakNSamples,
                               /*knownSampleRate=*/peakSR);
        sampleCard.setDuration(sample->lengthInSamples / sample->sampleRate);
        printf("[LOAD-TIMING] waveform set: %lldms\n",
               (juce::int64)juce::Time::getMillisecondCounter() - tMsgStart);

        sample->rootNote = sampleCard.getMidiNote();
        sample->lowNote  = sample->rootNote;
        sample->highNote = sample->rootNote;

        // ── Steps 5-7: Restore per-sample state (start/end/vol/loop only) ───────────
        // Pitch is NOT per-sample — it is never read or written here.
        // Pitch is left exactly as it currently is on the card (global session value).
        double effectiveStart = 0.0;
        double effectiveEnd   = -1.0;   // -1 = full sample length
        float  effectiveVol   = 1.0f;
        bool   effectiveLoop  = false;

        printf("[PERSIST] --- Loading: %s ---\n", file.getFileName().toRawUTF8());

        if (configManager != nullptr)
        {
            auto state = configManager->getSampleState(file);
            if (state.exists)
            {
                // Saved settings found — restore start/end/vol/loop/thresh for this file
                effectiveStart = state.startPoint;
                effectiveEnd   = state.endPoint;
                effectiveVol   = state.volume;
                effectiveLoop  = state.loopEnabled;
                // Restore transient threshold (re-runs detection with saved sensitivity)
                sampleCard.setTransientThreshold(state.transientThreshold);
                // Restore detected note and hidden base offset (quiet — no listener fired)
                sampleCard.setDetectedNoteName(state.detectedNoteName, state.detectedFreqHz);
                sampleCard.setBasePitchOffset(state.basePitchOffset);
                // Restore ADSR envelope state.
                // notifyListeners=false: silent restore, no save triggered here.
                // The final saveCurrentSampleState() at end of lambda saves the correct values.
                printf("[ADSR LOAD] Reading from disk: adsrEnabled=%s  atk=%.0fms  dcy=%.0fms  sus=%.0f%%  rel=%.0fms\n",
                       state.adsrEnabled ? "true" : "false",
                       state.adsrAttackMs, state.adsrDecayMs,
                       state.adsrSustain * 100.0f, state.adsrReleaseMs);
                sampleCard.setAdsrParams(state.adsrEnabled, state.adsrAttackMs, state.adsrDecayMs,
                                         state.adsrSustain, state.adsrReleaseMs, /*notifyListeners=*/false);
                printf("[ADSR LOAD] Applied to SampleCard  →  card reports: en=%s atk=%.0f\n",
                       sampleCard.isAdsrEnabled() ? "true" : "false", sampleCard.getAdsrAttackMs());

                // Restore EQ state silently (no listener = no redundant save)
                sampleCard.setEqParams(state.eqEnabled,
                                       state.eq1Freq, state.eq1Gain, state.eq1Q,
                                       state.eq2Freq, state.eq2Gain, state.eq2Q,
                                       state.eq3Freq, state.eq3Gain, state.eq3Q,
                                       /*notifyListeners=*/false);
                // Restore EQ filter modes silently
                eqFilterModes[0] = state.eq1Mode;
                eqFilterModes[1] = state.eq2Mode;
                eqFilterModes[2] = state.eq3Mode;
                sampleCard.setEqFilterModes(state.eq1Mode, state.eq2Mode, state.eq3Mode, /*notify=*/false);
                // OPT 2+5: Compute EQ coefficients on UI thread, write to double buffer.
                // resetEqState() is safe here because muteOutput=true (audio thread not running).
                eqActive.store(state.eqEnabled);
                {
                    const double sr = sampler.getSampleRate() > 0.0 ? sampler.getSampleRate() : 44100.0;
                    EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
                    newCoeffs[0] = computeEqCoeffs(state.eq1Freq, state.eq1Gain, state.eq1Q, state.eq1Mode, sr);
                    newCoeffs[1] = computeEqCoeffs(state.eq2Freq, state.eq2Gain, state.eq2Q, state.eq2Mode, sr);
                    newCoeffs[2] = computeEqCoeffs(state.eq3Freq, state.eq3Gain, state.eq3Q, state.eq3Mode, sr);
                    eqCoeffDB.writeFromUI(newCoeffs);
                }
                resetEqState();  // OPT 5: always reset filter state on sample load (muteOutput=true)
                printf("[EQ] Restored filter modes: band1=%d  band2=%d  band3=%d\n",
                       state.eq1Mode, state.eq2Mode, state.eq3Mode);

                // Restore normalize state silently, then recompute gain if enabled.
                sampleCard.setNormParams(state.normEnabled, state.normTargetDb, /*notifyListeners=*/false);
                if (state.normEnabled)
                {
                    const float gain   = computeNormGainFromAudio(state.normTargetDb);
                    normGain.store(gain);
                    const float gainDb = (gain > 0.0f) ? 20.0f * std::log10f(gain) : 0.0f;
                    sampleCard.setNormGainDisplay(gainDb);
                }
                else
                {
                    normGain.store(1.0f);
                }

                printf("[PERSIST] RESTORED  start=%.3f  end=%.3f  vol=%.2f  loop=%s  thresh=%.1f  note=%s\n",
                       effectiveStart, effectiveEnd, effectiveVol,
                       effectiveLoop ? "ON" : "OFF",
                       state.transientThreshold,
                       state.detectedNoteName.isEmpty() ? "-" : state.detectedNoteName.toRawUTF8());
            }
            else
            {
                // New file — reset sensitivity to default, clear detection and base offset
                sampleCard.setTransientThreshold(4.0);
                sampleCard.setDetectedNoteName("", 0.0);
                sampleCard.setBasePitchOffset(0);
                // New file: reset EQ to defaults (OFF, all bands flat, all Bell mode)
                sampleCard.setEqParams(false, 100.0f,0.0f,1.0f, 500.0f,0.0f,1.0f, 8000.0f,0.0f,1.0f,
                                       /*notifyListeners=*/false);
                eqFilterModes[0] = eqFilterModes[1] = eqFilterModes[2] = 2;
                sampleCard.setEqFilterModes(2, 2, 2, /*notify=*/false);
                eqActive.store(false);
                resetEqState();  // OPT 5: reset filter state so old sample state doesn't bleed in
                // New file: reset normalize to defaults (OFF, target -6dB)
                sampleCard.setNormParams(false, -6.0f, /*notifyListeners=*/false);
                normGain.store(1.0f);
                printf("[PERSIST] NEW FILE — using defaults  start=0.0  end=full  vol=1.0  loop=OFF  thresh=4.0\n");
            }
        }

        // Pitch: total = global user offset + per-sample base (from Tune).
        // basePitchOffset was restored from SampleState above (0 if never tuned).
        sample->pitchOffset = sampleCard.getPitchOffset() + sampleCard.getBasePitchOffset() * 100;
        printf("[PITCH] Sample loaded: user=%+d cents  base=%+d st  total=%+d cents\n",
               sampleCard.getPitchOffset(), sampleCard.getBasePitchOffset(), sample->pitchOffset);

        sample->startPointSeconds = effectiveStart;
        sample->endPointSeconds   = effectiveEnd;

        // Apply start marker
        if (effectiveStart > 0.0)
            sampleCard.setStartPoint(effectiveStart);
        // else already reset above

        // Apply end marker
        if (effectiveEnd > 0.0)
            sampleCard.setEndPoint(effectiveEnd);
        // else already reset above (full length)

        // Apply volume
        volumeGain.store(effectiveVol);
        sampleCard.setVolume(effectiveVol);

        // Apply loop
        loopEnabled.store(effectiveLoop);
        sampleCard.setLoopEnabled(effectiveLoop);

        // Freeze is ALWAYS off — never persisted, reset already done above

        // ── Build sampler sounds with fully resolved state ─────────────────────────
        printf("[LOAD-TIMING] updateSamplerSounds start: %lldms\n",
               (juce::int64)juce::Time::getMillisecondCounter() - tMsgStart);
        updateSamplerSounds();
        printf("[LOAD-TIMING] updateSamplerSounds done: %lldms\n",
               (juce::int64)juce::Time::getMillisecondCounter() - tMsgStart);

        // Propagate current one-shot state to the freshly built sound(s).
        {
            const bool oneShot = sampleCard.isOneShotEnabled();
            for (int i = 0; i < sampler.getNumSounds(); ++i)
                if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                    s->oneShotEnabled.store(oneShot);
        }

        // Propagate current reverse state to the freshly built sound(s).
        {
            const bool rev = sampleCard.isReverseEnabled();
            for (int i = 0; i < sampler.getNumSounds(); ++i)
                if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                    s->reverseEnabled.store(rev);
        }

        muteOutput.store(false);

        // ── Timing: print ms from button press to audio ready ─────────────────────
        if (deferTransients && navStartTimeMs > 0)
        {
            const juce::int64 elapsed =
                static_cast<juce::int64>(juce::Time::getMillisecondCounter()) - navStartTimeMs;
            printf("[NAV-TIMING] Audio ready in %lldms for '%s'\n",
                   elapsed, file.getFileName().toRawUTF8());
            fflush(stdout);
        }

        sampleCard.setMidiNote(sample->rootNote);
        // Do NOT call setPitchOffset here — pitch display was not changed during load

        // Zoom: reset for new sample (+button), restore saved zoom for Prev/Next/startup
        if (resetZoom)
        {
            // + button load: setWaveform() already reset zoom to 1.0 — just save
            if (configManager != nullptr)
            {
                configManager->saveZoomLevel(1.0f);
                configManager->saveZoomScrollPosition(0.0f);
            }
            printf("[ZOOM] New sample load: zoom reset to 1.0x\n");
        }
        else
        {
            // Navigation or startup: restore previously saved zoom and scroll
            const float savedZoom   = configManager != nullptr ? configManager->getZoomLevel() : 1.0f;
            const float savedScroll = configManager != nullptr ? configManager->getZoomScrollPosition() : 0.0f;
            if (savedZoom > 1.001f)
            {
                sampleCard.restoreZoomAndScroll((double)savedZoom, savedScroll);
                printf("[ZOOM] Restored: %.2fx  scroll=%.3f\n", savedZoom, savedScroll);
            }
        }

        if (deferTransients)
        {
            // Opt 2: Batch disk saves — wait 500ms after navigation stops, write once.
            // Opt 3: Detect transients — wait 800ms after navigation stops.
            // Starting the timer again resets it, so rapid navigation only fires once.
            navSaveTimer.startTimer(500);
            transientDetectionTimer.startTimer(800);
            printf("[NAV-OPT] Deferred save (500ms) + transient detection (800ms) scheduled\n");
        }
        else
        {
            // Normal load (+button, startup): save immediately as before.
            saveCurrentSampleState();
            saveCurrentSession();
        }

        printf("[PERSIST] --- Load complete: %s ---\n", file.getFileName().toRawUTF8());
        printf("[LOAD-TIMING] total message-thread work: %lldms\n",
               (juce::int64)juce::Time::getMillisecondCounter() - tMsgStart);
        fflush(stdout);

        // Optional preview note
        if (autoPlay)
        {
            juce::MessageManager::callAsync([this, sample]() {
                sampler.noteOn(1, sample->rootNote, 0.8f);
                juce::Timer::callAfterDelay(800, [this, sample]() {
                    sampler.noteOff(1, sample->rootNote, 0.0f, true);
                });
            });
        }
    });
}

void MainComponent::loadNextSample()
{
    if (folderAudioFiles.isEmpty())
    {
        // Folder not scanned yet — trigger lazy scan and navigate when done
        if (currentFolder.exists() && !isScanning)
        {
            printf("[NAV] Folder not scanned yet — triggering lazy scan, then navigating next\n");
            fflush(stdout);
            postScanAction = [this]() { loadNextSample(); };
            scanCurrentFolderForAudioFiles();
        }
        return;
    }

    // Fix 2 — Debounce: update the index immediately (so rapid presses accumulate
    // correctly), but only start the actual file load after 50ms of no new presses.
    if (currentFileIndex < 0)
        currentFileIndex = 0;
    else
        currentFileIndex = (currentFileIndex + 1) % folderAudioFiles.size();

    if (navDebounceTimer.isTimerRunning())
    {
        printf("[NAV] Debounced — skipping intermediate file, jumping to '%s'\n",
               folderAudioFiles[currentFileIndex].getFileName().toRawUTF8());
        fflush(stdout);
    }
    navDebounceTimer.startTimer(50);  // restart; fires once presses stop
}

void MainComponent::loadPrevSample()
{
    if (folderAudioFiles.isEmpty())
    {
        // Folder not scanned yet — trigger lazy scan and navigate when done
        if (currentFolder.exists() && !isScanning)
        {
            printf("[NAV] Folder not scanned yet — triggering lazy scan, then navigating prev\n");
            fflush(stdout);
            postScanAction = [this]() { loadPrevSample(); };
            scanCurrentFolderForAudioFiles();
        }
        return;
    }

    // Fix 2 — Debounce: update the index immediately (so rapid presses accumulate
    // correctly), but only start the actual file load after 50ms of no new presses.
    if (currentFileIndex < 0)
        currentFileIndex = folderAudioFiles.size() - 1;
    else
        currentFileIndex = (currentFileIndex - 1 + folderAudioFiles.size()) % folderAudioFiles.size();

    if (navDebounceTimer.isTimerRunning())
    {
        printf("[NAV] Debounced — skipping intermediate file, jumping to '%s'\n",
               folderAudioFiles[currentFileIndex].getFileName().toRawUTF8());
        fflush(stdout);
    }
    navDebounceTimer.startTimer(50);  // restart; fires once presses stop
}

void MainComponent::fireDebounceNavigation()
{
    // Called by NavDebounceTimer 50ms after the last Prev/Next press.
    // currentFileIndex has already been updated to the final target by loadNext/PrevSample().
    if (!folderAudioFiles.isEmpty() && currentFileIndex >= 0 && currentFileIndex < folderAudioFiles.size())
        navigateToFile(currentFileIndex);
}

//==============================================================================
// SampleCard::Listener implementation
void MainComponent::midiNoteChanged(int newNote)
{
    // Always update the current sample if one is selected
    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        sample->rootNote = newNote;
        
        // Also update low/high notes to match for single-note mode
        sample->lowNote = newNote;
        sample->highNote = newNote;
        
        printf("Sample root note updated: %s -> %d (%s) (applied immediately)\n", 
               sample->name.toRawUTF8(),
               newNote,
               juce::MidiMessage::getMidiNoteName(newNote, true, true, true).toRawUTF8());
        
        // Update sampler with new mapping IMMEDIATELY
        updateSamplerSounds();
        
        // SAVE THE SESSION whenever MIDI note changes
        saveCurrentSession();
    }
    else
    {
        printf("No sample selected to apply MIDI note change\n");
        
        // Even if no sample is selected, save the MIDI note for future samples
        saveCurrentSession();
    }
}

void MainComponent::midiChannelChanged(int newChannel)
{
    printf("MIDI channel filter set to: %s\n", 
           newChannel == 0 ? "All Channels" : juce::String(newChannel).toRawUTF8());
    
    // The actual filtering happens in handleIncomingMidiMessage
    // No need to update samples, but we might want to stop currently playing notes
    // when changing channels to avoid stuck notes
    if (newChannel != sampleCard.getMidiChannel())
    {
        // Stop all notes when changing channels to avoid confusion
        sampler.allNotesOff(1, false);
    }
    
    // SAVE THE SESSION when MIDI channel changes
    saveCurrentSession();
}

void MainComponent::learningModeChanged(bool isLearning)
{
    isLearningMode = isLearning;
    printf("MIDI Learn mode: %s\n", isLearning ? "ON" : "OFF");
}

void MainComponent::pitchOffsetChanged(int userPitchOffsetCents)
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();

    // userPitchOffsetCents: user relative offset in CENTS (not semitones).
    // basePitchOffset: whole semitones from Tune auto-correction → multiply ×100 to get cents.
    const int totalCents = userPitchOffsetCents + sampleCard.getBasePitchOffset() * 100;

    // Update the in-memory sample struct with the TOTAL offset
    {
        juce::ScopedLock lock(sampleLock);
        if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
            samples[selectedSampleIndex]->pitchOffset = totalCents;
    }

    // Push TOTAL (in cents) to all live sounds atomically — no rebuild, no note cutoff.
    // Audio thread reads pitchOffsetAtomic once per block; 10ms IIR ramp smoothes the transition.
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            sound->pitchOffsetAtomic.store(totalCents);

    const juce::int64 tAtomic = juce::Time::getMillisecondCounter();
    printf("[PITCH-TIMING] updateSamplerSounds called: 0ms — SKIPPED (atomic pitchOffsetAtomic store)\n");
    printf("[PITCH-TIMING] Ramp started: target=%.6f (10ms IIR smoothing on audio thread)\n",
           std::pow(2.0, totalCents / 1200.0));

    // In-memory save only (no disk flush) — deferred 500ms timer handles the flush.
    if (configManager != nullptr)
        configManager->savePitchOffset(userPitchOffsetCents);  // in-memory setValue only

    // Restart debounce timer — one disk write fires 500ms after the last pitch change.
    pitchSaveTimer.startTimer(500);

    const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
    printf("[PITCH-TIMING] saveCurrentSampleState called: deferred 500ms\n");
    printf("[PITCH-TIMING] total handler time: %lldms  (user=%+d cents  total=%+d cents)\n",
           (long long)elapsed, userPitchOffsetCents, totalCents);
    fflush(stdout);
}

void MainComponent::volumeChanged(float volume)
{
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    volumeGain.store(volume);
    if (configManager != nullptr)
        configManager->saveVolume(volume);  // in-memory setValue only, no flush
    volSaveTimer.startTimer(400);           // one disk write fires 400ms after dragging stops
    printf("[KNOB-TIMING] Vol updated atomically: %.0fms — no rebuild  (vol=%.2f)\n",
           juce::Time::getMillisecondCounterHiRes() - t0, volume);
    fflush(stdout);
}

void MainComponent::startPointChanged(double startPointSeconds)
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();

    double sampleRate = 0.0;
    {
        juce::ScopedLock lock(sampleLock);
        if (!samples.isEmpty())
        {
            samples[0]->startPointSeconds = startPointSeconds;
            sampleRate = samples[0]->sampleRate;
        }
    }

    // Push new start position to all live sounds atomically — no rebuild, no note cutoff.
    if (sampleRate > 0.0)
    {
        const juce::int64 newStart = (juce::int64)(startPointSeconds * sampleRate);
        for (int i = 0; i < sampler.getNumSounds(); ++i)
        {
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                // Keep start below end — clamp to endSampleAtomic - 1
                const juce::int64 curEnd = sound->endSampleAtomic.load();
                sound->startSampleAtomic.store(juce::jmin(newStart, curEnd - 1));
            }
        }
    }

    if (configManager != nullptr)
        configManager->saveStartPoint(startPointSeconds);  // in-memory only, no flush

    markerSaveTimer.startTimer(400);  // one disk write 400ms after last movement

    const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
    printf("[MARKER-TIMING] Start position updated atomically: %lldms — no sound rebuild  (%.3fs)\n",
           (long long)elapsed, startPointSeconds);
    fflush(stdout);
}

void MainComponent::endPointChanged(double endPointSeconds)
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();

    double sampleRate     = 0.0;
    juce::int64 bufLen    = 0;
    {
        juce::ScopedLock lock(sampleLock);
        if (!samples.isEmpty())
        {
            samples[0]->endPointSeconds = endPointSeconds;
            sampleRate = samples[0]->sampleRate;
            if (samples[0]->audioData != nullptr)
                bufLen = samples[0]->audioData->getNumSamples();
        }
    }

    // Push new end position to all live sounds atomically — no rebuild, no note cutoff.
    if (sampleRate > 0.0 && bufLen > 0)
    {
        juce::int64 newEnd = (endPointSeconds > 0.0)
                                 ? (juce::int64)(endPointSeconds * sampleRate)
                                 : bufLen;
        newEnd = juce::jmin(newEnd, bufLen);

        for (int i = 0; i < sampler.getNumSounds(); ++i)
        {
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                // Keep end above start — clamp to startSampleAtomic + 1
                const juce::int64 curStart = sound->startSampleAtomic.load();
                sound->endSampleAtomic.store(juce::jmax(newEnd, curStart + 1));
            }
        }
    }

    if (configManager != nullptr)
        configManager->saveEndPoint(endPointSeconds);  // in-memory only, no flush

    markerSaveTimer.startTimer(400);  // one disk write 400ms after last movement

    const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
    printf("[MARKER-TIMING] End position updated atomically: %lldms — no sound rebuild  (%.3fs)\n",
           (long long)elapsed, endPointSeconds);
    fflush(stdout);
}

void MainComponent::loopEnabledChanged(bool isLooping)
{
    loopEnabled.store(isLooping);

    // Update the flag on all currently loaded sounds — no rebuild needed
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            sound->loopEnabled.store(isLooping);

    if (configManager != nullptr)
        configManager->saveLoopEnabled(isLooping);  // in-memory setValue only

    loopSaveTimer.startTimer(400);  // one disk write 400ms after toggle
    printf("Loop %s — save deferred 400ms\n", isLooping ? "ON" : "OFF");
    fflush(stdout);
}

void MainComponent::gridSnapChanged(bool isEnabled)
{
    if (configManager != nullptr)
        configManager->saveGridSnapEnabled(isEnabled);
    printf("Grid snap %s\n", isEnabled ? "ON" : "OFF");
}

void MainComponent::gridResolutionChanged(int index)
{
    if (configManager != nullptr)
    {
        configManager->saveGridResolutionIndex(index);
        // FIX 2: also save as milliseconds under key 'gridResolution'
        const double ms = sampleCard.getGridInterval() * 1000.0;
        configManager->saveGridResolutionMs(ms);
        printf("Grid resolution: index=%d  %.3f ms\n", index, ms);
    }
}

void MainComponent::detectedNoteChanged(const juce::String& noteName, double freqHz)
{
    printf("[TUNE] Saving detected note: '%s' %.1f Hz\n", noteName.toRawUTF8(), freqHz);
    saveCurrentSampleState();
}

void MainComponent::transientDetectionEnabledChanged(bool enabled)
{
    if (configManager != nullptr)
        configManager->saveTransientDetectionEnabled(enabled);
    printf("[CRA] Transient detection %s — saved to disk\n", enabled ? "ON" : "OFF");
}

void MainComponent::pitchStepCentsChanged(int cents)
{
    if (configManager != nullptr)
        configManager->savePitchStepCents(cents);
    printf("[PITCH] Step size changed to %d cents\n", cents);
}

void MainComponent::adsrParamsChanged(bool enabled, float attackMs, float decayMs, float sustain, float releaseMs)
{
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    // Propagate to all live sounds atomically — takes effect within one audio block.
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
        {
            sound->customAdsrEnabled.store(enabled);
            sound->customAdsrAttackMs.store(attackMs);
            sound->customAdsrDecayMs.store(decayMs);
            sound->customAdsrSustain.store(sustain);
            sound->customAdsrReleaseMs.store(releaseMs);
        }
    double elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
    if (sampleCard.isAdsrDragging())
    {
        adsrSaveTimer.startTimer(400);  // one disk write fires 400ms after drag stops
        printf("[KNOB-TIMING] ADSR updated atomically: %.0fms — save deferred (dragging)  %s atk=%.0f dcy=%.0f sus=%.2f rel=%.0f\n",
               elapsed, enabled ? "ON" : "OFF", attackMs, decayMs, sustain, releaseMs);
    }
    else
    {
        saveCurrentSampleState();  // toggle or program change — save immediately
        printf("[KNOB-TIMING] ADSR updated atomically: %.0fms — saved immediately  %s atk=%.0f dcy=%.0f sus=%.2f rel=%.0f\n",
               elapsed, enabled ? "ON" : "OFF", attackMs, decayMs, sustain, releaseMs);
    }
    fflush(stdout);
}

void MainComponent::activeTabChanged(int tabIndex)
{
    if (configManager != nullptr)
        configManager->saveActiveTab(tabIndex);
    printf("[TAB] Active tab changed to %d\n", tabIndex);
}

//==============================================================================
// EQ helpers

// OPT 2: Compute biquad coefficients for one band on the UI thread.
// Returns normalized Coeffs; caller assembles all 3 bands and calls eqCoeffDB.writeFromUI().
MainComponent::EqCoeffDoubleBuffer::Coeffs MainComponent::computeEqCoeffs(
    float freqHz, float gainDb, float q, int filterMode, double sampleRate)
{
    // Audio EQ Cookbook formulas — all 6 filter types.
    const double w0    = juce::MathConstants<double>::twoPi * (double)freqHz / sampleRate;
    const double sinW0 = std::sin(w0);
    const double cosW0 = std::cos(w0);
    const double safeQ = (double)juce::jmax(q, 0.01f);
    const double alpha = sinW0 / (2.0 * safeQ);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (filterMode)
    {
        case 0: // Low Cut — 2nd order highpass
            b0 = (1.0 + cosW0) / 2.0; b1 = -(1.0 + cosW0); b2 = (1.0 + cosW0) / 2.0;
            a0 = 1.0 + alpha;          a1 = -2.0 * cosW0;   a2 = 1.0 - alpha;
            break;
        case 1: // Low Shelf
        {
            const double A   = std::pow(10.0, (double)gainDb / 40.0);
            const double sqA = std::sqrt(juce::jmax(A, 0.0001));
            const double arg = (A + 1.0 / A) * (1.0 / safeQ - 1.0) + 2.0;
            const double al  = sinW0 / 2.0 * std::sqrt(juce::jmax(arg, 0.0));
            b0 =   A * ((A+1.0) - (A-1.0)*cosW0 + 2.0*sqA*al);
            b1 = 2.0*A * ((A-1.0) - (A+1.0)*cosW0);
            b2 =   A * ((A+1.0) - (A-1.0)*cosW0 - 2.0*sqA*al);
            a0 =       (A+1.0) + (A-1.0)*cosW0 + 2.0*sqA*al;
            a1 =  -2.0 * ((A-1.0) + (A+1.0)*cosW0);
            a2 =        (A+1.0) + (A-1.0)*cosW0 - 2.0*sqA*al;
            break;
        }
        case 2: // Bell (peaking EQ)
        {
            const double A = std::pow(10.0, (double)gainDb / 40.0);
            b0 = 1.0 + alpha*A; b1 = -2.0*cosW0; b2 = 1.0 - alpha*A;
            a0 = 1.0 + alpha/A; a1 = -2.0*cosW0; a2 = 1.0 - alpha/A;
            break;
        }
        case 3: // Notch
            b0 = 1.0; b1 = -2.0*cosW0; b2 = 1.0;
            a0 = 1.0 + alpha; a1 = -2.0*cosW0; a2 = 1.0 - alpha;
            break;
        case 4: // High Shelf
        {
            const double A   = std::pow(10.0, (double)gainDb / 40.0);
            const double sqA = std::sqrt(juce::jmax(A, 0.0001));
            const double arg = (A + 1.0 / A) * (1.0 / safeQ - 1.0) + 2.0;
            const double al  = sinW0 / 2.0 * std::sqrt(juce::jmax(arg, 0.0));
            b0 =     A * ((A+1.0) + (A-1.0)*cosW0 + 2.0*sqA*al);
            b1 = -2.0*A * ((A-1.0) + (A+1.0)*cosW0);
            b2 =     A * ((A+1.0) + (A-1.0)*cosW0 - 2.0*sqA*al);
            a0 =        (A+1.0) - (A-1.0)*cosW0 + 2.0*sqA*al;
            a1 =   2.0 * ((A-1.0) - (A+1.0)*cosW0);
            a2 =        (A+1.0) - (A-1.0)*cosW0 - 2.0*sqA*al;
            break;
        }
        case 5: // High Cut — 2nd order lowpass
            b0 = (1.0 - cosW0) / 2.0; b1 = 1.0 - cosW0; b2 = (1.0 - cosW0) / 2.0;
            a0 = 1.0 + alpha;          a1 = -2.0*cosW0;  a2 = 1.0 - alpha;
            break;
        default: // Fallback: Bell
        {
            const double A = std::pow(10.0, (double)gainDb / 40.0);
            b0 = 1.0 + alpha*A; b1 = -2.0*cosW0; b2 = 1.0 - alpha*A;
            a0 = 1.0 + alpha/A; a1 = -2.0*cosW0; a2 = 1.0 - alpha/A;
            break;
        }
    }

    if (std::abs(a0) < 1e-30)   // Degenerate — pass-through
        return EqCoeffDoubleBuffer::Coeffs {};  // default: b0=1, rest=0 (pass-through)

    return EqCoeffDoubleBuffer::Coeffs {
        b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
    };
}

// OPT 1: FFT worker thread body — called every 33ms from FftWorkerThread::run().
// All FFT math happens here, completely off the audio thread.
void MainComponent::processFftOnWorkerThread()
{
    if (fft == nullptr) return;
    if (fftAbstractFifo.getNumReady() < kFFTSize) return;

    int start1, size1, start2, size2;
    fftAbstractFifo.prepareToRead(kFFTSize, start1, size1, start2, size2);

    // Build windowed interleaved real/imag frame in scratch buffer.
    int dst = 0;
    for (int i = 0; i < size1; ++i, ++dst)
    {
        fftScratch[dst * 2]     = fftCircularBuffer[start1 + i] * fftWindow[dst];
        fftScratch[dst * 2 + 1] = 0.0f;
    }
    for (int i = 0; i < size2; ++i, ++dst)
    {
        fftScratch[dst * 2]     = fftCircularBuffer[start2 + i] * fftWindow[dst];
        fftScratch[dst * 2 + 1] = 0.0f;
    }
    fftAbstractFifo.finishedRead(size1 + size2);

    fft->performFrequencyOnlyForwardTransform(fftScratch);

    // Write to back buffer with exponential smoothing, then flip.
    const int back = 1 - specFront.load(std::memory_order_relaxed);
    for (int k = 0; k < kSpecBins; ++k)
    {
        const float mag  = fftScratch[k] / static_cast<float>(kFFTSize);
        const float db   = juce::Decibels::gainToDecibels(mag, -100.0f);
        const float prev = specBuffers[back][k];
        specBuffers[back][k] = (db > prev)
            ? kSpecSmoothUp   * db + (1.0f - kSpecSmoothUp)   * prev
            : kSpecSmoothDown * db + (1.0f - kSpecSmoothDown) * prev;
    }
    specFront.store(back, std::memory_order_release);
    hasNewFFTData.store(true, std::memory_order_release);
}

// OPT 4: Returns true only when new FFT data is available — called by EQDisplay timer.
// Clears the hasNewFFTData flag so subsequent calls return false until the next FFT frame.
bool MainComponent::getSpectrumSnapshot(float* dest, int numBins)
{
    if (!hasNewFFTData.load(std::memory_order_acquire))
        return false;

    hasNewFFTData.store(false, std::memory_order_relaxed);
    const int front = specFront.load(std::memory_order_acquire);
    const int count = juce::jmin(numBins, kSpecBins);
    std::memcpy(dest, specBuffers[front], sizeof(float) * count);
    if (numBins > kSpecBins)
        std::memset(dest + kSpecBins, 0, sizeof(float) * (numBins - kSpecBins));
    return true;
}

void MainComponent::eqParamsChanged(bool enabled,
                                     float f1, float g1, float q1,
                                     float f2, float g2, float q2,
                                     float f3, float g3, float q3)
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();

    eqActive.store(enabled);

    // OPT 2: Compute all 3 bands on the UI thread, write to double buffer in one shot.
    const double sr = sampler.getSampleRate() > 0.0 ? sampler.getSampleRate() : 44100.0;
    EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
    newCoeffs[0] = computeEqCoeffs(f1, g1, q1, eqFilterModes[0], sr);
    newCoeffs[1] = computeEqCoeffs(f2, g2, q2, eqFilterModes[1], sr);
    newCoeffs[2] = computeEqCoeffs(f3, g3, q3, eqFilterModes[2], sr);

    const juce::int64 tCoeffs = juce::Time::getMillisecondCounter();
    eqCoeffDB.writeFromUI(newCoeffs);
    const juce::int64 tWrite = juce::Time::getMillisecondCounter();

    // Skip resetEqState() during drag — calling it on every pixel zeroes the filter memory
    // and causes an audible click each time. Only reset on the final mouseUp event or
    // when toggling the EQ on/off (not a drag).
    const bool isDragging = sampleCard.isEqDisplayDragging();
    if (enabled && !isDragging)
        resetEqState();

    // During drag: restart debounced timer — one disk flush fires 400ms after drag stops.
    // When not dragging (EQ toggle, mode change, final mouseUp): save immediately.
    if (isDragging)
    {
        eqSaveTimer.startTimer(400);
    }
    else
    {
        eqSaveTimer.stopTimer();
        saveCurrentSampleState();
    }

    printf("[EQ-TIMING] coeffs computed: %lldms  buffer written: %lldms  dragging=%s  %s  "
           "band1=%.0fHz/%.1fdB/Q%.2f(mode%d)  band2=%.0fHz/%.1fdB/Q%.2f(mode%d)  band3=%.0fHz/%.1fdB/Q%.2f(mode%d)\n",
           (long long)(tCoeffs - t0), (long long)(tWrite - tCoeffs),
           isDragging ? "YES" : "NO",
           enabled ? "ON" : "OFF",
           f1, g1, q1, eqFilterModes[0],
           f2, g2, q2, eqFilterModes[1],
           f3, g3, q3, eqFilterModes[2]);
}

void MainComponent::eqFilterModesChanged(int mode1, int mode2, int mode3)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    printf("[FILTER-TIMING] Filter mode changed: mode1=%d mode2=%d mode3=%d\n", mode1, mode2, mode3);

    eqFilterModes[0] = mode1;
    eqFilterModes[1] = mode2;
    eqFilterModes[2] = mode3;

    // Compute all 3 bands on UI thread, write to double buffer atomically.
    // Audio thread picks up new coefficients on the very next callback — no rebuild, no note cutoff.
    const double sr = sampler.getSampleRate() > 0.0 ? sampler.getSampleRate() : 44100.0;
    EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
    newCoeffs[0] = computeEqCoeffs(sampleCard.getEqBandFreq(0), sampleCard.getEqBandGain(0), sampleCard.getEqBandQ(0), mode1, sr);
    newCoeffs[1] = computeEqCoeffs(sampleCard.getEqBandFreq(1), sampleCard.getEqBandGain(1), sampleCard.getEqBandQ(1), mode2, sr);
    newCoeffs[2] = computeEqCoeffs(sampleCard.getEqBandFreq(2), sampleCard.getEqBandGain(2), sampleCard.getEqBandQ(2), mode3, sr);
    eqCoeffDB.writeFromUI(newCoeffs);

    const auto tCoeffs = juce::Time::getMillisecondCounter();
    printf("[FILTER-TIMING] coefficients recalculated: %lldms\n", (long long)(tCoeffs - t0));

    if (eqActive.load()) resetEqState();

    // NO updateSamplerSounds() — coefficients written atomically above.
    printf("[FILTER-TIMING] updateSamplerSounds called: 0ms — SKIPPED (atomic coefficient swap)\n");

    // Defer disk save — one write fires 400ms after the click.
    filterModeSaveTimer.startTimer(400);

    const auto tDone = juce::Time::getMillisecondCounter();
    printf("[FILTER-TIMING] saveCurrentSampleState called: deferred 400ms\n");
    printf("[FILTER-TIMING] total handler time: %lldms  (save deferred)\n", (long long)(tDone - t0));
    printf("[FILTER-TIMING] filter coefficients updated atomically: 0ms — no rebuild\n");
    fflush(stdout);
}

float MainComponent::computeNormGainFromAudio(float targetDb) const
{
    juce::ScopedLock lock(sampleLock);
    if (selectedSampleIndex < 0 || selectedSampleIndex >= samples.size()) return 1.0f;
    const auto* sample = samples[selectedSampleIndex];
    if (sample == nullptr || !sample->isValid()) return 1.0f;

    const auto& buf = *sample->audioData;
    const int totalSamples = buf.getNumSamples();
    if (totalSamples == 0) return 1.0f;

    const double sr = sample->sampleRate > 0.0 ? sample->sampleRate : 44100.0;
    const int startSamp = (int)(sample->startPointSeconds * sr);
    const int endSamp   = (sample->endPointSeconds > 0.0)
                          ? (int)(sample->endPointSeconds * sr)
                          : totalSamples;
    const int s0 = juce::jlimit(0, totalSamples - 1, startSamp);
    const int s1 = juce::jlimit(s0 + 1, totalSamples, endSamp);

    float peak = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const float* data = buf.getReadPointer(ch);
        for (int i = s0; i < s1; ++i)
            peak = juce::jmax(peak, std::abs(data[i]));
    }

    if (peak <= 1e-7f) return 1.0f;
    const float targetLinear = std::pow(10.0f, targetDb / 20.0f);
    return targetLinear / peak;
}

void MainComponent::normChanged(bool enabled, float targetDb)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    printf("[NORM-TIMING] Normalize target changed: value=%.0fdB  enabled=%s\n", targetDb, enabled ? "YES" : "NO");

    if (!enabled)
    {
        normGain.store(1.0f);
        sampleCard.setNormGainDisplay(0.0f);
        printf("[NORM-TIMING] normGain recalculated: 0ms  (disabled — gain=1.0)\n");
        printf("[NORM-TIMING] updateSamplerSounds called: 0ms — SKIPPED (atomic normGain store)\n");
        normSaveTimer.startTimer(400);
        printf("[NORM-TIMING] saveCurrentSampleState called: deferred 400ms\n");
        printf("[NORM-TIMING] total handler time: %lldms  (save deferred)\n",
               (long long)(juce::Time::getMillisecondCounter() - t0));
        printf("[NORM-TIMING] normGain updated atomically: 0ms — no rebuild\n");
        fflush(stdout);
        return;
    }

    // For enabled: check how many samples need scanning to decide sync vs async.
    int scanSamples = 0;
    {
        juce::ScopedLock lock(sampleLock);
        if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
            if (const auto* s = samples[selectedSampleIndex]; s != nullptr && s->audioData != nullptr)
                scanSamples = s->audioData->getNumSamples();
    }

    constexpr int kBgScanThreshold = 100000;

    if (scanSamples <= kBgScanThreshold || scanSamples == 0)
    {
        // Small sample — scan on message thread, fast enough.
        const float gain   = computeNormGainFromAudio(targetDb);
        normGain.store(gain);
        const float gainDb = (gain > 0.0f) ? 20.0f * std::log10f(gain) : 0.0f;
        sampleCard.setNormGainDisplay(gainDb);

        const auto tGain = juce::Time::getMillisecondCounter();
        printf("[NORM-TIMING] normGain recalculated: %lldms  (sync, %d samples)\n",
               (long long)(tGain - t0), scanSamples);
        printf("[NORM-TIMING] updateSamplerSounds called: 0ms — SKIPPED (atomic normGain store)\n");
        printf("[NORM-TIMING] normGain updated atomically: 0ms — no rebuild  target=%.0fdB  gain=%+.1fdB\n",
               targetDb, gainDb);

        normSaveTimer.startTimer(400);
        const auto tDone = juce::Time::getMillisecondCounter();
        printf("[NORM-TIMING] saveCurrentSampleState called: deferred 400ms\n");
        printf("[NORM-TIMING] total handler time: %lldms  (save deferred)\n", (long long)(tDone - t0));
    }
    else
    {
        // Large sample — show Scanning... label, run peak scan on background thread.
        sampleCard.setNormGainDisplay(0.0f);  // clear stale display while scanning
        printf("[NORM-TIMING] normGain recalculated: async (background scan, %d samples)\n", scanSamples);
        printf("[NORM-TIMING] updateSamplerSounds called: 0ms — SKIPPED\n");

        const float capturedTarget = targetDb;
        backgroundThreads.addJob([this, capturedTarget]()
        {
            const auto tBg = juce::Time::getMillisecondCounter();
            const float gain   = computeNormGainFromAudio(capturedTarget);
            const float gainDb = (gain > 0.0f) ? 20.0f * std::log10f(gain) : 0.0f;
            const auto tScan = juce::Time::getMillisecondCounter();
            printf("[NORM-TIMING] normGain recalculated (bg): %lldms\n", (long long)(tScan - tBg));

            juce::MessageManager::callAsync([this, gain, gainDb]()
            {
                normGain.store(gain);
                sampleCard.setNormGainDisplay(gainDb);
                normSaveTimer.startTimer(400);
                printf("[NORM-TIMING] normGain applied from bg thread  gain=%+.1fdB  save deferred 400ms\n", gainDb);
                fflush(stdout);
            });
        });

        const auto tDone = juce::Time::getMillisecondCounter();
        printf("[NORM-TIMING] total handler time: %lldms  (bg scan dispatched)\n", (long long)(tDone - t0));
    }

    fflush(stdout);
}

void MainComponent::oneShotEnabledChanged(bool enabled)
{
    // Propagate to all live sounds atomically — no rebuild needed.
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            sound->oneShotEnabled.store(enabled);

    // If oneshot is being turned off, cancel any running tail poll.
    if (!enabled && isOneShotTailPlaying)
    {
        isOneShotTailPlaying = false;
        oneShotTailTimer.stopTimer();
        sampleCard.setOneShotTailActive(false);
    }

    if (configManager != nullptr)
        configManager->saveOneShotEnabled(enabled);

    printf("[1SHOT] One Shot %s — saved to disk\n", enabled ? "ON" : "OFF");
}

void MainComponent::reverseEnabledChanged(bool enabled)
{
    // Before updating the sound flag, mirror the current playback position so
    // the audio continues from the same perceptual location but in the new direction.
    // Formula: mirrored = (sEnd - 1) - (currentPos - sStart)
    for (int si = 0; si < sampler.getNumSounds(); ++si)
    {
        if (auto* snd = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(si).get()))
        {
            const juce::int64 sStart = snd->startSampleAtomic.load();
            const juce::int64 sEnd   = snd->endSampleAtomic.load();

            for (int vi = 0; vi < sampler.getNumVoices(); ++vi)
            {
                if (auto* voice = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(vi)))
                {
                    const juce::int64 pos = voice->playheadPositionAtomic.load();
                    if (pos >= sStart && pos < sEnd)
                    {
                        juce::int64 mirrored = (sEnd - 1) - (pos - sStart);
                        mirrored = juce::jlimit(sStart, sEnd - 1, mirrored);
                        voice->pendingSeekAtomic.store(mirrored);
                    }
                }
            }

            snd->reverseEnabled.store(enabled);
        }
    }

    if (configManager != nullptr)
        configManager->saveReverseEnabled(enabled);

    printf("[REV] Reverse playback %s — saved to disk\n", enabled ? "ON" : "OFF");
}

void MainComponent::bounceEnabledChanged(bool enabled)
{
    // Propagate to all live sounds atomically — no rebuild needed.
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            sound->bounceEnabled.store(enabled);

    if (configManager != nullptr)
        configManager->saveBounceEnabled(enabled);

    printf("[BNC] Bounce playback %s — saved to disk\n", enabled ? "ON" : "OFF");
}


void MainComponent::checkOneShotTailDone()
{
    // Called every 200ms while a one-shot tail is playing.
    // Stop polling once no voice is active.
    bool anyActive = false;
    for (int i = 0; i < sampler.getNumVoices(); ++i)
        if (sampler.getVoice(i)->isVoiceActive()) { anyActive = true; break; }

    if (!anyActive)
    {
        isOneShotTailPlaying = false;
        oneShotTailTimer.stopTimer();
        // Tell SampleCard to stop pulsing and restore steady ON state.
        juce::MessageManager::callAsync([this] { sampleCard.setOneShotTailActive(false); });
        printf("[1SHOT] Tail complete — button restored to steady ON\n");
    }
}

//==============================================================================
// Navigation optimizations — deferred save and transient detection

void MainComponent::flushNavigationSave()
{
    // Called once, 500ms after the last Prev/Next press.
    // Snapshot all saveable state on the message thread (fast, memory-only reads),
    // then hand the disk write to a background thread so the message thread never blocks.

    if (configManager == nullptr) return;

    const auto tSnap = (juce::int64)juce::Time::getMillisecondCounter();
    printf("[SAVE-TIMING] flushNavigationSave started — snapshotting state\n");
    fflush(stdout);

    // ── Build snapshot on message thread ─────────────────────────────────────────
    struct SaveSnap
    {
        int          midiNote, midiChannel, pitchOffsetCents;
        juce::String midiDevice;
        float        volume, masterVolume;
        bool         loopEnabled;
        bool         hasSample;
        juce::File   sampleFile;
        double       startPoint, endPoint;
        ConfigurationManager::SampleState sampleState;
    };

    auto snap = std::make_shared<SaveSnap>();
    snap->midiNote         = sampleCard.getMidiNote();
    snap->midiChannel      = sampleCard.getMidiChannel();
    snap->midiDevice       = currentMidiDeviceName;
    snap->pitchOffsetCents = sampleCard.getPitchOffset();
    snap->volume           = sampleCard.getVolume();
    snap->masterVolume     = masterVolumeGain.load();
    snap->loopEnabled      = sampleCard.isLoopEnabled();
    snap->hasSample        = false;

    {
        juce::ScopedLock lock(sampleLock);
        if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
        {
            auto* sample      = samples[selectedSampleIndex];
            snap->hasSample   = true;
            snap->sampleFile  = sample->file;
            snap->startPoint  = sample->startPointSeconds;
            snap->endPoint    = sample->endPointSeconds;

            auto& s = snap->sampleState;
            s.startPoint         = sample->startPointSeconds;
            s.endPoint           = sample->endPointSeconds;
            s.volume             = sampleCard.getVolume();
            s.loopEnabled        = sampleCard.isLoopEnabled();
            s.transientThreshold = sampleCard.getTransientThreshold();
            s.detectedNoteName   = sampleCard.getDetectedNoteName();
            s.detectedFreqHz     = sampleCard.getDetectedFreqHz();
            s.basePitchOffset    = sampleCard.getBasePitchOffset();
            s.adsrEnabled        = sampleCard.isAdsrEnabled();
            s.adsrAttackMs       = sampleCard.getAdsrAttackMs();
            s.adsrDecayMs        = sampleCard.getAdsrDecayMs();
            s.adsrSustain        = sampleCard.getAdsrSustain();
            s.adsrReleaseMs      = sampleCard.getAdsrReleaseMs();
            s.eqEnabled          = sampleCard.isEqEnabled();
            s.eq1Freq  = sampleCard.getEqBandFreq(0); s.eq1Gain = sampleCard.getEqBandGain(0);
            s.eq1Q     = sampleCard.getEqBandQ(0);    s.eq1Mode = eqFilterModes[0];
            s.eq2Freq  = sampleCard.getEqBandFreq(1); s.eq2Gain = sampleCard.getEqBandGain(1);
            s.eq2Q     = sampleCard.getEqBandQ(1);    s.eq2Mode = eqFilterModes[1];
            s.eq3Freq  = sampleCard.getEqBandFreq(2); s.eq3Gain = sampleCard.getEqBandGain(2);
            s.eq3Q     = sampleCard.getEqBandQ(2);    s.eq3Mode = eqFilterModes[2];
        }
    }

    printf("[SAVE-TIMING] snapshot complete in %lldms — background save job started — message thread free\n",
           (juce::int64)juce::Time::getMillisecondCounter() - tSnap);
    fflush(stdout);

    // ── All disk I/O on background thread ────────────────────────────────────────
    backgroundThreads.addJob([this, snap]()
    {
        const auto t0 = (juce::int64)juce::Time::getMillisecondCounter();
        printf("[SAVE-TIMING] background save started\n");
        fflush(stdout);

        configManager->saveMidiSettings(snap->midiNote, snap->midiChannel, snap->midiDevice);
        configManager->savePitchOffset(snap->pitchOffsetCents);
        configManager->saveVolume(snap->volume);
        configManager->saveMasterVolume(snap->masterVolume);
        configManager->saveLoopEnabled(snap->loopEnabled);

        if (snap->hasSample)
        {
            configManager->saveLastSample(snap->sampleFile);
            configManager->saveStartPoint(snap->startPoint);
            configManager->saveEndPoint(snap->endPoint);
            configManager->saveSampleState(snap->sampleFile, snap->sampleState);
        }

        configManager->flush();   // ONE disk write for everything above

        printf("[SAVE-TIMING] background save complete: %lldms\n",
               (juce::int64)juce::Time::getMillisecondCounter() - t0);
        fflush(stdout);
    });
}

void MainComponent::runDeferredTransientDetection()
{
    // Opt 3: Called once, 800ms after the last Prev/Next press.
    // Runs detectTransients() on the message thread (file I/O) for the sample
    // that is now current.  At this point the user has stopped navigating so
    // a brief message-thread stall is acceptable.
    printf("[NAV-TRANSIENT] Deferred transient detection firing\n");
    fflush(stdout);
    sampleCard.runTransientDetection();
}

//==============================================================================
void MainComponent::performPanicReset()
{
    printf("[PANIC] Reset triggered — hard-killing all audio\n");

    // Flash bright red for 200ms then restore dark red
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFFF0000));
    resetButton.repaint();
    juce::Timer::callAfterDelay(200, [this]
    {
        resetButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF8B0000));
        resetButton.repaint();
    });

    // 1. Silence audio thread immediately — it will bail on the next block
    muteOutput.store(true);

    // 2. Clear per-sound states so stopNote() and forceStop() work correctly
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
        {
            s->freezeActive.store(false);
            s->loopEnabled.store(false);
            s->oneShotEnabled.store(false);
        }

    // 3. Force-stop all voices (releases currentlyPlayingSound refcount)
    for (int i = 0; i < sampler.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
            v->forceStop();

    // 4. Belt-and-suspenders: MIDI all-notes-off on every channel
    for (int ch = 1; ch <= 16; ++ch)
        sampler.allNotesOff(ch, false);

    // 5. Stop file browser preview player if open
    if (activePreviewComp != nullptr)
        activePreviewComp->stopPreview();

    // 7. Stop one-shot tail poll and pulse animation
    if (isOneShotTailPlaying)
    {
        isOneShotTailPlaying = false;
        oneShotTailTimer.stopTimer();
        sampleCard.setOneShotTailActive(false);
    }

    // 8. Stop test tone (sine wave)
    if (sineWaveActive)
    {
        sineWaveActive = false;
        testToneButton.setButtonText("Test tone");
    }

    // 9. Re-propagate Loop and OneShot UI states back to sounds so new notes work correctly.
    //    Freeze is intentionally left off — freeze requires an active voice to be meaningful.
    bool loopOn    = sampleCard.isLoopEnabled();
    bool oneShotOn = sampleCard.isOneShotEnabled();
    bool bounceOn  = sampleCard.isBounceEnabled();
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* s = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
        {
            s->loopEnabled.store(loopOn);
            s->oneShotEnabled.store(oneShotOn);
            s->bounceEnabled.store(bounceOn);
        }

    // 10. Re-enable audio — engine is now idle and ready for new MIDI triggers
    muteOutput.store(false);

    printf("[PANIC] Reset complete — audio engine ready\n");
}

void MainComponent::performTrimAsync()
{
    // ── Step 1: capture sample data under lock ──────────────────────────────
    juce::File   sourceFile;
    std::shared_ptr<juce::AudioBuffer<float>> audioData;
    double startSec = 0.0, endSec = 0.0, capSampleRate = 44100.0;
    int    capChannels = 1, numTotalSamples = 0;

    {
        juce::ScopedLock lock(sampleLock);
        if (samples.isEmpty() || selectedSampleIndex < 0 ||
            selectedSampleIndex >= (int)samples.size() ||
            samples[selectedSampleIndex] == nullptr ||
            !samples[selectedSampleIndex]->isValid())
        {
            sampleCard.setTrimInProgress(false);
            return;
        }
        auto* s        = samples[selectedSampleIndex];
        sourceFile     = s->file;
        audioData      = s->audioData;
        startSec       = s->startPointSeconds;
        capSampleRate  = s->sampleRate;
        capChannels    = s->numChannels;
        numTotalSamples = audioData->getNumSamples();
        endSec = (s->endPointSeconds < 0.0)
                 ? (double)numTotalSamples / capSampleRate
                 : s->endPointSeconds;
    }

    const double totalDuration = (double)numTotalSamples / capSampleRate;
    const double duration      = endSec - startSec;

    // ── Step 2: edge-case validation ────────────────────────────────────────
    if (startSec <= 0.001 && endSec >= totalDuration - 0.001)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "No Trim Needed",
            "Start and End markers cover the full sample. Move markers to trim a region.");
        sampleCard.setTrimInProgress(false);
        return;
    }
    if (duration < 0.1)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "Trim Region Too Short",
            "Trim region is too short - minimum 100ms.");
        sampleCard.setTrimInProgress(false);
        return;
    }

    // ── Step 3: confirmation dialog (non-blocking) ──────────────────────────
    const juce::int64 origFileSize  = sourceFile.getSize();
    const double      capStartSec   = startSec;
    const double      capEndSec     = endSec;
    auto              capAudio      = audioData;

    auto* w = new juce::AlertWindow(
        "Trim Sample",
        "Are you sure you want to trim?\n\n"
        "This will save a new file with only the audio between the Start and End markers. "
        "The original file will not be modified.",
        juce::AlertWindow::QuestionIcon);
    w->addButton("Yes, Trim", 1);
    w->addButton("Cancel",    0);

    w->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [this, sourceFile, capAudio, capStartSec, capEndSec,
             capSampleRate, capChannels, origFileSize](int result) mutable
            {
                if (result == 0)
                {
                    sampleCard.setTrimInProgress(false);
                    return;
                }

                // ── Step 4: generate unique output filename ──────────────────
                auto parent   = sourceFile.getParentDirectory();
                auto baseName = sourceFile.getFileNameWithoutExtension();
                juce::File outFile;
                int suffix = 1;
                while (suffix < 1000)
                {
                    outFile = parent.getChildFile(
                        baseName + juce::String::formatted("-%03d", suffix) + ".wav");
                    if (!outFile.existsAsFile()) break;
                    ++suffix;
                }
                if (suffix >= 1000)
                {
                    sampleCard.showTrimToast("Trim failed - could not find a unique filename.", true);
                    sampleCard.setTrimInProgress(false);
                    return;
                }

                // ── Step 5: compute sample range ─────────────────────────────
                const int totalSmp = capAudio->getNumSamples();
                const juce::int64 startSmp = juce::jlimit((juce::int64)0,
                                                           (juce::int64)totalSmp,
                                                           (juce::int64)(capStartSec * capSampleRate));
                const juce::int64 endSmp   = juce::jlimit(startSmp + 1,
                                                           (juce::int64)totalSmp,
                                                           (juce::int64)(capEndSec * capSampleRate));

                printf("[TRIM] Writing '%s' — samples %lld to %lld (%.3f–%.3f s)\n",
                       outFile.getFileName().toRawUTF8(),
                       (long long)startSmp, (long long)endSmp,
                       capStartSec, capEndSec);

                // ── Step 6: write on background thread ───────────────────────
                backgroundThreads.addJob([this, capAudio, startSmp, endSmp,
                                          capSampleRate, capChannels, outFile, origFileSize]() mutable
                {
                    juce::WavAudioFormat wavFormat;
                    auto outStream = std::unique_ptr<juce::FileOutputStream>(
                                         outFile.createOutputStream());
                    if (outStream == nullptr)
                    {
                        juce::MessageManager::callAsync([this] {
                            sampleCard.showTrimToast(
                                "Trim failed - could not write file. Check disk space and permissions.", true);
                            sampleCard.setTrimInProgress(false);
                        });
                        return;
                    }

                    auto* writer = wavFormat.createWriterFor(
                        outStream.get(), capSampleRate,
                        (unsigned int)capChannels, 24, {}, 0);
                    if (writer == nullptr)
                    {
                        juce::MessageManager::callAsync([this] {
                            sampleCard.showTrimToast("Trim failed - could not create WAV writer.", true);
                            sampleCard.setTrimInProgress(false);
                        });
                        return;
                    }
                    outStream.release();  // writer owns the stream now

                    // Write samples in 4096-frame blocks
                    constexpr int kBlock = 4096;
                    juce::int64 pos = startSmp;
                    bool writeOk   = true;
                    while (pos < endSmp && writeOk)
                    {
                        const int n = (int)juce::jmin((juce::int64)kBlock, endSmp - pos);
                        juce::AudioBuffer<float> blk(capChannels, n);
                        for (int ch = 0; ch < capChannels; ++ch)
                            blk.copyFrom(ch, 0, *capAudio, ch, (int)pos, n);
                        writeOk = writer->writeFromAudioSampleBuffer(blk, 0, n);
                        pos += n;
                    }
                    delete writer;  // flushes and closes file

                    // ── Step 7: verify ───────────────────────────────────────
                    if (!writeOk || !outFile.existsAsFile() || outFile.getSize() == 0)
                    {
                        outFile.deleteFile();
                        juce::MessageManager::callAsync([this] {
                            sampleCard.showTrimToast(
                                "Trim failed - could not write file. Check disk space and permissions.", true);
                            sampleCard.setTrimInProgress(false);
                        });
                        return;
                    }

                    // ── Step 8: compute memory-saved string ──────────────────
                    const juce::int64 newSize    = outFile.getSize();
                    const double      savedBytes = (double)(origFileSize - newSize);
                    juce::String savedStr;
                    if (savedBytes > 1024.0 * 1024.0)
                        savedStr = juce::String(savedBytes / (1024.0 * 1024.0), 1) + " MB";
                    else if (savedBytes > 1024.0)
                        savedStr = juce::String(savedBytes / 1024.0, 1) + " KB";
                    else if (savedBytes > 0.0)
                        savedStr = juce::String((int)savedBytes) + " B";

                    printf("[TRIM] Written successfully: %lld bytes  (saved %s)\n",
                           (long long)newSize, savedStr.toRawUTF8());

                    // ── Step 9: load result on message thread ────────────────
                    juce::MessageManager::callAsync([this, outFile, savedStr, savedBytes]() mutable
                    {
                        // Insert into folder navigation list
                        {
                            juce::ScopedWriteLock wlock(folderLock);
                            if (!folderAudioFiles.contains(outFile))
                                folderAudioFiles.add(outFile);
                            for (int i = 0; i < folderAudioFiles.size(); ++i)
                                if (folderAudioFiles[i] == outFile)
                                { currentFileIndex = i; break; }
                        }
                        currentFolder = outFile.getParentDirectory();

                        // Load the trimmed file (reset zoom to full view)
                        sampleCard.restoreZoomAndScroll(1.0, 0.0f);
                        loadSampleFileAsync(outFile, /*autoPlay=*/true, /*resetZoom=*/true);

                        // Show success toast
                        juce::String msg = "Trimmed -> " + outFile.getFileName();
                        if (savedBytes > 0.0)
                            msg += " (saved " + savedStr + ")";
                        sampleCard.showTrimToast(msg, /*isError=*/false);
                        sampleCard.setTrimInProgress(false);
                    });
                });
            }),
        /*deleteWhenDismissed=*/true);
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        performPanicReset();
        return true;  // consumed
    }
    return false;
}

void MainComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.eventComponent == &masterVolumeKnob)
    {
        // JUCE resets the knob value via setDoubleClickReturnValue.
        // Flash the value label briefly white to confirm the reset.
        masterVolValueLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0x88FFFFFF));
        masterVolValueLabel.repaint();
        juce::Timer::callAfterDelay(150, [safeThis = juce::Component::SafePointer<MainComponent>(this)]()
        {
            if (auto* p = safeThis.getComponent())
            {
                p->masterVolValueLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
                p->masterVolValueLabel.repaint();
            }
        });
    }
}

void MainComponent::freezeChanged(bool isFrozen)
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();
    printf("[FREEZE-TIMING] Freeze button clicked — isFrozen=%s\n", isFrozen ? "true" : "false");
    fflush(stdout);

    if (isFrozen)
    {
        // Freeze ON — purely atomic: mark sounds frozen + ensure loop flag is set.
        // No sound rebuild, no disk write, no blocking operations.
        for (int i = 0; i < sampler.getNumSounds(); ++i)
        {
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            {
                sound->freezeActive.store(true);
                sound->loopEnabled.store(true);  // freeze always loops
            }
        }

        // If no voice is currently playing, inject a phantom note to start the loop.
        // With freezeActive=true, stopNote is a no-op so the phantom loop runs until freeze is released.
        bool anyActive = false;
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (sampler.getVoice(i)->isVoiceActive()) { anyActive = true; break; }

        if (!anyActive && !samples.isEmpty() && samples[0]->isValid())
            sampler.noteOn(1, samples[0]->rootNote, 0.8f);

        const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
        printf("[FREEZE-TIMING] Freeze updated atomically: %lldms — no rebuild\n", (long long)elapsed);
        printf("[FREEZE-TIMING] No save needed — freeze does not persist\n");
        fflush(stdout);
    }
    else
    {
        // Freeze OFF — kill frozen voice without a full updateSamplerSounds() rebuild.
        //
        // The old sequence called clearSounds() + updateSamplerSounds() which allocated new
        // LoopingSamplerSound objects and re-added them. This was unnecessary: forceStop()
        // already releases the voice's currentlyPlayingSound refcount, leaving the existing
        // sounds in the sampler intact and ready for new MIDI notes.
        //
        // New sequence: atomic flag clears + forceStop + allNotesOff + loop restore.
        // No heap allocations, no disk writes, all steps complete in microseconds.

        // Step 1: Silence audio thread immediately — non-blocking atomic store.
        muteOutput.store(true);

        // Step 2: Clear freezeActive on all sounds so stopNote() works normally from now on.
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                sound->freezeActive.store(false);

        // Step 3: Force-stop all voices — directly releases currentlyPlayingSound refcount.
        // Safe: muteOutput=true guarantees the audio thread is not inside renderNextBlock.
        for (int i = 0; i < sampler.getNumVoices(); ++i)
            if (auto* v = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
                v->forceStop();

        // Step 4: Belt-and-suspenders JUCE voice-state reset (no tail-off).
        sampler.allNotesOff(0, false);

        // Step 5: Restore loop state on existing sounds (sounds stay in sampler — no rebuild needed).
        const bool loopOn = sampleCard.isLoopEnabled();
        loopEnabled.store(loopOn);
        for (int i = 0; i < sampler.getNumSounds(); ++i)
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
                sound->loopEnabled.store(loopOn);

        // Step 6: Resume audio output.
        muteOutput.store(false);

        const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
        printf("[FREEZE-TIMING] updateSamplerSounds called: 0ms — SKIPPED (forceStop+allNotesOff only)\n");
        printf("[FREEZE-TIMING] Freeze updated atomically: %lldms — no rebuild\n", (long long)elapsed);
        printf("[FREEZE-TIMING] No save needed — freeze does not persist\n");
        printf("[FREEZE-TIMING] total handler time: %lldms\n", (long long)elapsed);
        fflush(stdout);
    }
}

void MainComponent::handleMidiLearn(int noteNumber)
{
    if (isLearningMode)
    {
        // Update the sample card with the learned note
        sampleCard.setMidiNoteFromLearn(noteNumber);
        
        // If there's a selected sample, update its root note and sampler IMMEDIATELY
        if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
        {
            auto* sample = samples[selectedSampleIndex];
            sample->rootNote = noteNumber;
            
            // Also update low/high notes to match for single-note mode
            sample->lowNote = noteNumber;
            sample->highNote = noteNumber;
            
            // Force immediate update of the sampler
            updateSamplerSounds();
            
            printf("Sample %s root note updated to %d via MIDI Learn\n", 
                   sample->name.toRawUTF8(), noteNumber);
        }
        else
        {
            printf("No sample selected, but card note updated to %d\n", noteNumber);
        }
        
        // SAVE THE SESSION after MIDI learn
        saveCurrentSession();
    }
}

//==============================================================================
// Session persistence methods
void MainComponent::loadLastSession()
{
    printf("[PERSIST] ===== App startup: restoring session =====\n");

    // ── Global settings (not per-sample) ──────────────────────────────────────────
    int savedNote    = configManager->getMidiNote();
    int savedChannel = configManager->getMidiChannel();
    juce::String savedDevice = configManager->getMidiDevice();

    printf("[PERSIST] Global: midi_note=%d  midi_channel=%d  device=%s\n",
           savedNote, savedChannel, savedDevice.toRawUTF8());

    // ── Restore global pitch FIRST — before any callbacks that call saveCurrentSession ──
    // setMidiNote/setMidiChannel fire listeners → saveCurrentSession → savePitchOffset.
    // If pitch is still 0 at that point it overwrites the saved value on disk.
    // Setting pitch first ensures every subsequent saveCurrentSession writes the correct value.
    int savedPitch = configManager->getPitchOffset();
    printf("[PITCH] Startup: reading pitch from %s  →  pitch=%+d\n",
           configManager->getSettingsFilePath().toRawUTF8(), savedPitch);
    sampleCard.setPitchOffset(savedPitch);

    sampleCard.setMidiNote(savedNote);
    if (savedChannel >= 0 && savedChannel <= 16)
        sampleCard.setMidiChannel(savedChannel);

    float savedMasterVolume = configManager->getMasterVolume();
    masterVolumeGain.store(savedMasterVolume);
    masterVolumeKnob.setValue(savedMasterVolume, juce::dontSendNotification);
    masterVolValueLabel.setText(juce::String(juce::roundToInt(savedMasterVolume * 100)) + "%",
                                juce::dontSendNotification);
    printf("[PERSIST] Global: master_vol=%.2f\n", savedMasterVolume);

    bool savedGridSnap = configManager->getGridSnapEnabled();
    sampleCard.setGridSnapEnabled(savedGridSnap);
    printf("[PERSIST] Global: grid_snap=%s\n", savedGridSnap ? "ON" : "OFF");

    // Restore base tuning frequency
    double savedTuningHz = configManager->getBaseTuningHz();
    baseTuningLabel.setHz(savedTuningHz);
    sampleCard.setBaseTuningHz(savedTuningHz);
    printf("[TUNING] Restored base tuning: %.1f Hz\n", savedTuningHz);

    // Restore pitch step size
    int savedStepCents = configManager->getPitchStepCents();
    sampleCard.setPitchStepCents(savedStepCents);
    printf("[PITCH] Restored step size: %d cents\n", savedStepCents);

    // Restore active tab
    int savedTab = configManager->getActiveTab();
    sampleCard.setActiveTabQuiet(savedTab);
    printf("[TAB] Restored active tab: %d\n", savedTab);

    // FIX 3: restore transient detection (CRA) on/off state
    bool savedCRA = configManager->getTransientDetectionEnabled();
    sampleCard.setTransientDetectionEnabled(savedCRA);
    printf("[CRA] Restored transient detection: %s\n", savedCRA ? "ON" : "OFF");

    // Restore One Shot on/off state
    bool savedOneShot = configManager->getOneShotEnabled();
    sampleCard.setOneShotEnabled(savedOneShot);
    // Propagate to any live sounds (none at startup, but safe to call)
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            sound->oneShotEnabled.store(savedOneShot);
    printf("[1SHOT] Restored one shot: %s\n", savedOneShot ? "ON" : "OFF");

    // Restore Reverse on/off state
    bool savedReverse = configManager->getReverseEnabled();
    sampleCard.setReverseEnabled(savedReverse);
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            sound->reverseEnabled.store(savedReverse);
    printf("[REV] Restored reverse: %s\n", savedReverse ? "ON" : "OFF");

    // Restore Bounce on/off state
    bool savedBounce = configManager->getBounceEnabled();
    sampleCard.setBounceEnabled(savedBounce);
    for (int i = 0; i < sampler.getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(sampler.getSound(i).get()))
            sound->bounceEnabled.store(savedBounce);
    printf("[BNC] Restored bounce: %s\n", savedBounce ? "ON" : "OFF");

    // FIX 2: restore user-selected grid resolution from 'gridResolution' (ms) key if present
    {
        static const double resVals[] = { 0.001, 0.01, 0.05, 0.1, 0.5, 1.0 };
        double savedMs = configManager->getGridResolutionMs();
        if (savedMs > 0.0)
        {
            // Convert ms back to index (find closest match)
            double savedSec = savedMs / 1000.0;
            int idx = 5; // default 1s
            double bestDiff = 1e9;
            for (int i = 0; i < 6; ++i)
            {
                double diff = std::abs(resVals[i] - savedSec);
                if (diff < bestDiff) { bestDiff = diff; idx = i; }
            }
            sampleCard.setGridResolutionIndex(idx); // also sets userHasSetGridResolution = true
            printf("[PERSIST] Grid resolution restored: %.3f ms (index %d)\n", savedMs, idx);
        }
        else
        {
            // No user preference saved — keep default; auto-select will apply on first load
            printf("[PERSIST] Grid resolution: no user preference saved, auto-select enabled\n");
        }
    }

    // ── Restore MIDI device ────────────────────────────────────────────────────────
    if (savedDevice.isNotEmpty() && isValidMidiDevice(savedDevice))
    {
        currentMidiDeviceName = savedDevice;
        auto devices = juce::MidiInput::getAvailableDevices();
        for (auto& device : devices)
        {
            if (device.name == savedDevice)
            {
                midiInput = juce::MidiInput::openDevice(device.identifier, this);
                if (midiInput != nullptr)
                {
                    midiInput->start();
                    printf("[PERSIST] Restored MIDI device: %s\n", savedDevice.toRawUTF8());
                }
                break;
            }
        }
    }

    // ── Load last sample ──────────────────────────────────────────────────────────
    // Per-sample state (start, end, vol, loop) is looked up inside loadSampleFileAsync.
    // Pitch is NOT restored here — it was already restored above as a global value.
    juce::File lastSample = configManager->getLastSample();
    if (lastSample.existsAsFile() &&
        formatManager.findFormatForFileExtension(lastSample.getFileExtension()) != nullptr)
    {
        // Peek at what the settings file contains for this sample BEFORE any load begins.
        // This lets us verify the save survived the previous session.
        {
            auto peek = configManager->getSampleState(lastSample);
            printf("[ADSR LOAD] On-disk state for '%s' (before load): exists=%s  adsrEnabled=%s  atk=%.0fms  dcy=%.0fms  sus=%.0f%%  rel=%.0fms\n",
                   lastSample.getFileName().toRawUTF8(),
                   peek.exists ? "true" : "false",
                   peek.adsrEnabled ? "true" : "false",
                   peek.adsrAttackMs, peek.adsrDecayMs,
                   peek.adsrSustain * 100.0f, peek.adsrReleaseMs);
            fflush(stdout);
        }
        currentFolder = lastSample.getParentDirectory();
        // Don't scan the folder on startup — only one file needs to load.
        // The folder listing is built lazily on the first Prev/Next press.
        printf("[STARTUP] Loading only saved sample: '%s' — no directory preload\n",
               lastSample.getFileName().toRawUTF8());

        // autoPlay=false → no preview note played on startup
        backgroundThreads.addJob([this, lastSample]() {
            loadSampleFileAsync(lastSample, false);
        });
    }
    else
    {
        printf("[PERSIST] No last sample to restore.\n");
    }

    printf("[PERSIST] ===== Session restore initiated =====\n");
    fflush(stdout);
}

void MainComponent::saveOutgoingSampleState()
{
    if (configManager == nullptr) return;
    juce::ScopedLock lock(sampleLock);
    if (selectedSampleIndex < 0 || selectedSampleIndex >= samples.size()) return;

    auto* sample = samples[selectedSampleIndex];
    ConfigurationManager::SampleState s;
    s.startPoint          = sample->startPointSeconds;
    s.endPoint            = sample->endPointSeconds;
    s.volume              = sampleCard.getVolume();
    s.loopEnabled         = sampleCard.isLoopEnabled();
    s.transientThreshold  = sampleCard.getTransientThreshold();
    s.detectedNoteName    = sampleCard.getDetectedNoteName();
    s.detectedFreqHz      = sampleCard.getDetectedFreqHz();
    s.basePitchOffset     = sampleCard.getBasePitchOffset();
    s.adsrEnabled         = sampleCard.isAdsrEnabled();
    s.adsrAttackMs        = sampleCard.getAdsrAttackMs();
    s.adsrDecayMs         = sampleCard.getAdsrDecayMs();
    s.adsrSustain         = sampleCard.getAdsrSustain();
    s.adsrReleaseMs       = sampleCard.getAdsrReleaseMs();
    s.eqEnabled           = sampleCard.isEqEnabled();
    s.eq1Freq             = sampleCard.getEqBandFreq(0);
    s.eq1Gain             = sampleCard.getEqBandGain(0);
    s.eq1Q                = sampleCard.getEqBandQ(0);
    s.eq1Mode             = eqFilterModes[0];
    s.eq2Freq             = sampleCard.getEqBandFreq(1);
    s.eq2Gain             = sampleCard.getEqBandGain(1);
    s.eq2Q                = sampleCard.getEqBandQ(1);
    s.eq2Mode             = eqFilterModes[1];
    s.eq3Freq             = sampleCard.getEqBandFreq(2);
    s.eq3Gain             = sampleCard.getEqBandGain(2);
    s.eq3Q                = sampleCard.getEqBandQ(2);
    s.eq3Mode             = eqFilterModes[2];
    s.normEnabled         = sampleCard.isNormEnabled();
    s.normTargetDb        = sampleCard.getNormTargetDb();
    // User pitchOffset is NOT saved here — it is a global value saved via savePitchOffset()

    configManager->saveSampleState(sample->file, s);
    printf("[PERSIST] SAVED (outgoing): %s  start=%.3f  end=%.3f  vol=%.2f  loop=%s  note=%s\n",
           sample->file.getFileName().toRawUTF8(),
           s.startPoint, s.endPoint, s.volume,
           s.loopEnabled ? "ON" : "OFF",
           s.detectedNoteName.isEmpty() ? "-" : s.detectedNoteName.toRawUTF8());
    fflush(stdout);
}

void MainComponent::saveCurrentSampleState()
{
    printf("[ADSR-DBG] saveCurrentSampleState() called  (adsr_en=%s atk=%.0f dcy=%.0f sus=%.2f rel=%.0f)\n",
           sampleCard.isAdsrEnabled() ? "true" : "false",
           sampleCard.getAdsrAttackMs(), sampleCard.getAdsrDecayMs(),
           sampleCard.getAdsrSustain(),  sampleCard.getAdsrReleaseMs());

    if (configManager == nullptr) { printf("[ADSR-DBG]   → skipped: configManager null\n"); return; }
    juce::ScopedLock lock(sampleLock);
    if (selectedSampleIndex < 0 || selectedSampleIndex >= samples.size())
    {
        printf("[ADSR-DBG]   → skipped: no valid sample (idx=%d size=%d)\n",
               selectedSampleIndex, samples.size());
        return;
    }

    auto* sample = samples[selectedSampleIndex];
    ConfigurationManager::SampleState s;
    s.startPoint          = sample->startPointSeconds;
    s.endPoint            = sample->endPointSeconds;
    s.volume              = sampleCard.getVolume();
    s.loopEnabled         = sampleCard.isLoopEnabled();
    s.transientThreshold  = sampleCard.getTransientThreshold();
    s.detectedNoteName    = sampleCard.getDetectedNoteName();
    s.detectedFreqHz      = sampleCard.getDetectedFreqHz();
    s.basePitchOffset     = sampleCard.getBasePitchOffset();
    s.adsrEnabled         = sampleCard.isAdsrEnabled();
    s.adsrAttackMs        = sampleCard.getAdsrAttackMs();
    s.adsrDecayMs         = sampleCard.getAdsrDecayMs();
    s.adsrSustain         = sampleCard.getAdsrSustain();
    s.adsrReleaseMs       = sampleCard.getAdsrReleaseMs();
    s.eqEnabled           = sampleCard.isEqEnabled();
    s.eq1Freq             = sampleCard.getEqBandFreq(0);
    s.eq1Gain             = sampleCard.getEqBandGain(0);
    s.eq1Q                = sampleCard.getEqBandQ(0);
    s.eq1Mode             = eqFilterModes[0];
    s.eq2Freq             = sampleCard.getEqBandFreq(1);
    s.eq2Gain             = sampleCard.getEqBandGain(1);
    s.eq2Q                = sampleCard.getEqBandQ(1);
    s.eq2Mode             = eqFilterModes[1];
    s.eq3Freq             = sampleCard.getEqBandFreq(2);
    s.eq3Gain             = sampleCard.getEqBandGain(2);
    s.eq3Q                = sampleCard.getEqBandQ(2);
    s.eq3Mode             = eqFilterModes[2];
    s.normEnabled         = sampleCard.isNormEnabled();
    s.normTargetDb        = sampleCard.getNormTargetDb();
    // User pitchOffset is NOT saved here — it is a global value saved via savePitchOffset()

    configManager->saveSampleState(sample->file, s);
    configManager->flush();   // ONE disk write for the entire sample state batch
    printf("[ADSR SAVE] adsrEnabled=%s  atk=%.0fms  dcy=%.0fms  sus=%.0f%%  rel=%.0fms  →  %s\n",
           s.adsrEnabled ? "true" : "false",
           s.adsrAttackMs, s.adsrDecayMs, s.adsrSustain * 100.0f, s.adsrReleaseMs,
           configManager->getSettingsFilePath().toRawUTF8());
    printf("[PERSIST] SAVED: %s  start=%.3f  end=%.3f  vol=%.2f  loop=%s  note=%s\n",
           sample->file.getFileName().toRawUTF8(),
           s.startPoint, s.endPoint, s.volume,
           s.loopEnabled ? "ON" : "OFF",
           s.detectedNoteName.isEmpty() ? "-" : s.detectedNoteName.toRawUTF8());
    fflush(stdout);
}

void MainComponent::saveCurrentSession()
{
    // Always save MIDI settings (in-memory only — flush at end)
    configManager->saveMidiSettings(
        sampleCard.getMidiNote(),
        sampleCard.getMidiChannel(),
        currentMidiDeviceName
    );

    configManager->savePitchOffset(sampleCard.getPitchOffset());
    configManager->saveVolume(sampleCard.getVolume());
    configManager->saveLoopEnabled(sampleCard.isLoopEnabled());
    // NOTE: saveAudioSettings() intentionally NOT called here — audio device settings
    // never change during normal session events. They are saved only in audioDeviceChanged().

    if (selectedSampleIndex >= 0 && selectedSampleIndex < samples.size())
    {
        auto* sample = samples[selectedSampleIndex];
        configManager->saveLastSample(sample->file);
        configManager->saveStartPoint(sample->startPointSeconds);
        configManager->saveEndPoint(sample->endPointSeconds);
        // saveCurrentSampleState() does the per-sample write AND the single flush.
        saveCurrentSampleState();
    }
    else
    {
        configManager->flush();   // flush global keys even with no sample loaded
        printf("Session saved (no sample) → %s\n", configManager->getSettingsFilePath().toRawUTF8());
    }
}

bool MainComponent::isValidMidiDevice(const juce::String& deviceName)
{
    auto devices = juce::MidiInput::getAvailableDevices();
    for (auto& device : devices)
    {
        if (device.name == deviceName)
            return true;
    }
    return false;
}

void MainComponent::midiDeviceChanged(const juce::String& newDevice)
{
    currentMidiDeviceName = newDevice;
    saveCurrentSession();
}



