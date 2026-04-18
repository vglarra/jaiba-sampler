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
    
    setSize(900, 815);

    // ── Pad grid + global controls (Part 1 — visual only) ──
    addAndMakeVisible (globalControlsBar);
    addAndMakeVisible (padGrid);

    padGrid.onPadTriggered = [this] (int padIndex)
    {
        if (!padManager.hasEngine (padIndex)) return;
        auto& engine = padManager.getEngine (padIndex);

        // Read the root note assigned to this pad under the sample lock
        int note = 60; // default if no sample loaded
        {
            juce::ScopedLock sl (engine.sampleLock);
            if (engine.samples.isEmpty()) return;
            note = engine.samples[0]->rootNote;
        }

        // Trigger note on — full velocity
        engine.getSynthesiser().noteOn (1, note, 1.0f);

        // Schedule note off after 500ms
        juce::Timer::callAfterDelay (500, [this, padIndex, note]()
        {
            if (!padManager.hasEngine (padIndex)) return;
            padManager.getEngine (padIndex).getSynthesiser().noteOff (1, note, 0.0f, true);
        });
    };
    
    formatManager.registerBasicFormats();

    // PadAudioEngine ctor already adds 16 LoopingSamplerVoice instances and enables note stealing.

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
        for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
        padManager.setMasterVolume(v);
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
          for (int i = 0; i < pad().getSynthesiser().getNumVoices(); ++i)
              if (auto* v = dynamic_cast<LoopingSamplerVoice*>(pad().getSynthesiser().getVoice(i)))
                  if (v->playheadPositionAtomic.load() >= 0)
                      return v->playDirectionAtomic.load();
          return 1;
      };
      sampleCard.onTrimRequested = [this] { performTrimAsync(); };

      // FIX 2: Fast path for EQ Reset — write pre-computed flat coefficients atomically.
      // No coefficient math, no resetEqState() data race, no synchronous disk write.
      sampleCard.onEqReset = [this]
      {
          const juce::int64 t0 = juce::Time::getMillisecondCounter();

          // FIX 2: Copy pre-computed flat defaults directly to double buffer — nanoseconds.
          pad().eqCoeffDB.writeFromUI(pad().defaultFlatCoeffs);
          // Stamp the request time so the audio thread can print round-trip latency.
          pad().eqResetRequestedMs.store(t0, std::memory_order_relaxed);

          const juce::int64 tWrite = juce::Time::getMillisecondCounter();
          printf("[RESET-TIMING] coefficients written atomically: %lldms — no calculation\n",
                 (long long)(tWrite - t0));
          // FIX 3: No updateSamplerSounds.
          printf("[RESET-TIMING] updateSamplerSounds called: 0ms — SKIPPED (atomic coefficient swap)\n");

          // FIX 4: Defer save — no synchronous disk write on Reset.
          eqSaveTimer.startTimer(400);
          const juce::int64 tSave = juce::Time::getMillisecondCounter();
          printf("[RESET-TIMING] save deferred: 400ms timer started at %lldms\n",
                 (long long)(tSave - t0));
          printf("[RESET-TIMING] total MainComponent reset handler: %lldms\n",
                 (long long)(tSave - t0));
          fflush(stdout);
      };

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
    midiSaveTimer.stopTimer();
    deviceManager.removeChangeListener(this);

    // Kill any active freeze/loop voices before audio shutdown.
    // PadAudioEngine owns the FFT worker thread — it will be stopped in its destructor.
    pad().muteOutput.store(true);
    pad().clearActiveSoundFlags();
    pad().forceStopAllVoices();
    pad().clearSoundsAndVoices();
    pad().allNotesOff(0, false);

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
    midiCollector.reset(sampleRate);

    // Delegate all audio engine initialization (FFT, EQ, voices) to PadManager.
    padManager.prepareToPlay(samplesPerBlockExpected, sampleRate);
    printf("[EQ-RESET] Default flat coefficients pre-computed at SR=%.0f\n", sampleRate);

    // Inform EQDisplay of the current sample rate so biquad response rendering is correct.
    sampleCard.setEqSampleRate(sampleRate);

    // OPT 4: Wire spectrum callback — returns true only when new FFT data is ready.
    sampleCard.setEqSpectrumCallback([this](float* dest, int numBins) -> bool {
        return pad().getSpectrumSnapshot(dest, numBins);
    });

    // ── Latency report ──────────────────────────────────────────────────────────
    const double bufMs = (double)samplesPerBlockExpected / sampleRate * 1000.0;
    const juce::String driverType = (deviceManager.getCurrentAudioDevice() != nullptr)
        ? deviceManager.getCurrentAudioDevice()->getTypeName() : "Unknown";
    printf("\n[LATENCY-REPORT] ════════════════════════════════════\n");
    printf("[LATENCY-REPORT] Buffer size  : %d samples = %.2f ms\n", samplesPerBlockExpected, bufMs);
    printf("[LATENCY-REPORT] Sample rate  : %.0f Hz\n", sampleRate);
    printf("[LATENCY-REPORT] Driver type  : %s\n", driverType.toRawUTF8());
    printf("[LATENCY-REPORT] MIDI→noteOn  : < 1 us (lock-free FIFO)\n");
    printf("[LATENCY-REPORT] noteOn→audio : 0 – %.2f ms (within same block)\n", bufMs);
    printf("[LATENCY-REPORT] Target total : %.2f ms  (add output device latency)\n", bufMs);
    if (driverType.containsIgnoreCase("ASIO"))
        printf("[LATENCY-REPORT] ASIO detected — optimal low-latency path active\n");
    else
        printf("[LATENCY-REPORT] TIP: For sub-3ms latency use an ASIO driver (e.g. ASIO4ALL)\n");
    printf("[LATENCY-REPORT] ════════════════════════════════════\n\n");
    fflush(stdout);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // muteOutput is set true by the message thread during sample-change to guarantee a
    // silent, zeroed buffer while the sampler is being rebuilt.  Checked atomically so
    // the audio thread sees it within one block (~6 ms) with no locks required.
    if (pad().muteOutput.load())
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    bufferToFill.clearActiveBufferRegion();

    // ── Block budget monitor ─────────────────────────────────────────────────────
    // Measure total time spent in this callback. Anything over half the buffer duration
    // risks an underrun.  Budget = bufferSize / sampleRate * 1e6 us.
    const juce::int64 blockStartTicks = juce::Time::getHighResolutionTicks();

    juce::MidiBuffer midiMessages;
    juce::MidiBuffer incomingMidi;
    midiCollector.removeNextBlockOfMessages(incomingMidi, bufferToFill.numSamples);
    midiMessages.addEvents(incomingMidi, 0, bufferToFill.numSamples, 0);

    // ── MIDI-to-audio latency measurement ────────────────────────────────────────
    // Store measured latency in an atomic — CPU timer on message thread prints it.
    // No printf here: printf+fflush in the audio thread adds ~1ms latency on Windows.
    {
        const juce::int64 noteTick = pad().midiNoteOnTicks.exchange(0, std::memory_order_relaxed);
        if (noteTick != 0)
        {
            const double ticksPerUs = juce::Time::getHighResolutionTicksPerSecond() / 1.0e6;
            const juce::int64 now   = juce::Time::getHighResolutionTicks();
            pad().lastMidiLatencyUs.store((juce::int64)((double)(now - noteTick) / ticksPerUs),
                                    std::memory_order_relaxed);
        }
    }

    // Delegate all per-pad audio rendering + EQ + FFT + normGain + volumeGain to PadManager.
    // PadManager also applies master volume after mixing all pads.
    padManager.renderNextBlock(*bufferToFill.buffer, midiMessages, 0, bufferToFill.numSamples);

    if (sineWaveActive)
    {
        const double sampleRate = pad().getSampleRate();
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

    // Master volume is applied by PadManager::renderNextBlock — do NOT apply it again here.

    // ── Block budget monitor ─────────────────────────────────────────────────────
    // Print a warning if this block took more than 50% of its time budget.
    // Only evaluated every 512 blocks (~3s @ 44.1kHz/256) to avoid print overhead.
    {
        static int blockCounter = 0;
        if (++blockCounter >= 512)
        {
            blockCounter = 0;
            const juce::int64 blockEndTicks = juce::Time::getHighResolutionTicks();
            const double ticksPerUs  = juce::Time::getHighResolutionTicksPerSecond() / 1.0e6;
            const double elapsedUs   = (double)(blockEndTicks - blockStartTicks) / ticksPerUs;
            const double budgetUs    = (double)bufferToFill.numSamples / pad().getSampleRate() * 1.0e6;
            if (elapsedUs > budgetUs * 0.5)
            {
                // Note: printf here is a last-resort diagnostic — it only fires when
                // the block is already overrunning. fflush intentionally omitted.
                printf("[AUDIO-PERF] Block took %.0f us — budget %.0f us (%.0f%%) — OVERRUN WARNING\n",
                       elapsedUs, budgetUs, 100.0 * elapsedUs / budgetUs);
            }
        }
    }
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

    // Line above footer (footer is 50px from bottom)
    g.setColour(juce::Colour(0xFF404040));
    auto footerY = getHeight() - 55;
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
    // Guard against recursive resizing
    static bool isResizing = false;
    if (isResizing) return;
    isResizing = true;

    constexpr int kTopBarH   = 40;
    constexpr int kGlobCtrlH = 40;
    constexpr int kPadRowH   = 120;
    constexpr int kCardH     = 410;
    constexpr int kCardMaxW  = 720;
    constexpr int kFooterH   = 50;
    constexpr int kHMargin   = 20;
    constexpr int kGap       = 10;

    // Consume from top (full width — no horizontal margins yet)
    auto strip = getLocalBounds();

    // =========================================================
    // TOP BAR  (40 px, with horizontal margins)
    // =========================================================
    {
        auto topBar = strip.removeFromTop (kTopBarH).reduced (kHMargin, 0);

        menuButton.setBounds (topBar.removeFromLeft (60).withSizeKeepingCentre (56, 30));

        // Right side: Reset+Hz+MasterVol+knob+val%+MIDI+TestTone = 358 px
        constexpr int kRightW = 52+6+70+6+62+4+28+4+32+4+22+6+62; // 358
        auto rightSide = topBar.removeFromRight (kRightW);
        resetButton.setBounds       (rightSide.removeFromLeft (52).withSizeKeepingCentre (48, 30));
        rightSide.removeFromLeft (6);
        baseTuningLabel.setBounds   (rightSide.removeFromLeft (70).withSizeKeepingCentre (68, 24));
        rightSide.removeFromLeft (6);
        masterVolumeLabel.setBounds (rightSide.removeFromLeft (62).withSizeKeepingCentre (62, 16));
        rightSide.removeFromLeft (4);
        masterVolumeKnob.setBounds  (rightSide.removeFromLeft (28).withSizeKeepingCentre (28, 28));
        rightSide.removeFromLeft (4);
        masterVolValueLabel.setBounds (rightSide.removeFromLeft (32).withSizeKeepingCentre (30, 16));
        rightSide.removeFromLeft (4);
        midiActivityLight.setBounds (rightSide.removeFromLeft (22).withSizeKeepingCentre (20, 20));
        rightSide.removeFromLeft (6);
        testToneButton.setBounds    (rightSide.removeFromLeft (62).withSizeKeepingCentre (58, 30));
    }

    // =========================================================
    // GLOBAL CONTROLS BAR  (40 px, full width)
    // =========================================================
    globalControlsBar.setBounds (strip.removeFromTop (kGlobCtrlH));

    // =========================================================
    // TOP TRIANGLE PAD ROW  (kPadRowH, full width)
    // =========================================================
    const int topPadRowY = strip.getY();
    strip.removeFromTop (kPadRowH);

    strip.removeFromTop (kGap);

    // =========================================================
    // SAMPLE CARD  (kCardH, centred)
    // =========================================================
    {
        auto cardStrip = strip.removeFromTop (kCardH);
        int  cardW     = juce::jmin (getWidth() - kHMargin * 2 - 20, kCardMaxW);
        if (cardW < 100) cardW = 100;
        auto cardBounds = juce::Rectangle<int> (0, 0, cardW, kCardH)
                              .withCentre (cardStrip.getCentre());
        sampleCard.setBounds (cardBounds);
    }

    strip.removeFromTop (kGap);

    // =========================================================
    // BOTTOM TRIANGLE PAD ROW  (kPadRowH, full width)
    // =========================================================
    const int bottomPadRowBottom = strip.getY() + kPadRowH;
    strip.removeFromTop (kPadRowH);

    // =========================================================
    // FOOTER  (kFooterH, anchored to bottom, with horizontal margins)
    // =========================================================
    {
        auto footerArea = getLocalBounds().removeFromBottom (kFooterH).reduced (kHMargin, 0);

        auto leftFooter = footerArea.withTrimmedRight (130);
        constexpr int kRowH2       = 20;
        const int     vertPadding  = (footerArea.getHeight() - kRowH2 * 2) / 2;

        auto audioRow = leftFooter.removeFromTop (kRowH2).translated (0, vertPadding);
        audioRow.removeFromLeft (5);
        audioDeviceInfoLabel.setBounds (audioRow);
        audioDeviceInfoLabel.setFont (juce::Font (10.0f));
        audioDeviceInfoLabel.setJustificationType (juce::Justification::left);

        auto midiRow = leftFooter.removeFromTop (kRowH2).translated (0, vertPadding);
        midiRow.removeFromLeft (5);
        midiDeviceInfoLabel.setBounds (midiRow);
        midiDeviceInfoLabel.setFont (juce::Font (10.0f));
        midiDeviceInfoLabel.setJustificationType (juce::Justification::left);

        cpuUsageLabel.setBounds (footerArea.removeFromRight (120).reduced (5));
        cpuUsageLabel.setFont (juce::Font (11.0f, juce::Font::bold));
        cpuUsageLabel.setJustificationType (juce::Justification::right);
    }

    // =========================================================
    // PAD GRID  — transparent component, spans both pad rows
    //             (middle section passes mouse events through)
    // =========================================================
    {
        const int padGridTop = topPadRowY;
        const int padGridH   = bottomPadRowBottom - padGridTop;
        padGrid.setRowHeight (kPadRowH);
        padGrid.setBounds    (0, padGridTop, getWidth(), padGridH);
    }

    // =========================================================
    // Layout diagnostics
    // =========================================================
    static bool printed = false;
    if (!printed)
    {
        printed = true;
        printf ("[GRID-LAYOUT] App window: %dx%d\n", getWidth(), getHeight());
        printf ("[GRID-LAYOUT] Top bar: height=%dpx\n", kTopBarH);
        printf ("[GRID-LAYOUT] Global controls bar: height=%dpx\n", kGlobCtrlH);
        printf ("[GRID-LAYOUT] Top pad row: height=%dpx -- 8 pads each %dpx wide\n",
                kPadRowH, getWidth() / 8);
        printf ("[GRID-LAYOUT] SampleCard: height=%dpx\n", kCardH);
        printf ("[GRID-LAYOUT] Bottom pad row: height=%dpx -- 8 pads each %dpx wide\n",
                kPadRowH, getWidth() / 8);
        printf ("[GRID-LAYOUT] Footer: height=%dpx\n", kFooterH);
        const int total = kTopBarH + kGlobCtrlH + kPadRowH + kGap + kCardH + kGap + kPadRowH + kFooterH;
        printf ("[GRID-LAYOUT] Total: %dpx -- fits window: %s\n",
                total, total <= getHeight() ? "YES" : "NO");
    }

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
                        juce::ScopedLock lock(pad().sampleLock);
                        if (!pad().samples.isEmpty())
                            pad().samples[0]->pitchOffset = 0;
                    }
                    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
                        if (auto* snd = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
    // ── Deferred diagnostic prints (written by audio thread, printed here on message thread) ─
    {
        const juce::int64 latUs = pad().lastMidiLatencyUs.exchange(-1, std::memory_order_relaxed);
        if (latUs >= 0)
        {
            const double bufMs = (pad().getSampleRate() > 0)
                ? (double)deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples()
                  / pad().getSampleRate() * 1000.0
                : 0.0;
            printf("[LATENCY] MIDI noteOn to audio block: %lld us  (buffer ~%.1f ms)\n",
                   (long long)latUs, bufMs);
            fflush(stdout);
        }
    }
    {
        const juce::int64 eqMs = pad().eqResetElapsedMs.exchange(-1, std::memory_order_relaxed);
        if (eqMs >= 0)
        {
            printf("[RESET-TIMING] Audio thread applied new EQ coefficients after: %lldms\n",
                   (long long)eqMs);
            fflush(stdout);
        }
    }

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
        juce::ScopedLock lock(pad().sampleLock);
        pad().samples.clear();
        pad().selectedSampleIndex = 0;
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
            juce::ScopedLock lock(pad().sampleLock);
            pad().samples.add(sample);
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


void MainComponent::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    // Drop high-frequency background messages immediately — zero work, zero latency.
    if (message.isMidiClock() || message.isActiveSense())
        return;

    // Trigger MIDI activity LED — writes an atomic flag + schedules async repaint.
    // Safe to call from any thread. No blocking I/O.
    midiActivityLight.triggerActivity();

    // Stamp high-resolution tick for MIDI-to-audio latency measurement.
    // Audio thread reads this in getNextAudioBlock() and prints the delta once.
    if (message.isNoteOn())
        pad().midiNoteOnTicks.store(juce::Time::getHighResolutionTicks(), std::memory_order_relaxed);

    // MIDI Learn: intercept note-on before channel filter.
    // IMPORTANT: dispatch to message thread — handleMidiLearn must NOT run on the MIDI
    // callback thread (it calls updateSamplerSounds which acquires the synthesizer lock,
    // and saveCurrentSampleState which flushes disk I/O — both would block here for seconds).
    if (isLearningMode && message.isNoteOn())
    {
        const int learnedNote = message.getNoteNumber();
        midiCollector.addMessageToQueue(message);
        juce::MessageManager::callAsync([this, learnedNote] { handleMidiLearn(learnedNote); });
        return;
    }

    // Channel filter: drop messages on wrong channel (0 = any).
    const int selectedChannel = sampleCard.getMidiChannel();
    if (selectedChannel != 0 && message.getChannel() != selectedChannel)
        return;

    // Add to lock-free FIFO — audio thread drains this each block.
    // This is the ONLY operation that affects audio latency in this path.
    midiCollector.addMessageToQueue(message);

    // One-shot tail detection: note-off ignored by audio engine while 1Shot is ON.
    // Timer and UI updates must run on the message thread — use callAsync.
    if (message.isNoteOff() && sampleCard.isOneShotEnabled() && !isOneShotTailPlaying)
    {
        isOneShotTailPlaying = true;
        juce::MessageManager::callAsync([this]
        {
            sampleCard.setOneShotTailActive(true);
            oneShotTailTimer.startTimer(200);  // MUST be called from message thread
        });
    }
}


void MainComponent::sliderValueChanged(juce::Slider* slider)
{
    if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
    {
        auto* sample = pad().samples[pad().selectedSampleIndex];

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
    if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
    {
        auto* sample = pad().samples[pad().selectedSampleIndex];

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
    if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
    {
        juce::String sampleName = pad().samples[pad().selectedSampleIndex]->name;
        pad().samples.remove(pad().selectedSampleIndex);
        pad().selectedSampleIndex = -1;
        updateSamplerSounds();
        sampleListBox.updateContent();
        updateMappingUI();

        printf("Sample '%s' removed. Total samples: %d\n",
               sampleName.toRawUTF8(), pad().samples.size());
        fflush(stdout);
    }
}

void MainComponent::clearAllSamples()
{
    pad().samples.clear();
    pad().selectedSampleIndex = -1;
    pad().getSynthesiser().clearSounds();
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
    pad().getSynthesiser().clearSounds();

    for (auto* sample : pad().samples)
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
        sound->loopEnabled.store(pad().loopEnabled.load());
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

        pad().getSynthesiser().addSound(sound);

        printf("Added sound: %s -> note %d, pitchOffset %+d\n",
               sample->name.toRawUTF8(), sample->rootNote, sample->pitchOffset);
    }

    printf("Sampler updated with %d sounds (each mapped to single note)\n", pad().samples.size());
    fflush(stdout);
}

//==============================================================================
// New UI functionality implementations
void MainComponent::showSettingsMenu()
{
    // Buffer size submenu — shows current size and lets user pick from common values.
    // Smaller = lower latency but higher CPU load risk.
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup(setup);
    const int currentBuf = setup.bufferSize;

    juce::PopupMenu bufferMenu;
    const int bufSizes[] = { 64, 128, 256, 512 };
    // IDs 10–13 reserved for buffer sizes
    for (int i = 0; i < 4; ++i)
    {
        const int sz    = bufSizes[i];
        const double ms = (double)sz / (setup.sampleRate > 0 ? setup.sampleRate : 44100.0) * 1000.0;
        juce::String label = juce::String(sz) + " samples  (" + juce::String(ms, 1) + " ms)";
        if (sz == 64)  label += "  [very tight — may drop out]";
        if (sz == 128) label += "  [recommended]";
        if (sz == currentBuf) label = "* " + label;  // mark active size
        bufferMenu.addItem(10 + i, label);
    }

    juce::PopupMenu menu;
    menu.addItem(1, "Audio Settings");
    menu.addItem(2, "MIDI Settings");
    menu.addSeparator();
    menu.addSubMenu("Buffer Size", bufferMenu);

    menu.showMenuAsync(juce::PopupMenu::Options()
                       .withTargetComponent(&menuButton)
                       .withMinimumWidth(200),
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
                           else if (result >= 10 && result <= 13)
                           {
                               const int newBufSizes[] = { 64, 128, 256, 512 };
                               const int newSize = newBufSizes[result - 10];
                               juce::AudioDeviceManager::AudioDeviceSetup s;
                               deviceManager.getAudioDeviceSetup(s);
                               if (s.bufferSize != newSize)
                               {
                                   s.bufferSize = newSize;
                                   juce::String err = deviceManager.setAudioDeviceSetup(s, true);
                                   if (err.isEmpty())
                                   {
                                       saveAudioSettings();
                                       const double ms = (double)newSize / (s.sampleRate > 0 ? s.sampleRate : 44100.0) * 1000.0;
                                       printf("[LATENCY-REPORT] Buffer size changed to %d samples = %.1f ms\n",
                                              newSize, ms);
                                       fflush(stdout);
                                   }
                                   else
                                   {
                                       printf("[LATENCY-REPORT] Buffer size change failed: %s\n",
                                              err.toRawUTF8());
                                       fflush(stdout);
                                   }
                               }
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
                juce::ScopedLock lock(pad().sampleLock);
                if (!pad().samples.isEmpty())
                    currentFile = pad().samples[0]->file;
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

void MainComponent::loadSampleFileAsync(const juce::File& file, bool autoPlay, bool resetZoom, bool deferTransients, TrimSettingsSnapshot trimSnapshot)
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
                                      tBgStart, trimSnapshot]() mutable {

        const juce::int64 tMsgStart = (juce::int64)juce::Time::getMillisecondCounter();
        printf("[LOAD-TIMING] callAsync lambda started: %lldms after bg job\n",
               tMsgStart - tBgStart);
        fflush(stdout);

        // ── Step 2: Stop all audio completely ─────────────────────────────────────
        pad().muteOutput.store(true);   // audio thread bails immediately

        // Stop any one-shot tail poll (new sample cancels the old tail)
        if (isOneShotTailPlaying)
        {
            isOneShotTailPlaying = false;
            oneShotTailTimer.stopTimer();
            sampleCard.setOneShotTailActive(false);
        }

        // Clear freeze/loop/oneshot flags so stopNote() fires correctly
        pad().clearActiveSoundFlags();
        // Force-stop every voice: releases currentlyPlayingSound ref-count directly
        pad().forceStopAllVoices();
        pad().clearSoundsAndVoices();
        pad().allNotesOff(0, false);

        // ── Step 3: Reset card to neutral state ───────────────────────────────────
        sampleCard.resetStartPoint();
        sampleCard.resetEndPoint();
        sampleCard.setLoopEnabled(false);
        sampleCard.resetFreeze();
        sampleCard.resetBounce();

        // ── Step 4: Install new sample ────────────────────────────────────────────
        {
            juce::ScopedLock lock(pad().sampleLock);
            pad().samples.clear();
            pad().selectedSampleIndex = 0;
            pad().samples.add(sample);
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

        // ── Steps 5-7: Restore per-sample state ──────────────────────────────────
        // Start/end always reset to 0 / full-length (trim changes sample length).
        // For trim loads, all other settings come from the TrimSettingsSnapshot.
        // For normal loads, settings come from ConfigurationManager (per-sample key).
        double effectiveStart = 0.0;
        double effectiveEnd   = -1.0;   // -1 = full sample length
        float  effectiveVol   = 1.0f;
        bool   effectiveLoop  = false;

        printf("[PERSIST] --- Loading: %s ---\n", file.getFileName().toRawUTF8());

        // Helper lambda — applies ADSR/EQ/Norm state from fields (avoids code duplication)
        auto applyAdsrState = [&](bool adsrEn, float atk, float dcy, float sus, float rel)
        {
            printf("[ADSR LOAD] adsrEnabled=%s  atk=%.0fms  dcy=%.0fms  sus=%.0f%%  rel=%.0fms\n",
                   adsrEn ? "true" : "false", atk, dcy, sus * 100.0f, rel);
            sampleCard.setAdsrParams(adsrEn, atk, dcy, sus, rel, /*notifyListeners=*/false);
        };

        auto applyEqState = [&](bool eqEn,
                                float f1, float g1, float q1, int m1,
                                float f2, float g2, float q2, int m2,
                                float f3, float g3, float q3, int m3)
        {
            sampleCard.setEqParams(eqEn, f1,g1,q1, f2,g2,q2, f3,g3,q3, /*notifyListeners=*/false);
            pad().eqFilterModes[0] = m1; pad().eqFilterModes[1] = m2; pad().eqFilterModes[2] = m3;
            sampleCard.setEqFilterModes(m1, m2, m3, /*notify=*/false);
            pad().eqActive.store(eqEn);
            const double sr = pad().getSampleRate() > 0.0 ? pad().getSampleRate() : 44100.0;
            PadAudioEngine::EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
            newCoeffs[0] = pad().computeEqCoeffs(f1, g1, q1, m1, sr);
            newCoeffs[1] = pad().computeEqCoeffs(f2, g2, q2, m2, sr);
            newCoeffs[2] = pad().computeEqCoeffs(f3, g3, q3, m3, sr);
            pad().eqCoeffDB.writeFromUI(newCoeffs);
            pad().resetEqState();  // safe: muteOutput=true here
            printf("[EQ] Restored filter modes: band1=%d  band2=%d  band3=%d\n", m1, m2, m3);
        };

        auto applyNormState = [&](bool normEn, float targetDb)
        {
            sampleCard.setNormParams(normEn, targetDb, /*notifyListeners=*/false);
            if (normEn)
            {
                const float gain   = pad().computeNormGainFromAudio(targetDb);
                pad().normGain.store(gain);
                const float gainDb = (gain > 0.0f) ? 20.0f * std::log10f(gain) : 0.0f;
                sampleCard.setNormGainDisplay(gainDb);
            }
            else
            {
                pad().normGain.store(1.0f);
            }
        };

        if (trimSnapshot.valid)
        {
            // ── TRIM LOAD: restore all settings from pre-trim snapshot ────────────
            // Start/end intentionally NOT restored — trimmed file has new length.
            printf("[TRIM] Restoring settings from snapshot — pitch=%+dcents  loop=%s  adsr=%s  eq=%s  norm=%s\n",
                   trimSnapshot.pitchCents,
                   trimSnapshot.loopEnabled ? "ON" : "OFF",
                   trimSnapshot.adsrEnabled ? "ON" : "OFF",
                   trimSnapshot.eqEnabled   ? "ON" : "OFF",
                   trimSnapshot.normEnabled ? "ON" : "OFF");

            effectiveVol  = trimSnapshot.volumeLevel;
            effectiveLoop = trimSnapshot.loopEnabled;

            // Pitch
            sampleCard.setPitchOffset(trimSnapshot.pitchCents);
            sampleCard.setBasePitchOffset(trimSnapshot.basePitchOffset);
            sampleCard.setDetectedNoteName(trimSnapshot.detectedNoteName, trimSnapshot.detectedFreqHz);
            sampleCard.setBaseTuningHz(trimSnapshot.baseTuningHz);
            sampleCard.setPitchStepCents(trimSnapshot.pitchStepCents);

            // Playback modes (quiet setters — no listener fired)
            sampleCard.setOneShotEnabled(trimSnapshot.oneShotEnabled);
            sampleCard.setReverseEnabled(trimSnapshot.reverseEnabled);
            sampleCard.setBounceEnabled(trimSnapshot.bounceEnabled);

            // Transient
            sampleCard.setTransientDetectionEnabled(trimSnapshot.transientDetectionEnabled);
            sampleCard.setTransientThreshold((double)trimSnapshot.transientThreshold);

            // Grid
            sampleCard.setGridSnapEnabled(trimSnapshot.gridSnapEnabled);
            sampleCard.setGridResolutionIndex(trimSnapshot.gridResolutionIndex);

            // ADSR
            applyAdsrState(trimSnapshot.adsrEnabled, trimSnapshot.adsrAttackMs,
                           trimSnapshot.adsrDecayMs, trimSnapshot.adsrSustain, trimSnapshot.adsrReleaseMs);

            // EQ
            applyEqState(trimSnapshot.eqEnabled,
                         trimSnapshot.eq1Freq, trimSnapshot.eq1Gain, trimSnapshot.eq1Q, trimSnapshot.eq1Mode,
                         trimSnapshot.eq2Freq, trimSnapshot.eq2Gain, trimSnapshot.eq2Q, trimSnapshot.eq2Mode,
                         trimSnapshot.eq3Freq, trimSnapshot.eq3Gain, trimSnapshot.eq3Q, trimSnapshot.eq3Mode);

            // Normalize (recomputed on trimmed audio — peak may differ from original)
            applyNormState(trimSnapshot.normEnabled, trimSnapshot.normTargetDb);

            printf("[TRIM] Settings restore complete\n");
        }
        else if (configManager != nullptr)
        {
            // ── NORMAL LOAD: restore from per-sample config key ───────────────────
            auto state = configManager->getSampleState(file);
            if (state.exists)
            {
                effectiveStart = state.startPoint;
                effectiveEnd   = state.endPoint;
                effectiveVol   = state.volume;
                effectiveLoop  = state.loopEnabled;
                sampleCard.setTransientThreshold(state.transientThreshold);
                sampleCard.setDetectedNoteName(state.detectedNoteName, state.detectedFreqHz);
                sampleCard.setBasePitchOffset(state.basePitchOffset);
                applyAdsrState(state.adsrEnabled, state.adsrAttackMs, state.adsrDecayMs,
                               state.adsrSustain, state.adsrReleaseMs);
                printf("[ADSR LOAD] Applied to SampleCard  ->  card reports: en=%s atk=%.0f\n",
                       sampleCard.isAdsrEnabled() ? "true" : "false", sampleCard.getAdsrAttackMs());
                applyEqState(state.eqEnabled,
                             state.eq1Freq, state.eq1Gain, state.eq1Q, state.eq1Mode,
                             state.eq2Freq, state.eq2Gain, state.eq2Q, state.eq2Mode,
                             state.eq3Freq, state.eq3Gain, state.eq3Q, state.eq3Mode);
                applyNormState(state.normEnabled, state.normTargetDb);
                printf("[PERSIST] RESTORED  start=%.3f  end=%.3f  vol=%.2f  loop=%s  thresh=%.1f  note=%s\n",
                       effectiveStart, effectiveEnd, effectiveVol,
                       effectiveLoop ? "ON" : "OFF",
                       state.transientThreshold,
                       state.detectedNoteName.isEmpty() ? "-" : state.detectedNoteName.toRawUTF8());
            }
            else
            {
                // New file — reset all to defaults
                sampleCard.setTransientThreshold(4.0);
                sampleCard.setDetectedNoteName("", 0.0);
                sampleCard.setBasePitchOffset(0);
                applyEqState(false, 100.0f,0.0f,1.0f,2, 500.0f,0.0f,1.0f,2, 8000.0f,0.0f,1.0f,2);
                applyNormState(false, -6.0f);
                printf("[PERSIST] NEW FILE — using defaults  start=0.0  end=full  vol=1.0  loop=OFF  thresh=4.0\n");
            }
        }

        // Pitch: total = global user offset + per-sample base (from Tune).
        // For trim loads, both were restored from the snapshot above.
        sample->pitchOffset = sampleCard.getPitchOffset() + sampleCard.getBasePitchOffset() * 100;
        printf("[PITCH] Sample loaded: user=%+d cents  base=%+d st  total=%+d cents\n",
               sampleCard.getPitchOffset(), sampleCard.getBasePitchOffset(), sample->pitchOffset);

        sample->startPointSeconds = effectiveStart;
        sample->endPointSeconds   = effectiveEnd;

        // Apply start marker (always 0 for trim loads)
        if (effectiveStart > 0.0)
            sampleCard.setStartPoint(effectiveStart);
        // else already reset above

        // Apply end marker (always full for trim loads)
        if (effectiveEnd > 0.0)
            sampleCard.setEndPoint(effectiveEnd);
        // else already reset above (full length)

        // Apply volume
        pad().volumeGain.store(effectiveVol);
        sampleCard.setVolume(effectiveVol);

        // Apply loop
        pad().loopEnabled.store(effectiveLoop);
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
            for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
                if (auto* s = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
                    s->oneShotEnabled.store(oneShot);
        }

        // Propagate current reverse state to the freshly built sound(s).
        {
            const bool rev = sampleCard.isReverseEnabled();
            for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
                if (auto* s = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
                    s->reverseEnabled.store(rev);
        }

        // ── STEP 7: Pre-warm CPU cache for sample start position ────────────────
        // Touch the first and last 2048 frames of each loaded buffer while muteOutput=true.
        // This brings the audio data into L2/L3 cache so the first noteOn has minimal
        // cache-miss latency — the difference between "tight" and "sluggish" drum feel.
        {
            juce::ScopedLock lock(pad().sampleLock);
            for (auto* s : pad().samples)
            {
                if (s->audioData != nullptr && s->audioData->getNumSamples() > 0)
                {
                    const int numCh      = s->audioData->getNumChannels();
                    const int numFrames  = s->audioData->getNumSamples();
                    const int warmFrames = juce::jmin(2048, numFrames);
                    volatile float sink  = 0.0f;  // volatile prevents the compiler from optimising away reads

                    // Touch start region (most likely play position)
                    for (int ch = 0; ch < numCh; ++ch)
                    {
                        const float* p = s->audioData->getReadPointer(ch);
                        for (int i = 0; i < warmFrames; i += 16)  // stride 16 = one cache line (64 bytes / 4)
                            sink += p[i];
                    }
                    // Touch end region (used by reverse + bounce modes)
                    for (int ch = 0; ch < numCh; ++ch)
                    {
                        const float* p = s->audioData->getReadPointer(ch);
                        for (int i = numFrames - warmFrames; i < numFrames; i += 16)
                            sink += p[i];
                    }
                    (void)sink;  // suppress "unused variable" warning
                }
            }
        }

        pad().muteOutput.store(false);

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
                pad().getSynthesiser().noteOn(1, sample->rootNote, 0.8f);
                juce::Timer::callAfterDelay(800, [this, sample]() {
                    pad().getSynthesiser().noteOff(1, sample->rootNote, 0.0f, true);
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
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    // Always update the current sample if one is selected
    if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
    {
        auto* sample = pad().samples[pad().selectedSampleIndex];
        sample->rootNote = newNote;
        sample->lowNote  = newNote;
        sample->highNote = newNote;

        printf("[LEARN-TIMING] updateSamplerSounds called: %.0fms\n",
               juce::Time::getMillisecondCounterHiRes() - t0);

        // LoopingSamplerSound stores the trigger noteRange — must rebuild to change it.
        // This is the ONE and ONLY updateSamplerSounds() call for MIDI note changes.
        updateSamplerSounds();

        printf("[LEARN-TIMING] updateSamplerSounds done: %.0fms\n",
               juce::Time::getMillisecondCounterHiRes() - t0);
    }

    // Deferred save — in-memory only here, one disk write fires 500ms after last change.
    // Do NOT call saveCurrentSession() / flush() synchronously — that blocks the message thread.
    configManager->saveMidiSettings(newNote, sampleCard.getMidiChannel(), currentMidiDeviceName);
    midiSaveTimer.startTimer(500);

    printf("[LEARN-TIMING] saveCurrentSampleState called: %.0fms (deferred 500ms via midiSaveTimer)\n",
           juce::Time::getMillisecondCounterHiRes() - t0);
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
        pad().getSynthesiser().allNotesOff(1, false);
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
        juce::ScopedLock lock(pad().sampleLock);
        if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
            pad().samples[pad().selectedSampleIndex]->pitchOffset = totalCents;
    }

    // Push TOTAL (in cents) to all live sounds atomically — no rebuild, no note cutoff.
    // Audio thread reads pitchOffsetAtomic once per block; 10ms IIR ramp smoothes the transition.
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
    pad().volumeGain.store(volume);
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
        juce::ScopedLock lock(pad().sampleLock);
        if (!pad().samples.isEmpty())
        {
            pad().samples[0]->startPointSeconds = startPointSeconds;
            sampleRate = pad().samples[0]->sampleRate;
        }
    }

    // Push new start position to all live sounds atomically — no rebuild, no note cutoff.
    if (sampleRate > 0.0)
    {
        const juce::int64 newStart = (juce::int64)(startPointSeconds * sampleRate);
        for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        {
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
        juce::ScopedLock lock(pad().sampleLock);
        if (!pad().samples.isEmpty())
        {
            pad().samples[0]->endPointSeconds = endPointSeconds;
            sampleRate = pad().samples[0]->sampleRate;
            if (pad().samples[0]->audioData != nullptr)
                bufLen = pad().samples[0]->audioData->getNumSamples();
        }
    }

    // Push new end position to all live sounds atomically — no rebuild, no note cutoff.
    if (sampleRate > 0.0 && bufLen > 0)
    {
        juce::int64 newEnd = (endPointSeconds > 0.0)
                                 ? (juce::int64)(endPointSeconds * sampleRate)
                                 : bufLen;
        newEnd = juce::jmin(newEnd, bufLen);

        for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        {
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
    pad().loopEnabled.store(isLooping);

    // Update the flag on all currently loaded sounds — no rebuild needed
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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

// computeEqCoeffs, processFftOnWorkerThread, getSpectrumSnapshot moved to PadAudioEngine.h.

void MainComponent::eqParamsChanged(bool enabled,
                                     float f1, float g1, float q1,
                                     float f2, float g2, float q2,
                                     float f3, float g3, float q3)
{
    const juce::int64 t0 = juce::Time::getMillisecondCounter();

    pad().eqActive.store(enabled);

    // OPT 2: Compute all 3 bands on the UI thread, write to double buffer in one shot.
    const double sr = pad().getSampleRate() > 0.0 ? pad().getSampleRate() : 44100.0;
    PadAudioEngine::EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
    newCoeffs[0] = pad().computeEqCoeffs(f1, g1, q1, pad().eqFilterModes[0], sr);
    newCoeffs[1] = pad().computeEqCoeffs(f2, g2, q2, pad().eqFilterModes[1], sr);
    newCoeffs[2] = pad().computeEqCoeffs(f3, g3, q3, pad().eqFilterModes[2], sr);

    const juce::int64 tCoeffs = juce::Time::getMillisecondCounter();
    pad().eqCoeffDB.writeFromUI(newCoeffs);
    const juce::int64 tWrite = juce::Time::getMillisecondCounter();

    // FIX 7: resetEqState() is ONLY safe when muteOutput=true (audio thread not running).
    // resetEqState() is intentionally NOT called here.

    // FIX 4: Always defer save — no synchronous disk write from eqParamsChanged.
    const bool isDragging = sampleCard.isEqDisplayDragging();
    eqSaveTimer.startTimer(400);

    printf("[EQ-TIMING] coeffs computed: %lldms  buffer written: %lldms  dragging=%s  %s  "
           "band1=%.0fHz/%.1fdB/Q%.2f(mode%d)  band2=%.0fHz/%.1fdB/Q%.2f(mode%d)  band3=%.0fHz/%.1fdB/Q%.2f(mode%d)\n",
           (long long)(tCoeffs - t0), (long long)(tWrite - tCoeffs),
           isDragging ? "YES" : "NO",
           enabled ? "ON" : "OFF",
           f1, g1, q1, pad().eqFilterModes[0],
           f2, g2, q2, pad().eqFilterModes[1],
           f3, g3, q3, pad().eqFilterModes[2]);
}

void MainComponent::eqFilterModesChanged(int mode1, int mode2, int mode3)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    printf("[FILTER-TIMING] Filter mode changed: mode1=%d mode2=%d mode3=%d\n", mode1, mode2, mode3);

    pad().eqFilterModes[0] = mode1;
    pad().eqFilterModes[1] = mode2;
    pad().eqFilterModes[2] = mode3;

    // Compute all 3 bands on UI thread, write to double buffer atomically.
    // Audio thread picks up new coefficients on the very next callback — no rebuild, no note cutoff.
    const double sr = pad().getSampleRate() > 0.0 ? pad().getSampleRate() : 44100.0;
    PadAudioEngine::EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
    newCoeffs[0] = pad().computeEqCoeffs(sampleCard.getEqBandFreq(0), sampleCard.getEqBandGain(0), sampleCard.getEqBandQ(0), mode1, sr);
    newCoeffs[1] = pad().computeEqCoeffs(sampleCard.getEqBandFreq(1), sampleCard.getEqBandGain(1), sampleCard.getEqBandQ(1), mode2, sr);
    newCoeffs[2] = pad().computeEqCoeffs(sampleCard.getEqBandFreq(2), sampleCard.getEqBandGain(2), sampleCard.getEqBandQ(2), mode3, sr);
    pad().eqCoeffDB.writeFromUI(newCoeffs);

    const auto tCoeffs = juce::Time::getMillisecondCounter();
    printf("[FILTER-TIMING] coefficients recalculated: %lldms\n", (long long)(tCoeffs - t0));

    // FIX 7: Do NOT call resetEqState() here — same data race issue as eqParamsChanged.
    // Filter state decays naturally through the biquad equation.
    printf("[FILTER-TIMING] resetEqState: SKIPPED — unsafe during active playback (data race)\n");

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

// computeNormGainFromAudio is defined in PadAudioEngine.h and forwarded
// via the inline wrapper in MainComponent.h.

void MainComponent::normChanged(bool enabled, float targetDb)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    printf("[NORM-TIMING] Normalize target changed: value=%.0fdB  enabled=%s\n", targetDb, enabled ? "YES" : "NO");

    if (!enabled)
    {
        pad().normGain.store(1.0f);
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
        juce::ScopedLock lock(pad().sampleLock);
        if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
            if (const auto* s = pad().samples[pad().selectedSampleIndex]; s != nullptr && s->audioData != nullptr)
                scanSamples = s->audioData->getNumSamples();
    }

    constexpr int kBgScanThreshold = 100000;

    if (scanSamples <= kBgScanThreshold || scanSamples == 0)
    {
        // Small sample — scan on message thread, fast enough.
        const float gain   = pad().computeNormGainFromAudio(targetDb);
        pad().normGain.store(gain);
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
            const float gain   = pad().computeNormGainFromAudio(capturedTarget);
            const float gainDb = (gain > 0.0f) ? 20.0f * std::log10f(gain) : 0.0f;
            const auto tScan = juce::Time::getMillisecondCounter();
            printf("[NORM-TIMING] normGain recalculated (bg): %lldms\n", (long long)(tScan - tBg));

            juce::MessageManager::callAsync([this, gain, gainDb]()
            {
                pad().normGain.store(gain);
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
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
    for (int si = 0; si < pad().getSynthesiser().getNumSounds(); ++si)
    {
        if (auto* snd = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(si).get()))
        {
            const juce::int64 sStart = snd->startSampleAtomic.load();
            const juce::int64 sEnd   = snd->endSampleAtomic.load();

            for (int vi = 0; vi < pad().getSynthesiser().getNumVoices(); ++vi)
            {
                if (auto* voice = dynamic_cast<LoopingSamplerVoice*>(pad().getSynthesiser().getVoice(vi)))
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
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
            sound->bounceEnabled.store(enabled);

    if (configManager != nullptr)
        configManager->saveBounceEnabled(enabled);

    printf("[BNC] Bounce playback %s — saved to disk\n", enabled ? "ON" : "OFF");
}


void MainComponent::checkOneShotTailDone()
{
    // Called every 200ms while a one-shot tail is playing.
    // Stop polling once no voice is active.
    bool anyActive = pad().isAnyVoiceActive();

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
        juce::ScopedLock lock(pad().sampleLock);
        if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
        {
            auto* sample      = pad().samples[pad().selectedSampleIndex];
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
            s.eq1Q     = sampleCard.getEqBandQ(0);    s.eq1Mode = pad().eqFilterModes[0];
            s.eq2Freq  = sampleCard.getEqBandFreq(1); s.eq2Gain = sampleCard.getEqBandGain(1);
            s.eq2Q     = sampleCard.getEqBandQ(1);    s.eq2Mode = pad().eqFilterModes[1];
            s.eq3Freq  = sampleCard.getEqBandFreq(2); s.eq3Gain = sampleCard.getEqBandGain(2);
            s.eq3Q     = sampleCard.getEqBandQ(2);    s.eq3Mode = pad().eqFilterModes[2];
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
    pad().muteOutput.store(true);

    // 2. Clear per-sound states so stopNote() and forceStop() work correctly
    pad().clearActiveSoundFlags();

    // 3. Force-stop all voices (releases currentlyPlayingSound refcount)
    pad().forceStopAllVoices();

    // 4. Belt-and-suspenders: MIDI all-notes-off on every channel
    for (int ch = 1; ch <= 16; ++ch)
        pad().getSynthesiser().allNotesOff(ch, false);

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
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* s = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
        {
            s->loopEnabled.store(loopOn);
            s->oneShotEnabled.store(oneShotOn);
            s->bounceEnabled.store(bounceOn);
        }

    // 10. Re-enable audio — engine is now idle and ready for new MIDI triggers
    pad().muteOutput.store(false);

    printf("[PANIC] Reset complete — audio engine ready\n");
}

void MainComponent::performTrimAsync()
{
    // ── Step 1a: snapshot all current SampleCard settings (before any write) ──
    // Start/end points are intentionally NOT captured — the trimmed file has a
    // different length so they reset to 0.0 / full-length after load.
    TrimSettingsSnapshot snap;
    snap.valid                   = true;
    snap.pitchCents              = sampleCard.getPitchOffset();
    snap.basePitchOffset         = sampleCard.getBasePitchOffset();
    snap.baseTuningHz            = sampleCard.getBaseTuningHz();
    snap.pitchStepCents          = sampleCard.getPitchStepCents();
    snap.detectedNoteName        = sampleCard.getDetectedNoteName();
    snap.detectedFreqHz          = sampleCard.getDetectedFreqHz();
    snap.loopEnabled             = sampleCard.isLoopEnabled();
    snap.oneShotEnabled          = sampleCard.isOneShotEnabled();
    snap.reverseEnabled          = sampleCard.isReverseEnabled();
    snap.bounceEnabled           = sampleCard.isBounceEnabled();
    snap.volumeLevel             = sampleCard.getVolume();
    snap.normEnabled             = sampleCard.isNormEnabled();
    snap.normTargetDb            = sampleCard.getNormTargetDb();
    snap.adsrEnabled             = sampleCard.isAdsrEnabled();
    snap.adsrAttackMs            = sampleCard.getAdsrAttackMs();
    snap.adsrDecayMs             = sampleCard.getAdsrDecayMs();
    snap.adsrSustain             = sampleCard.getAdsrSustain();
    snap.adsrReleaseMs           = sampleCard.getAdsrReleaseMs();
    snap.eqEnabled               = sampleCard.isEqEnabled();
    snap.eq1Freq = sampleCard.getEqBandFreq(0); snap.eq1Gain = sampleCard.getEqBandGain(0);
    snap.eq1Q    = sampleCard.getEqBandQ(0);    snap.eq1Mode = sampleCard.getEqFilterMode(0);
    snap.eq2Freq = sampleCard.getEqBandFreq(1); snap.eq2Gain = sampleCard.getEqBandGain(1);
    snap.eq2Q    = sampleCard.getEqBandQ(1);    snap.eq2Mode = sampleCard.getEqFilterMode(1);
    snap.eq3Freq = sampleCard.getEqBandFreq(2); snap.eq3Gain = sampleCard.getEqBandGain(2);
    snap.eq3Q    = sampleCard.getEqBandQ(2);    snap.eq3Mode = sampleCard.getEqFilterMode(2);
    snap.transientDetectionEnabled = sampleCard.isTransientDetectionEnabled();
    snap.transientThreshold        = (float)sampleCard.getTransientThreshold();
    snap.gridSnapEnabled           = sampleCard.isGridSnapEnabled();
    snap.gridResolutionIndex       = sampleCard.getGridResolutionIndex();

    printf("[TRIM] Settings snapshot captured — pitch=%+dcents  loop=%s  adsr=%s  eq=%s  norm=%s\n",
           snap.pitchCents,
           snap.loopEnabled    ? "ON" : "OFF",
           snap.adsrEnabled    ? "ON" : "OFF",
           snap.eqEnabled      ? "ON" : "OFF",
           snap.normEnabled    ? "ON" : "OFF");
    fflush(stdout);

    // ── Step 1b: capture sample data under lock ─────────────────────────────
    juce::File   sourceFile;
    std::shared_ptr<juce::AudioBuffer<float>> audioData;
    double startSec = 0.0, endSec = 0.0, capSampleRate = 44100.0;
    int    capChannels = 1, numTotalSamples = 0;

    {
        juce::ScopedLock lock(pad().sampleLock);
        if (pad().samples.isEmpty() || pad().selectedSampleIndex < 0 ||
            pad().selectedSampleIndex >= (int)pad().samples.size() ||
            pad().samples[pad().selectedSampleIndex] == nullptr ||
            !pad().samples[pad().selectedSampleIndex]->isValid())
        {
            sampleCard.setTrimInProgress(false);
            return;
        }
        auto* s        = pad().samples[pad().selectedSampleIndex];
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
             capSampleRate, capChannels, origFileSize, snap](int result) mutable
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
                                          capSampleRate, capChannels, outFile, origFileSize, snap]() mutable
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
                    juce::MessageManager::callAsync([this, outFile, savedStr, savedBytes, snap]() mutable
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

                        // Load the trimmed file, passing the settings snapshot so all
                        // parameters (pitch, ADSR, EQ, loop, normalize, etc.) are preserved.
                        sampleCard.restoreZoomAndScroll(1.0, 0.0f);
                        loadSampleFileAsync(outFile, /*autoPlay=*/true, /*resetZoom=*/true,
                                            /*deferTransients=*/false, snap);

                        // Show success toast — mention settings preserved
                        juce::String msg = "Trimmed -> " + outFile.getFileName() + " — settings preserved";
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
        for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        {
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
            {
                sound->freezeActive.store(true);
                sound->loopEnabled.store(true);  // freeze always loops
            }
        }

        // If no voice is currently playing, inject a phantom note to start the loop.
        // With freezeActive=true, stopNote is a no-op so the phantom loop runs until freeze is released.
        if (!pad().isAnyVoiceActive() && !pad().samples.isEmpty() && pad().samples[0]->isValid())
            pad().getSynthesiser().noteOn(1, pad().samples[0]->rootNote, 0.8f);

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
        pad().muteOutput.store(true);

        // Step 2: Clear freezeActive on all sounds so stopNote() works normally from now on.
        for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
                sound->freezeActive.store(false);

        // Step 3: Force-stop all voices — directly releases currentlyPlayingSound refcount.
        // Safe: muteOutput=true guarantees the audio thread is not inside renderNextBlock.
        pad().forceStopAllVoices();

        // Step 4: Belt-and-suspenders JUCE voice-state reset (no tail-off).
        pad().getSynthesiser().allNotesOff(0, false);

        // Step 5: Restore loop state on existing sounds (sounds stay in sampler — no rebuild needed).
        const bool loopOn = sampleCard.isLoopEnabled();
        pad().loopEnabled.store(loopOn);
        for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
            if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
                sound->loopEnabled.store(loopOn);

        // Step 6: Resume audio output.
        pad().muteOutput.store(false);

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
    // Runs on the message thread (dispatched via callAsync in handleIncomingMidiMessage).
    // ALL work here is message-thread-safe.
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    printf("[LEARN-TIMING] MIDI note received during learn mode: note=%d\n", noteNumber);

    if (!isLearningMode)
        return;

    // setMidiNoteFromLearn:
    //   1. Updates the card note display
    //   2. Fires midiNoteChanged listener → updates sample->rootNote + calls updateSamplerSounds()
    //      (exactly one rebuild — do NOT call updateSamplerSounds() again below)
    //   3. Deactivates learn mode and fires learningModeChanged(false)
    sampleCard.setMidiNoteFromLearn(noteNumber);
    printf("[LEARN-TIMING] setMidiNote called: %.0fms\n",
           juce::Time::getMillisecondCounterHiRes() - t0);

    // updateSamplerSounds() was already called inside midiNoteChanged listener above.
    // Save is deferred via midiSaveTimer started inside midiNoteChanged.
    printf("[LEARN-TIMING] updateSamplerSounds called: (see midiNoteChanged above)\n");
    printf("[LEARN-TIMING] saveCurrentSampleState called: (deferred 500ms via midiSaveTimer)\n");
    printf("[LEARN-TIMING] learn mode deactivated: %.0fms\n",
           juce::Time::getMillisecondCounterHiRes() - t0);
    printf("[LEARN-TIMING] total learn completion time: %.0fms\n",
           juce::Time::getMillisecondCounterHiRes() - t0);
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
    padManager.setMasterVolume(savedMasterVolume);
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
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
            sound->oneShotEnabled.store(savedOneShot);
    printf("[1SHOT] Restored one shot: %s\n", savedOneShot ? "ON" : "OFF");

    // Restore Reverse on/off state
    bool savedReverse = configManager->getReverseEnabled();
    sampleCard.setReverseEnabled(savedReverse);
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
            sound->reverseEnabled.store(savedReverse);
    printf("[REV] Restored reverse: %s\n", savedReverse ? "ON" : "OFF");

    // Restore Bounce on/off state
    bool savedBounce = configManager->getBounceEnabled();
    sampleCard.setBounceEnabled(savedBounce);
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
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
    juce::ScopedLock lock(pad().sampleLock);
    if (pad().selectedSampleIndex < 0 || pad().selectedSampleIndex >= pad().samples.size()) return;

    auto* sample = pad().samples[pad().selectedSampleIndex];
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
    s.eq1Mode             = pad().eqFilterModes[0];
    s.eq2Freq             = sampleCard.getEqBandFreq(1);
    s.eq2Gain             = sampleCard.getEqBandGain(1);
    s.eq2Q                = sampleCard.getEqBandQ(1);
    s.eq2Mode             = pad().eqFilterModes[1];
    s.eq3Freq             = sampleCard.getEqBandFreq(2);
    s.eq3Gain             = sampleCard.getEqBandGain(2);
    s.eq3Q                = sampleCard.getEqBandQ(2);
    s.eq3Mode             = pad().eqFilterModes[2];
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
    juce::ScopedLock lock(pad().sampleLock);
    if (pad().selectedSampleIndex < 0 || pad().selectedSampleIndex >= pad().samples.size())
    {
        printf("[ADSR-DBG]   → skipped: no valid sample (idx=%d size=%d)\n",
               pad().selectedSampleIndex, pad().samples.size());
        return;
    }

    auto* sample = pad().samples[pad().selectedSampleIndex];
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
    s.eq1Mode             = pad().eqFilterModes[0];
    s.eq2Freq             = sampleCard.getEqBandFreq(1);
    s.eq2Gain             = sampleCard.getEqBandGain(1);
    s.eq2Q                = sampleCard.getEqBandQ(1);
    s.eq2Mode             = pad().eqFilterModes[1];
    s.eq3Freq             = sampleCard.getEqBandFreq(2);
    s.eq3Gain             = sampleCard.getEqBandGain(2);
    s.eq3Q                = sampleCard.getEqBandQ(2);
    s.eq3Mode             = pad().eqFilterModes[2];
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

    if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < pad().samples.size())
    {
        auto* sample = pad().samples[pad().selectedSampleIndex];
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



