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
    
    // Create configuration manager for session persistence
    configManager = std::make_unique<ConfigurationManager>();
    
    // Create the model
    sampleListModel = std::make_unique<SampleListModel>(*this);
    sampleListBox.setModel(sampleListModel.get());
    
    setSize(900, 815);

    // ── Pad grid + global controls (Part 1 — visual only) ──
    addAndMakeVisible (globalControlsBar);
    addAndMakeVisible (padGrid);

    // ── Global Loop Column ──────────────────────────────────────
    addAndMakeVisible (globalLoopColumn);

    globalLoopColumn.onPadSelected = [this] (int slotIdx)
    {
        selectGlobalPad (slotIdx);
    };

    globalLoopColumn.onPadTogglePlayback = [this] (int slotIdx)
    {
        toggleGlobalPadPlayback (slotIdx);
    };

    padGrid.onPadTriggered = [this] (int padIndex)
    {
        if (!padManager.hasEngine (padIndex)) return;
        auto& engine = padManager.getEngine (padIndex);

        // Read the root note assigned to this pad under the sample lock
        int note = padManager.getSettings(padIndex).midiNote;
        {
            juce::ScopedLock sl (engine.sampleLock);
            if (!engine.samples.isEmpty())
                note = engine.samples[0]->rootNote;
        }

        // Trigger note on — full velocity.
        // Note off is sent by onPadReleased (mouseUp), not on a timer.
        engine.getSynthesiser().noteOn (1, note, 1.0f);

        // Record this trigger if the recording engine is active and this isn't the target pad
        if (recIsActive.load (std::memory_order_acquire) && padIndex != recTargetPad)
        {
            const double beatNow = recSongBeatPos.load (std::memory_order_relaxed);
            recEvents.push_back ({ padIndex, beatNow });
        }
    };

    padGrid.onPadReleased = [this] (int padIndex)
    {
        if (!padManager.hasEngine (padIndex)) return;
        auto& engine = padManager.getEngine (padIndex);

        int note = padManager.getSettings (padIndex).midiNote;
        {
            juce::ScopedLock sl (engine.sampleLock);
            if (!engine.samples.isEmpty())
                note = engine.samples[0]->rootNote;
        }

        engine.getSynthesiser().noteOff (1, note, 0.0f, true);
    };

    padGrid.onPadSelected = [this] (int padIndex)
    {
        // FIX 1: INSTANT pad switching — audio is ALREADY in RAM in each PadAudioEngine.
        // Never call loadSampleFileAsync here; that reads from disk and causes multi-second lag.

        const auto t0 = static_cast<juce::int64>(juce::Time::getMillisecondCounter());

        // ── Step 1: Capture outgoing pad state (in-memory, no disk flush) ──
        // MUST happen BEFORE padSelectionSource is changed.  When we were on a G pad,
        // captureSampleCardToPadSettings checks padSelectionSource==Global and routes to
        // captureGlobalPadFromSampleCard — saving G pad state correctly.  If padSelectionSource
        // were switched to Bank first, that guard would miss and the G pad's SampleCard data
        // (midiNote=60, midiChannel=1 defaults) would be written into the outgoing kit pad,
        // corrupting its MIDI mapping.
        captureSampleCardToPadSettings (padManager.selectedPadIndex);
        saveOutgoingSampleState();

        // Switching to a bank pad — deselect any active global pad slot.
        padSelectionSource = PadSelectionSource::Bank;
        globalLoopColumn.clearSelection();
        sampleCard.setRecGlobalPadMode(false);

        // ── Step 2: Switch active pad index — instant (atomic-equivalent for UI thread) ──
        padManager.selectPad (padIndex);

        auto& engine         = padManager.getEngine (padIndex);
        const auto& settings = padManager.getSettings (padIndex);

        // ── Step 3: Restore all UI controls from saved PadSettings — instant ──
        sampleCard.updateUIFromSettings (settings);
        baseTuningLabel.setHz (settings.baseTuningHz);

        // ── Step 3b-MNFreeze: sync the incoming pad's MNFreeze atomics. ──────────────
        // We do NOT kill any active freeze on the outgoing pad — its loop keeps running
        // independently (that is the whole point of MNFreeze: set it and navigate freely).
        // The per-pad padMnFreeze[] arrays mean every pad tracks its own state regardless
        // of which pad is currently selected in the UI.
        {
            auto& mf = padMnFreeze[padManager.selectedPadIndex];
            mf.enabled.store(settings.mnFreezeEnabled, std::memory_order_relaxed);
            mf.note   .store(settings.midiNote,        std::memory_order_relaxed);
            mf.ch     .store(settings.midiChannel,     std::memory_order_relaxed);
        }

        // ── Step 3b: Push EQ state to the new engine's audio pipeline ─────────────
        // updateUIFromSettings uses notifyListeners=false for EQ — the UI is updated
        // but eqParamsChanged listener is NOT fired, so the engine's eqCoeffDB is stale.
        // We must push the coefficients, filter modes, and pad gain manually here.
        {
            engine.eqActive.store(settings.eqEnabled);
            engine.eqFilterModes[0] = settings.eq1Mode;
            engine.eqFilterModes[1] = settings.eq2Mode;
            engine.eqFilterModes[2] = settings.eq3Mode;
            engine.padGain.store(juce::jlimit(0.0f, 2.0f, settings.padGain));

            const double sr = engine.getSampleRate() > 0.0 ? engine.getSampleRate() : 44100.0;
            PadAudioEngine::EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
            newCoeffs[0] = engine.computeEqCoeffs(settings.eq1Freq, settings.eq1Gain, settings.eq1Q, settings.eq1Mode, sr);
            newCoeffs[1] = engine.computeEqCoeffs(settings.eq2Freq, settings.eq2Gain, settings.eq2Q, settings.eq2Mode, sr);
            newCoeffs[2] = engine.computeEqCoeffs(settings.eq3Freq, settings.eq3Gain, settings.eq3Q, settings.eq3Mode, sr);
            engine.eqCoeffDB.writeFromUI(newCoeffs);
        }


        if (!engine.hasSampleLoaded())
        {
            // Empty pad — no audio cached yet.
            sampleCard.setEmptyState (true);
            // Reset folder navigation so Prev/Next won't navigate a stale folder list.
            currentFolder = juce::File{};
            {
                juce::ScopedWriteLock wlock (folderLock);
                folderAudioFiles.clear();
            }
            currentFileIndex = -1;
        }
        else
        {
            // Audio is already in RAM — no disk access needed.
            sampleCard.setEmptyState (false);

            // Update file / duration display without resetting UI controls.
            // setWaveformFileOnly() calls setFile() which clears peaksReady,
            // so setAudioPeaksFromBuffer() MUST come after to restore the peaks.
            sampleCard.setWaveformFileOnly (juce::File (settings.sampleFilePath),
                                            engine.getPeakNumSamples(),
                                            engine.getPeakSampleRate());

            // Deliver cached peak bins to WaveformComponent (deep copy — zero disk I/O).
            sampleCard.setAudioPeaksFromBuffer (engine.getPeakBins(),
                                                engine.getPeakNumBins(),
                                                engine.getPeakNumCh(),
                                                engine.getPeakNumSamples(),
                                                engine.getPeakSampleRate());

            // Restore start/end markers from saved settings.
            // setStartPoint/setEndPoint take seconds; duration must be set first (done above).
            sampleCard.setStartPoint (settings.startPointSeconds);
            if (settings.endPointSeconds > 0.0)
                sampleCard.setEndPoint (settings.endPointSeconds);

            // Restore zoom / scroll position.
            sampleCard.restoreZoomAndScroll (settings.zoomLevel,
                                             settings.zoomScrollPosition);

            // Reset folder navigation state to this pad's folder.
            // folderAudioFiles is cleared so the lazy-scan in loadNext/PrevSample
            // will re-populate it from the correct folder on the first Prev/Next press.
            currentFolder = juce::File (settings.sampleFilePath).getParentDirectory();
            {
                juce::ScopedWriteLock wlock (folderLock);
                folderAudioFiles.clear();
            }
            currentFileIndex = -1;
        }

        // Update the pad grid label.
        padGrid.setPadSampleName (padIndex,
            settings.sampleFilePath.isEmpty()
                ? juce::String{}
                : juce::File (settings.sampleFilePath).getFileName());

    };
    
    formatManager.registerBasicFormats();

    // PadAudioEngine ctor already adds 16 LoopingSamplerVoice instances and enables note stealing.

    // Configure buttons
    menuButton.setButtonText("Menu");
    menuButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF4A4A4A));
    menuButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
    addAndMakeVisible(menuButton);
    menuButton.addListener(this);

    // Kit button — save / load bank kits (.jai files)
    kitButton.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF2A4A6A));
    kitButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFCECECE));
    addAndMakeVisible(kitButton);
    kitButton.onClick = [this] { showKitMenu(); };

    // Kit name display — backlit panel showing the loaded kit filename
    kitNameLabel.setFont(juce::Font(12.0f, juce::Font::bold));
    kitNameLabel.setJustificationType(juce::Justification::centred);
    kitNameLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF101820));  // dark no-kit state
    kitNameLabel.setColour(juce::Label::textColourId,       juce::Colour(0xFF3A5A7A));
    kitNameLabel.setText("no kit loaded", juce::dontSendNotification);
    kitNameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(kitNameLabel);

    // Kit navigation buttons — step through .jai files in the same folder
    for (auto* btn : { &kitPrevButton, &kitNextButton })
    {
        btn->setColour(juce::TextButton::buttonColourId,  juce::Colour(0xFF2A3A4A));
        btn->setColour(juce::TextButton::textColourOffId, juce::Colour(0xFF8AACCC));
        addAndMakeVisible(*btn);
    }
    kitPrevButton.onClick = [this] { navigateKit (-1); };
    kitNextButton.onClick = [this] { navigateKit (+1); };

    // GJM status label — right side of the GlobalControlsBar row
    gjmStatusLabel.setFont (juce::Font (11.0f, juce::Font::bold));
    gjmStatusLabel.setJustificationType (juce::Justification::centredLeft);
    gjmStatusLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xFF101820));
    gjmStatusLabel.setColour (juce::Label::textColourId,       juce::Colour (0xFF3A5A7A));
    gjmStatusLabel.setText ("New Session", juce::dontSendNotification);
    gjmStatusLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (gjmStatusLabel);

    // Wire GlobalControlsBar bank Up/Down to GJM bank switching (always active)
    globalControlsBar.setMaxBank (GjmManager::kNumBanks);
    globalControlsBar.onBankChanged = [this] (int bank1Based)
    {
        if (!gjmParsing.load())
            switchGjmBank (bank1Based - 1);
    };

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

      sampleCard.onTransferKitPadToGlobal = [this] (int kitPadIdx, int globalPadIdx)
      {
          transferKitPadToGlobal(kitPadIdx, globalPadIdx);
      };

      sampleCard.onDropGlobalPad = [this] (int globalPadIdx)
      {
          dropGlobalPad(globalPadIdx);
      };

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
          // FIX 3: No updateSamplerSounds.

          // No disk save — kit must be saved explicitly by the user.
          captureSampleCardToPadSettings(padManager.selectedPadIndex);
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

}

MainComponent::~MainComponent()
{
    heartbeatThread.stopThread(500);
    masterVolumeKnob.setLookAndFeel(nullptr);
    cpuTimer.stopTimer();
    transientDetectionTimer.stopTimer();
    navDebounceTimer.stopTimer();
    patternPlayTimer.stopTimer();
    recIsActive.store(false);
    deviceManager.removeChangeListener(this);

    // Kill any active freeze/loop voices before audio shutdown.
    // PadAudioEngine owns the FFT worker thread — it will be stopped in its destructor.
    pad().muteOutput.store(true);
    pad().clearActiveSoundFlags();
    pad().forceStopAllVoices();
    pad().clearSoundsAndVoices();
    pad().allNotesOff(0, false);

    shutdownAudio();
}

//==============================================================================
void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    printf ("[AUDIO] prepareToPlay called  SR=%.0f  blockSize=%d\n",
            sampleRate, samplesPerBlockExpected);
    fflush (stdout);

    midiCollector.reset(sampleRate);

    // Delegate all audio engine initialization (FFT, EQ, voices) to PadManager.
    padManager.prepareToPlay(samplesPerBlockExpected, sampleRate);

    // Inform EQDisplay of the current sample rate so biquad response rendering is correct.
    sampleCard.setEqSampleRate(sampleRate);

    // OPT 4: Wire spectrum callback — returns true only when new FFT data is ready.
    sampleCard.setEqSpectrumCallback([this](float* dest, int numBins) -> bool {
        return pad().getSpectrumSnapshot(dest, numBins);
    });

    const double bufMs = (double)samplesPerBlockExpected / sampleRate * 1000.0;
    auto* device = deviceManager.getCurrentAudioDevice();
    const char* driverType = device ? device->getTypeName().toRawUTF8() : "none";
    printf ("[LATENCY-REPORT] Buffer: %d samples = %.2f ms  SR: %.0f Hz  Driver: %s\n",
            samplesPerBlockExpected, bufMs, sampleRate, driverType);
    fflush (stdout);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // muteOutput is set true by the message thread during sample-change to guarantee a
    // silent, zeroed buffer while the sampler is being rebuilt.  Checked atomically so
    // the audio thread sees it within one block (~6 ms) with no locks required.
    bufferToFill.clearActiveBufferRegion();
    if (pad().muteOutput.load())
        return;

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

    // ── Live output capture — tap master mix into WAV while pattern plays ─────
    if (liveRenderActive.load (std::memory_order_acquire))
    {
        if (auto* w = liveRenderWriter.load (std::memory_order_relaxed))
            w->write (bufferToFill.buffer->getArrayOfReadPointers(),
                      bufferToFill.numSamples);
    }

    // ── Beat clock + metronome beep ──────────────────────────────────────────
    // The clock runs whenever recording is active, waiting for a beat, or
    // the standalone metronome is on.  This lets Metro work independently.
    {
        const double sr = pad().getSampleRate();
        if (sr > 0.0)
        {
            const bool clockOn = recIsActive.load     (std::memory_order_relaxed)
                              || recWaitForBeat.load  (std::memory_order_relaxed)
                              || metronomeStandalone.load (std::memory_order_relaxed);

            if (clockOn)
            {
                const double bps   = recBpmAtomic.load (std::memory_order_relaxed) / 60.0;
                const double delta = (double)bufferToFill.numSamples / sr * bps;
                const double oldBeat = recSongBeatPos.load (std::memory_order_relaxed);
                const double newBeat = oldBeat + delta;

                // ── Quantized record arm: fire at next beat boundary ──────────
                if (recWaitForBeat.load (std::memory_order_relaxed))
                {
                    // Count WAV pre-roll samples while waiting
                    recBeatSampleOffset.fetch_add (bufferToFill.numSamples,
                                                   std::memory_order_relaxed);

                    if ((int)std::floor (newBeat) > (int)std::floor (oldBeat))
                    {
                        // Beat crossed — arm recording from this exact block
                        recWaitForBeat.store (false, std::memory_order_relaxed);
                        recIsActive.store    (true,  std::memory_order_release);
                        recSongBeatPos.store (0.0,   std::memory_order_relaxed);
                        recLastBeat = -1;  // trigger beep immediately at beat 0
                        juce::MessageManager::callAsync ([this]()
                        {
                            sampleCard.setRecordingActive (true, recTargetPad);
                        });
                    }
                    else
                    {
                        recSongBeatPos.store (newBeat, std::memory_order_relaxed);
                    }
                }
                else
                {
                    recSongBeatPos.store (newBeat, std::memory_order_relaxed);
                }

                // ── Metronome beep trigger ────────────────────────────────────
                if (recMetronomeOn.load (std::memory_order_relaxed))
                {
                    const double beat   = recSongBeatPos.load (std::memory_order_relaxed);
                    const int beatInt   = (int)std::floor (beat);
                    if (beatInt > recLastBeat)
                    {
                        recLastBeat       = beatInt;
                        recMetroBeepLeft  = (int)(sr * 0.020);  // 20ms burst
                        recMetroBeepPhase = 0.0;
                    }
                }
            }

            // Mix metronome beep (finishes even after clock stops)
            if (recMetroBeepLeft > 0)
            {
                const double phaseInc = juce::MathConstants<double>::twoPi * 1000.0 / sr;
                const int n  = juce::jmin (recMetroBeepLeft, bufferToFill.numSamples);
                const float mv  = padManager.getMasterVolume();
                const float vol = metronomeVolume.load (std::memory_order_relaxed);

                for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch)
                {
                    float* data = bufferToFill.buffer->getWritePointer (ch, 0);
                    for (int i = 0; i < n; ++i)
                    {
                        const float t   = (float)(i + 0.5f) / (float)n;
                        const float env = std::sin (t * juce::MathConstants<float>::pi);
                        data[i] += (float)(std::sin (recMetroBeepPhase + (double)i * phaseInc)
                                           * vol * env * mv);
                    }
                }
                recMetroBeepPhase += phaseInc * n;
                while (recMetroBeepPhase >= juce::MathConstants<double>::twoPi)
                    recMetroBeepPhase -= juce::MathConstants<double>::twoPi;
                recMetroBeepLeft -= n;
            }
        }
    }

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
            juce::ignoreUnused(elapsedUs, budgetUs);
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
    // Left trim: margin(20) + menu(60) + gap(4+4) = 88px
    g.setColour(juce::Colour(0xFFCECECE));
    g.setFont(juce::Font(18.0f, juce::Font::bold));
    auto titleArea = getLocalBounds().withTrimmedLeft(88).withTrimmedRight(348).removeFromTop(40);
    g.drawText("JAIVA-SAMPLER||1.0", titleArea, juce::Justification::centred, true);

    // Line above footer (footer is 50px from bottom)
    g.setColour(juce::Colour(0xFF404040));
    auto footerY = getHeight() - 55;
    g.drawHorizontalLine(footerY, 20, getWidth() - 20);

    // Draw dark outline around top-level buttons
    g.setColour(juce::Colour(0xFF0A0A0A));
    g.drawRect(menuButton.getBounds(), 1);
    g.drawRect(kitButton.getBounds(), 1);
    g.drawRect(kitPrevButton.getBounds(), 1);
    g.drawRect(kitNextButton.getBounds(), 1);
    if (gjmStatusLabel.isVisible() && gjmStatusLabel.getWidth() > 0)
        g.drawRect(gjmStatusLabel.getBounds(), 1);
    g.drawRect(resetButton.getBounds(), 1);
    g.drawRect(baseTuningLabel.getBounds(), 1);
    g.drawRect(testToneButton.getBounds(), 1);
    g.drawRect(masterVolumeKnob.getBounds(), 1);
}

void MainComponent::paintOverChildren(juce::Graphics& g)
{
    // Draw decorative kit-name panel border after children have painted.
    // The label itself draws its own solid background; we add glow + top-edge highlight.
    const auto b = kitNameLabel.getBounds().toFloat();
    const bool hasKit = kitNameLabel.getText() != "no kit loaded";

    // Outer dark bezel (just outside the label bounds — doesn't cover text)
    g.setColour(juce::Colour(0xFF050C14));
    g.drawRoundedRectangle(b.expanded(1.0f), 5.0f, 1.5f);

    // Inner top-edge highlight — simulates a backlit edge
    g.setColour(hasKit ? juce::Colour(0x6080C8FF) : juce::Colour(0x304060A0));
    g.drawLine(b.getX() + 5.0f, b.getY() + 1.0f,
               b.getRight() - 5.0f, b.getY() + 1.0f, 1.0f);

    // Subtle rim glow
    g.setColour(hasKit ? juce::Colour(0xFF1A5080) : juce::Colour(0xFF0A1828));
    g.drawRoundedRectangle(b, 4.0f, 1.0f);
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
    constexpr int kCardMinH  = 250;
    constexpr int kFooterH   = 50;
    constexpr int kHMargin   = 20;
    constexpr int kGap       = 10;

    // Card height fills whatever space is left between the two pad rows.
    const int fixedRowsH   = kTopBarH + kGlobCtrlH + kPadRowH + kGap + kGap + kPadRowH + kFooterH;
    const int dynamicCardH = juce::jmax(kCardMinH, getHeight() - fixedRowsH);

    // Consume from top (full width — no horizontal margins yet)
    auto strip = getLocalBounds();

    // =========================================================
    // TOP BAR  (40 px, with horizontal margins)
    // =========================================================
    {
        auto topBar = strip.removeFromTop (kTopBarH).reduced (kHMargin, 0);

        menuButton.setBounds (topBar.removeFromLeft (60).withSizeKeepingCentre (56, 30));
        topBar.removeFromLeft (4);

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
    {
        auto globRow = strip.removeFromTop (kGlobCtrlH);
        globalControlsBar.setBounds (globRow);

        // Kit button + name display: placed after bank controls in the same row.
        // GlobalControlsBar uses reduced(8,0) internally; bank controls = 144px.
        // We mirror that offset so kit controls align flush with bank buttons.
        auto kitArea = globRow.reduced (8, 0);
        kitArea.removeFromLeft (144);      // skip bank controls
        kitArea.removeFromLeft (4);        // gap after bankDownBtn
        kitButton.setBounds    (kitArea.removeFromLeft (44).withSizeKeepingCentre (40, 20));
        kitArea.removeFromLeft (4);
        // Reserve space for < > buttons (24px each) + 2px gap + 4px trailing gap = 54px
        constexpr int kNavBtnW = 24;
        constexpr int kNavReserve = kNavBtnW + 2 + kNavBtnW + 4;
        const int labelW = juce::jmin (180, kitArea.getWidth() - kNavReserve - 4);
        kitNameLabel.setBounds   (kitArea.removeFromLeft (labelW).withSizeKeepingCentre (labelW, 20));
        kitArea.removeFromLeft (4);
        kitPrevButton.setBounds  (kitArea.removeFromLeft (kNavBtnW).withSizeKeepingCentre (kNavBtnW, 20));
        kitArea.removeFromLeft (2);
        kitNextButton.setBounds  (kitArea.removeFromLeft (kNavBtnW).withSizeKeepingCentre (kNavBtnW, 20));

        // GJM status label — fills whatever space remains to the right of kit controls
        kitArea.removeFromLeft (10);
        if (kitArea.getWidth() > 30)
            gjmStatusLabel.setBounds (kitArea.withSizeKeepingCentre (
                juce::jmin (220, kitArea.getWidth()), 20));
    }

    // =========================================================
    // TOP TRIANGLE PAD ROW  (kPadRowH, full width)
    // =========================================================
    const int topPadRowY = strip.getY();
    strip.removeFromTop (kPadRowH);

    strip.removeFromTop (kGap);

    // =========================================================
    // SAMPLE CARD + GLOBAL LOOP COLUMN  (kCardH, centred)
    // =========================================================
    {
        constexpr int kGlobalColW = 62;  // width of GlobalLoopColumn
        constexpr int kGlobalColGap = 6; // gap between card and column

        auto cardStrip = strip.removeFromTop (dynamicCardH);
        int  totalW    = getWidth() - kHMargin * 2;
        if (totalW < 100) totalW = 100;

        // Reserve space for GlobalLoopColumn on the right
        const int cardW = juce::jmax (100, totalW - kGlobalColW - kGlobalColGap);

        // Centre the combined block (card + gap + column) in the available width
        const int combinedW = cardW + kGlobalColGap + kGlobalColW;
        const int startX    = kHMargin + (totalW - combinedW) / 2;

        sampleCard.setBounds (startX, cardStrip.getY(), cardW, dynamicCardH);
        globalLoopColumn.setBounds (startX + cardW + kGlobalColGap,
                                    cardStrip.getY(),
                                    kGlobalColW, dynamicCardH);
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
        const int total = kTopBarH + kGlobCtrlH + kPadRowH + kGap + dynamicCardH + kGap + kPadRowH + kFooterH;
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
        }
    }
    {
        const juce::int64 eqMs = pad().eqResetElapsedMs.exchange(-1, std::memory_order_relaxed);
        if (eqMs >= 0)
        {
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

    // Persist the FULL device manager state (output + input device, active channel
    // bitmasks, buffer size, sample rate).  createStateXml() captures everything
    // the AudioDeviceSelectorComponent can configure, so no information is lost.
    auto xml = deviceManager.createStateXml();
    if (xml != nullptr)
    {
        const juce::String xmlText = xml->toString();
        configManager->saveAudioDeviceStateXml (xmlText);
        printf ("[AUDIO] Full device state saved (%d bytes)\n", xmlText.length());
        fflush (stdout);
    }
}

void MainComponent::loadAudioSettings()
{
    if (configManager == nullptr)
        return;

    const juce::String xmlText = configManager->getAudioDeviceStateXml();
    if (xmlText.isNotEmpty())
    {
        // Parse the stored XML and hand it back to the device manager.
        // preferredSetupOptions = nullptr means: use exactly what's in the XML.
        auto xml = juce::XmlDocument::parse (xmlText);
        if (xml != nullptr)
        {
            // setAudioChannels has already been called (with 0 in / 2 out) before
            // loadAudioSettings() is invoked.  Passing the saved XML to initialise()
            // reopens the device with the saved configuration — input device, active
            // channels, buffer size, sample rate — overriding the defaults.
            juce::String error = deviceManager.initialise (0, 2, xml.get(), true);
            if (error.isNotEmpty())
            {
                printf ("[AUDIO] loadAudioSettings: initialise error: %s\n", error.toRawUTF8());
                fflush (stdout);
            }
            else
            {
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                deviceManager.getAudioDeviceSetup (setup);
                printf ("[AUDIO] Device state restored — output: %s  input: %s  buf: %d  SR: %.0f\n",
                        setup.outputDeviceName.toRawUTF8(),
                        setup.inputDeviceName.toRawUTF8(),
                        setup.bufferSize,
                        setup.sampleRate);
                fflush (stdout);
            }
            return;
        }
    }

    // Fallback: no saved XML yet — use whatever setAudioChannels opened as default.
    printf ("[AUDIO] No saved audio device state — using system default\n");
    fflush (stdout);
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
    
}

void MainComponent::showMidiDeviceSettings()
{

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

    // MNFreeze intercept: check ALL pads — any pad with MNFreeze enabled and a matching
    // MIDI note/channel will toggle its freeze loop, regardless of which pad is selected.
    // Runs on the MIDI callback thread using relaxed atomics only — no blocking I/O.
    if (message.isNoteOn())
    {
        const int note    = message.getNoteNumber();
        const int channel = message.getChannel();

        for (int padIdx = 0; padIdx < 16; ++padIdx)
        {
            auto& mf = padMnFreeze[padIdx];
            if (!mf.enabled.load(std::memory_order_relaxed)) continue;
            if (!padManager.hasEngine(padIdx))                continue;

            const int padNote = mf.note.load(std::memory_order_relaxed);
            const int padCh   = mf.ch  .load(std::memory_order_relaxed);

            if (note == padNote && (padCh == 0 || channel == padCh))
            {
                // Toggle this pad's freeze state atomically.
                const bool wasActive = mf.isActive.load(std::memory_order_relaxed);
                mf.isActive.store(!wasActive, std::memory_order_relaxed);
                const bool nowActive  = !wasActive;
                const float velocity  = message.getFloatVelocity();

                juce::MessageManager::callAsync([this, padIdx, nowActive, velocity]
                {
                    activateMnFreezeForPad(padIdx, nowActive, velocity);
                });

                // Note consumed by MNFreeze — do not pass to the sampler.
                return;
            }
        }
    }

    // Global loop pad MIDI routing — checked before bank-pad routing.
    // Each global pad has its own midiNote/midiChannel in padManager.globalSettings[].
    if (message.isNoteOn() || message.isNoteOff())
    {
        const int note    = message.getNoteNumber();
        const int channel = message.getChannel();

        for (int g = 0; g < PadManager::kNumGlobalPads; ++g)
        {
            const auto& gs = padManager.globalSettings[g];
            if (gs.midiNote >= 0 && gs.midiChannel >= 0
                && gs.midiNote == note && (gs.midiChannel == 0 || gs.midiChannel == channel))
            {
                if (message.isNoteOn() && padManager.hasGlobalEngine(g))
                {
                    // Inject noteOn directly — global engines don't go through midiCollector
                    padManager.getGlobalEngine(g).getSynthesiser().noteOn(
                        channel, note, message.getFloatVelocity());
                    juce::MessageManager::callAsync([this, g] {
                        globalLoopColumn.triggerFlash(g);
                    });
                }
                else if (message.isNoteOff() && padManager.hasGlobalEngine(g))
                {
                    padManager.getGlobalEngine(g).getSynthesiser().noteOff(
                        channel, note, message.getFloatVelocity(), true);
                }
                return;  // consumed by global pad
            }
        }
    }

    // Channel filter: drop messages on wrong channel (0 = any).
    // Skip filter entirely when a G pad is displayed — G pads default to midiChannel=1
    // and would incorrectly block all MIDI from kit pads on other channels while the
    // SampleCard shows a G pad.  The per-pad synthesizer note-range already handles routing.
    if (padSelectionSource == PadSelectionSource::Bank)
    {
        const int selectedChannel = sampleCard.getMidiChannel();
        if (selectedChannel != 0 && message.getChannel() != selectedChannel)
            return;
    }

    // Add to lock-free FIFO — audio thread drains this each block.
    // This is the ONLY operation that affects audio latency in this path.
    midiCollector.addMessageToQueue(message);

    // Light up the pad for the duration of the MIDI note.
    // triggerStart on noteOn, triggerEnd on noteOff — no timer involved.
    {
                const int flashPad = padManager.findPadForMidiNote(
            message.getNoteNumber(), message.getChannel());
        if (flashPad >= 0)
        {
            if (message.isNoteOn())
            {
                juce::MessageManager::callAsync([this, flashPad]()
                    { padGrid.triggerStart(flashPad); });

                // Capture MIDI note-on events for quantized recording only.
                // Skipped when recQuantInBeats == 0 ("None") — raw recording needs no events,
                // and the push_back (plus any reallocation) must not run on the MIDI callback thread.
                if (recIsActive.load(std::memory_order_acquire)
                    && flashPad != recTargetPad
                    && recQuantInBeats > 0.0)
                {
                    const double beatNow = recSongBeatPos.load(std::memory_order_acquire);
                    recEvents.push_back({ flashPad, beatNow });
                }
            }
            else if (message.isNoteOff())
                juce::MessageManager::callAsync([this, flashPad]()
                    { padGrid.triggerEnd(flashPad); });
        }
    }

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
            }
            else
            {
                highNoteSlider.setValue(sample->highNote);
            }
        }
        else if (slider == &rootNoteSlider)
        {
            sample->rootNote = (int)rootNoteSlider.getValue();
        }
        
        // Update sampler with new mapping
        updateSamplerSounds();
        sampleListBox.updateContent();
    }
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

}

void MainComponent::preloadPadEngineAsync(int padIdx, juce::File file, PadSettings settings)
{
    // Run on a background thread so disk I/O never blocks the message thread.
    backgroundThreads.addJob([this, padIdx, file, settings]()
    {

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (reader == nullptr)
        {
            return;
        }

        auto* sample = new MappedSample();
        sample->file            = file;
        sample->name            = file.getFileName();
        sample->rootNote        = settings.midiNote;
        sample->lowNote         = settings.midiNote;
        sample->highNote        = settings.midiNote;
        sample->sampleRate      = reader->sampleRate;
        sample->numChannels     = reader->numChannels;
        sample->lengthInSamples = reader->lengthInSamples;
        sample->attack          = 0.01;
        sample->release         = 0.1;
        sample->startPointSeconds = settings.startPointSeconds;
        sample->endPointSeconds   = settings.endPointSeconds;
        sample->pitchOffset       = settings.pitchCents;

        auto buffer = std::make_shared<juce::AudioBuffer<float>>(
            (int)reader->numChannels, (int)reader->lengthInSamples);
        if (!reader->read(buffer.get(), 0, (int)reader->lengthInSamples, 0, true, true))
        {
            delete sample;
            return;
        }
        sample->audioData = std::move(buffer);

        // Pre-compute waveform peaks for instant display on pad switch.
        const int    peakNumCh    = (int)reader->numChannels;
        const juce::int64 peakNS = reader->lengthInSamples;
        const double peakSR      = reader->sampleRate;

        auto peaks = std::make_unique<WaveformPeakBin[]>(kWaveformPeakBins);
        {
            const float* L = sample->audioData->getReadPointer(0);
            const float* R = (peakNumCh > 1) ? sample->audioData->getReadPointer(1) : L;
            for (int bin = 0; bin < kWaveformPeakBins; ++bin)
            {
                const juce::int64 sS = (juce::int64)((double)bin       / kWaveformPeakBins * peakNS);
                const juce::int64 sE = juce::jmin((juce::int64)((double)(bin+1) / kWaveformPeakBins * peakNS), peakNS);
                WaveformPeakBin& pb = peaks[bin];
                pb.minL = pb.minR = 1.0f; pb.maxL = pb.maxR = -1.0f;
                for (juce::int64 s = sS; s < sE; ++s)
                {
                    pb.minL = juce::jmin(pb.minL, L[s]); pb.maxL = juce::jmax(pb.maxL, L[s]);
                    pb.minR = juce::jmin(pb.minR, R[s]); pb.maxR = juce::jmax(pb.maxR, R[s]);
                }
            }
        }


        juce::MessageManager::callAsync(
            [this, padIdx, sample, settings, file,
             peaks = std::move(peaks), peakNumCh, peakNS, peakSR]() mutable
        {
            PadAudioEngine& engine = padManager.getEngine(padIdx);

            // Hard-stop any existing audio in that engine slot.
            engine.muteOutput.store(true);
            engine.clearActiveSoundFlags();
            engine.forceStopAllVoices();
            engine.clearSoundsAndVoices();
            engine.allNotesOff(0, false);

            // Install audio buffer.
            {
                juce::ScopedLock lock(engine.sampleLock);
                engine.samples.clear();
                engine.selectedSampleIndex = 0;
                engine.samples.add(sample);
            }

            // Store peak cache for instant waveform display on pad switch.
            engine.storePeakCache(peaks.get(), kWaveformPeakBins, peakNumCh, peakNS, peakSR);

            // Create LoopingSamplerSound so MIDI notes trigger this pad.
            class DummyAudioReader : public juce::AudioFormatReader
            {
            public:
                DummyAudioReader(double sr, unsigned int ch)
                    : juce::AudioFormatReader(nullptr, "Dummy")
                { sampleRate=sr; numChannels=ch; lengthInSamples=1; bitsPerSample=32; usesFloatingPointData=true; }
                bool readSamples(int* const* dest, int numDest, int startOff,
                                 juce::int64, int num) override
                {
                    for (int ch=0;ch<numDest;++ch)
                        if(dest[ch]) memset(reinterpret_cast<float*>(dest[ch])+startOff,0,(size_t)num*sizeof(float));
                    return true;
                }
            };
            DummyAudioReader dummyReader(sample->sampleRate, (unsigned int)sample->numChannels);

            const juce::int64 bufTotal = (juce::int64)sample->audioData->getNumSamples();
            juce::int64 startSmp = 0, endSmp = juce::jmax((juce::int64)1, bufTotal - 1);
            if (sample->startPointSeconds > 0.0 && sample->sampleRate > 0)
                startSmp = juce::jlimit((juce::int64)0, endSmp-1, (juce::int64)(sample->startPointSeconds * sample->sampleRate));
            if (sample->endPointSeconds > 0.0 && sample->sampleRate > 0)
                endSmp = juce::jlimit(startSmp+1, bufTotal-1, (juce::int64)(sample->endPointSeconds * sample->sampleRate));

            juce::BigInteger noteRange;
            noteRange.setRange(0, 128, false);
            noteRange.setBit(sample->rootNote);

            auto* sound = new LoopingSamplerSound(
                sample->name, dummyReader, noteRange,
                sample->rootNote, sample->attack, sample->release, 10.0);
            sound->fullAudioData = sample->audioData;
            sound->startSampleAtomic.store(startSmp);
            sound->endSampleAtomic.store(endSmp);
            sound->loopEnabled.store(settings.loopEnabled);
            sound->pitchOffsetAtomic.store(settings.pitchCents);
            sound->oneShotEnabled.store(settings.oneShotEnabled);
            sound->baseTuningRatioAtomic.store(1.0f);  // 440 Hz default at startup
            sound->customAdsrEnabled.store(settings.adsrEnabled);
            sound->customAdsrAttackMs.store(settings.adsrAttackMs);
            sound->customAdsrDecayMs.store(settings.adsrDecayMs);
            sound->customAdsrSustain.store(settings.adsrSustain);
            sound->customAdsrReleaseMs.store(settings.adsrReleaseMs);
            sound->reverseEnabled.store(settings.reverseEnabled);
            sound->bounceEnabled.store(settings.bounceEnabled);
            engine.getSynthesiser().addSound(sound);

            engine.loopEnabled.store(settings.loopEnabled);
            engine.volumeGain.store(settings.volumeLevel);

            engine.muteOutput.store(false);

            // Update grid label with filename (in case it wasn't set yet).
            padGrid.setPadSampleName(padIdx, file.getFileName());

        });
    });
}

void MainComponent::preloadGlobalPadEngineAsync(int globalPadIdx, juce::File file, PadSettings settings)
{
    backgroundThreads.addJob([this, globalPadIdx, file, settings]()
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (reader == nullptr)
            return;

        auto* sample = new MappedSample();
        sample->file            = file;
        sample->name            = file.getFileName();
        sample->rootNote        = juce::jlimit(0, 127, settings.midiNote >= 0 ? settings.midiNote : 60);
        sample->lowNote         = sample->rootNote;
        sample->highNote        = sample->rootNote;
        sample->sampleRate      = reader->sampleRate;
        sample->numChannels     = reader->numChannels;
        sample->lengthInSamples = reader->lengthInSamples;
        sample->attack          = 0.01;
        sample->release         = 0.1;
        sample->startPointSeconds = settings.startPointSeconds;
        sample->endPointSeconds   = settings.endPointSeconds;
        sample->pitchOffset       = settings.pitchCents;

        auto buffer = std::make_shared<juce::AudioBuffer<float>>(
            (int)reader->numChannels, (int)reader->lengthInSamples);
        if (!reader->read(buffer.get(), 0, (int)reader->lengthInSamples, 0, true, true))
        {
            delete sample;
            return;
        }
        sample->audioData = std::move(buffer);

        const int         peakNumCh = (int)reader->numChannels;
        const juce::int64 peakNS   = reader->lengthInSamples;
        const double      peakSR   = reader->sampleRate;

        auto peaks = std::make_unique<WaveformPeakBin[]>(kWaveformPeakBins);
        {
            const float* L = sample->audioData->getReadPointer(0);
            const float* R = (peakNumCh > 1) ? sample->audioData->getReadPointer(1) : L;
            for (int bin = 0; bin < kWaveformPeakBins; ++bin)
            {
                const juce::int64 sS = (juce::int64)((double)bin       / kWaveformPeakBins * peakNS);
                const juce::int64 sE = juce::jmin((juce::int64)((double)(bin+1) / kWaveformPeakBins * peakNS), peakNS);
                WaveformPeakBin& pb = peaks[bin];
                pb.minL = pb.minR = 1.0f; pb.maxL = pb.maxR = -1.0f;
                for (juce::int64 s = sS; s < sE; ++s)
                {
                    pb.minL = juce::jmin(pb.minL, L[s]); pb.maxL = juce::jmax(pb.maxL, L[s]);
                    pb.minR = juce::jmin(pb.minR, R[s]); pb.maxR = juce::jmax(pb.maxR, R[s]);
                }
            }
        }

        juce::MessageManager::callAsync(
            [this, globalPadIdx, sample, settings, file,
             peaks = std::move(peaks), peakNumCh, peakNS, peakSR]() mutable
        {
            PadAudioEngine& engine = padManager.getGlobalEngine(globalPadIdx);

            engine.muteOutput.store(true);
            engine.clearActiveSoundFlags();
            engine.forceStopAllVoices();
            engine.clearSoundsAndVoices();
            engine.allNotesOff(0, false);

            {
                juce::ScopedLock lock(engine.sampleLock);
                engine.samples.clear();
                engine.selectedSampleIndex = 0;
                engine.samples.add(sample);
            }

            engine.storePeakCache(peaks.get(), kWaveformPeakBins, peakNumCh, peakNS, peakSR);

            class DummyAudioReader : public juce::AudioFormatReader
            {
            public:
                DummyAudioReader(double sr, unsigned int ch)
                    : juce::AudioFormatReader(nullptr, "Dummy")
                { sampleRate=sr; numChannels=ch; lengthInSamples=1; bitsPerSample=32; usesFloatingPointData=true; }
                bool readSamples(int* const* dest, int numDest, int startOff,
                                 juce::int64, int num) override
                {
                    for (int ch=0;ch<numDest;++ch)
                        if(dest[ch]) memset(reinterpret_cast<float*>(dest[ch])+startOff,0,(size_t)num*sizeof(float));
                    return true;
                }
            };
            DummyAudioReader dummyReader(sample->sampleRate, (unsigned int)sample->numChannels);

            const juce::int64 bufTotal = (juce::int64)sample->audioData->getNumSamples();
            juce::int64 startSmp = 0, endSmp = juce::jmax((juce::int64)1, bufTotal - 1);
            if (sample->startPointSeconds > 0.0 && sample->sampleRate > 0)
                startSmp = juce::jlimit((juce::int64)0, endSmp-1, (juce::int64)(sample->startPointSeconds * sample->sampleRate));
            if (sample->endPointSeconds > 0.0 && sample->sampleRate > 0)
                endSmp = juce::jlimit(startSmp+1, bufTotal-1, (juce::int64)(sample->endPointSeconds * sample->sampleRate));

            juce::BigInteger noteRange;
            noteRange.setRange(0, 128, false);
            noteRange.setBit(sample->rootNote);

            auto* sound = new LoopingSamplerSound(
                sample->name, dummyReader, noteRange,
                sample->rootNote, sample->attack, sample->release, 10.0);
            sound->fullAudioData = sample->audioData;
            sound->startSampleAtomic.store(startSmp);
            sound->endSampleAtomic.store(endSmp);
            sound->loopEnabled.store(settings.loopEnabled);
            sound->pitchOffsetAtomic.store(settings.pitchCents);
            sound->oneShotEnabled.store(settings.oneShotEnabled);
            sound->baseTuningRatioAtomic.store(1.0f);
            sound->customAdsrEnabled.store(settings.adsrEnabled);
            sound->customAdsrAttackMs.store(settings.adsrAttackMs);
            sound->customAdsrDecayMs.store(settings.adsrDecayMs);
            sound->customAdsrSustain.store(settings.adsrSustain);
            sound->customAdsrReleaseMs.store(settings.adsrReleaseMs);
            sound->reverseEnabled.store(settings.reverseEnabled);
            sound->bounceEnabled.store(settings.bounceEnabled);
            engine.getSynthesiser().addSound(sound);

            engine.loopEnabled.store(settings.loopEnabled);
            engine.volumeGain.store(settings.volumeLevel);

            engine.muteOutput.store(false);

            // Update the column state now that audio is ready.
            globalLoopColumn.setSlotName (globalPadIdx, file.getFileName());
            globalLoopColumn.setSlotState (globalPadIdx, GlobalLoopColumn::SlotState::Loaded);

            DBG ("[G-PAD] preloadGlobalPadEngineAsync done: G" + juce::String(globalPadIdx + 1)
                 + " = " + file.getFileName());

            // If this G pad is currently selected, refresh the waveform display.
            if (padSelectionSource == PadSelectionSource::Global
                && selectedGlobalPadIndex == globalPadIdx)
            {
                selectGlobalPad(globalPadIdx);
            }
        });
    });
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

    }

}

//==============================================================================
// Kit save / load (.jai bank kit files)

void MainComponent::showKitMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Session");
    menu.addItem (5, "New Session");
    menu.addItem (3, gjmManager.isUntitled ? "Save Session..." : "Save Session");
    menu.addItem (6, "Save Session As...");
    menu.addItem (4, "Load Session...");
    menu.addSeparator();
    menu.addSectionHeader ("Current Bank Kit");
    menu.addItem (1, "Save Bank Kit...");
    menu.addItem (2, "Load Bank Kit...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&kitButton),
        [this] (int result)
        {
            if      (result == 5) newKitAction();
            else if (result == 3) saveSessionAction (false);
            else if (result == 6) saveSessionAction (true);
            else if (result == 4) loadSessionAction();
            else if (result == 1) saveBankKitAction();
            else if (result == 2) loadBankKitAction();
        });
}

void MainComponent::saveKitToFile (const juce::File& file)
{
    // Flush current SampleCard state so norm/EQ/gain changes made since the
    // last change-event (especially deferred async norm scans) are captured.
    captureSampleCardToPadSettings (padManager.selectedPadIndex);

    auto root = std::make_unique<juce::XmlElement> ("JaivaKit");
    root->setAttribute ("version",     1);
    root->setAttribute ("appVersion",  "1.0.0");
    root->setAttribute ("savedDate",   juce::Time::getCurrentTime().toString (true, true));

    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        auto* padEl = root->createNewChildElement ("Pad");
        padEl->setAttribute ("index", i);
        padManager.padSettings[i].saveToXml (*padEl);
    }

    if (root->writeTo (file))
    {
        currentKitFile = file;
        kitIsDirty = false;
        kitNameLabel.setText(file.getFileNameWithoutExtension(), juce::dontSendNotification);
        kitNameLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF1A3050));
        kitNameLabel.setColour(juce::Label::textColourId,       juce::Colour(0xFFB8DEFF));
        sampleCard.showTrimToast ("Kit saved: " + file.getFileName(), false);
    }
    else
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Save Failed",
            "Could not write to:\n" + file.getFullPathName(),
            "OK", nullptr);
}

void MainComponent::loadKitFromFile (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || xml->getTagName() != "JaivaKit")
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Load Failed",
            "Not a valid .jai kit file:\n" + file.getFullPathName(),
            "OK", nullptr);
        return;
    }

    // Parse all pad settings from XML
    for (auto* padEl : xml->getChildIterator())
    {
        if (padEl->getTagName() != "Pad") continue;
        const int idx = padEl->getIntAttribute ("index", -1);
        if (idx < 0 || idx >= PadManager::kMaxPads) continue;
        padManager.padSettings[idx].padIndex = idx;
        padManager.padSettings[idx].loadFromXml (*padEl);
    }

    // Sync per-pad MNFreeze atomics from freshly-loaded PadSettings so the MIDI
    // intercept loop works correctly for all pads, not just the selected one.
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        auto& mf       = padMnFreeze[i];
        const auto& ps = padManager.padSettings[i];
        mf.enabled.store(ps.mnFreezeEnabled, std::memory_order_relaxed);
        mf.note   .store(ps.midiNote,        std::memory_order_relaxed);
        mf.ch     .store(ps.midiChannel,     std::memory_order_relaxed);
        // isActive always starts false after a kit load — no freeze survives a load.
        mf.isActive.store(false, std::memory_order_relaxed);
    }

    // Update grid name labels for all pads
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        const auto& ps = padManager.padSettings[i];
        padGrid.setPadSampleName (i, ps.sampleFilePath.isEmpty()
                                       ? juce::String{}
                                       : juce::File (ps.sampleFilePath).getFileName());
    }

    if (padSelectionSource == PadSelectionSource::Global)
    {
        // A G pad is currently displayed in the SampleCard.  Loading a bank kit must NOT
        // call loadSampleFileAsync — its callAsync tail fires saveCurrentSampleState() →
        // captureSampleCardToPadSettings() → captureGlobalPadFromSampleCard(), which would
        // overwrite the G pad's settings with whichever kit pad happens to be in
        // padManager.selectedPadIndex.  Instead, preload all kit engines in the background
        // and keep the SampleCard showing the G pad unchanged.
        for (int i = 0; i < PadManager::kMaxPads; ++i)
        {
            const auto& ps = padManager.padSettings[i];
            if (!ps.sampleFilePath.isEmpty())
            {
                juce::File f (ps.sampleFilePath);
                if (f.existsAsFile())
                    preloadPadEngineAsync (i, f, ps);
            }
        }
        // Refresh the G pad display so any SampleCard controls that depend on kit state
        // (e.g. MnFreeze) are kept in sync.
        selectGlobalPad (selectedGlobalPadIndex);
    }
    else
    {
        // Load the currently selected kit pad into the UI (with all settings restored)
        const int curPad = padManager.selectedPadIndex;
        const auto& curPs = padManager.padSettings[curPad];
        if (!curPs.sampleFilePath.isEmpty())
        {
            juce::File f (curPs.sampleFilePath);
            if (f.existsAsFile())
            {
                // Reset folder navigation so Prev/Next scans the correct folder after kit load.
                currentFolder = f.getParentDirectory();
                {
                    juce::ScopedWriteLock wlock (folderLock);
                    folderAudioFiles.clear();
                }
                currentFileIndex = -1;

                sampleCard.restoreZoomAndScroll (1.0, 0.0f);
                loadSampleFileAsync (f, /*autoPlay=*/false, true, false, padSettingsToSnapshot (curPs));
            }
            else
            {
                sampleCard.setEmptyState (true);
            }
        }
        else
        {
            sampleCard.setEmptyState (true);
        }

        // Preload audio for all other pads in the background
        for (int i = 0; i < PadManager::kMaxPads; ++i)
        {
            if (i == curPad) continue;
            const auto& ps = padManager.padSettings[i];
            if (!ps.sampleFilePath.isEmpty())
            {
                juce::File f (ps.sampleFilePath);
                if (f.existsAsFile())
                    preloadPadEngineAsync (i, f, ps);
            }
        }
    }

    currentKitFile = file;
    kitIsDirty = false;

    // Sync the loaded kit into the active GJM bank slot
    {
        auto& activeB = gjmManager.banks[gjmManager.activeBank];
        activeB.kitFilePath = file.getFullPathName();
        activeB.displayName = file.getFileNameWithoutExtension();
        for (int i = 0; i < PadManager::kMaxPads; ++i)
            activeB.pads[i] = padManager.padSettings[i];
        activeB.isReady = true;
    }

    sampleCard.showTrimToast ("Kit loaded: " + file.getFileName(), false);
}

void MainComponent::newKitAction()
{
    captureSampleCardToPadSettings (padManager.selectedPadIndex);

    if (!gjmManager.isDirty && !kitIsDirty)
    {
        clearAllPadsForNewKit();
        return;
    }

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::AlertWindow::QuestionIcon)
            .withTitle ("New Session")
            .withMessage ("You have unsaved changes.\n"
                          "Save the session before starting a new one?")
            .withButton ("Save Session")
            .withButton ("Don't Save")
            .withButton ("Cancel"),
        [this] (int r)
        {
            if (r == 0 || r == 3)  // Cancel or dismissed
                return;

            if (r == 2)  // Don't Save — clear immediately
            {
                clearAllPadsForNewKit();
                return;
            }

            // r == 1 — Save first, then clear
            saveSessionAction (false);
            // clearAllPadsForNewKit() is NOT called here because saveSessionAction
            // opens an async file dialog; the user flow is: save completes → user
            // manually clicks New Session again if they still want to clear.
        });
}

void MainComponent::clearAllPadsForNewKit()
{
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        // Skip pads that have nothing loaded.
        if (padManager.getSettings(i).sampleFilePath.isEmpty() && !padManager.hasEngine(i))
            continue;

        auto& engine = padManager.getEngine (i);
        engine.muteOutput.store (true);
        engine.clearActiveSoundFlags();
        engine.forceStopAllVoices();
        engine.clearSoundsAndVoices();
        engine.allNotesOff (0, false);
        {
            juce::ScopedLock lock (engine.sampleLock);
            engine.samples.clear();
            engine.selectedSampleIndex = 0;
        }
        engine.clearPeakCache();
        engine.muteOutput.store (false);

        padManager.getSettings(i).resetToDefaults();
        padGrid.setPadSampleName (i, {});
    }

    // Reset selected pad UI.
    sampleCard.updateUIFromSettings (padManager.getSettings (padManager.selectedPadIndex));
    sampleCard.setEmptyState (true);
    currentFolder = juce::File{};
    {
        juce::ScopedWriteLock wlock (folderLock);
        folderAudioFiles.clear();
    }
    currentFileIndex = -1;

    // Clear MNFreeze state for all pads.
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        padMnFreeze[i].isActive.store (false, std::memory_order_relaxed);
        padMnFreeze[i].enabled .store (false, std::memory_order_relaxed);
    }

    // Reset session identity — start fresh untitled session
    currentKitFile = juce::File{};
    kitIsDirty = false;
    gjmManager.reset();
    updateGjmUI();

    sampleCard.showTrimToast ("New session started", false);
    printf ("[KIT] All pads cleared — new kit ready\n");
    fflush (stdout);
}

void MainComponent::navigateKit (int direction)
{
    if (!currentKitFile.existsAsFile()) return;

    const juce::File folder = currentKitFile.getParentDirectory();
    juce::Array<juce::File> kits;
    folder.findChildFiles (kits, juce::File::findFiles, false, "*.jai");
    kits.sort();

    if (kits.isEmpty()) return;

    int current = -1;
    for (int i = 0; i < kits.size(); ++i)
        if (kits[i] == currentKitFile) { current = i; break; }

    // Wrap around: if kit was renamed/moved since load, current == -1 → start at 0 or last
    int next = 0;
    if (current >= 0)
        next = (current + direction + kits.size()) % kits.size();
    else
        next = (direction > 0) ? 0 : kits.size() - 1;

    if (kits[next] == currentKitFile) return;   // only one .jai in folder

    configManager->saveLastKitFolder (folder.getFullPathName());
    loadKitFromFile (kits[next]);
}

//==============================================================================
// GJM — Global Jaiva Map implementation
//==============================================================================

void MainComponent::loadGjmFromFile (const juce::File& file)
{
    if (!gjmManager.loadManifest (file))
    {
        sampleCard.showTrimToast ("Invalid .gjm file", true);
        return;
    }
    // loadManifest() sets isUntitled=false, isDirty=false, isLoaded=true

    // Disable bank nav while the background parse job runs
    globalControlsBar.setMaxBank (GjmManager::kNumBanks);
    globalControlsBar.setBankIndex (1);
    globalControlsBar.setNavEnabled (false);
    gjmParsing.store (true);

    gjmStatusLabel.setText ("Loading session...", juce::dontSendNotification);
    gjmStatusLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xFF1A1A00));
    gjmStatusLabel.setColour (juce::Label::textColourId,       juce::Colour (0xFFCCCC00));

    // Parse all 16 bank kit files in a single background job (sequential XML reads, ~20-80ms total).
    // When done, callAsync fires finishGjmLoad() on the message thread.
    backgroundThreads.addJob ([this]()
    {
        gjmManager.parseAllBanks();   // thread-safe: only this job writes to banks[].pads
        juce::MessageManager::callAsync ([this]() { finishGjmLoad(); });
    });
}

void MainComponent::finishGjmLoad()
{
    gjmParsing.store (false);
    globalControlsBar.setNavEnabled (true);

    // Restore global loop pads from GjmManager into PadManager.
    // Done before switchGjmBank so the audio engines get pre-warmed.
    for (int g = 0; g < PadManager::kNumGlobalPads; ++g)
    {
        padManager.globalSettings[g] = gjmManager.globalPads[g];

        const auto& gs = padManager.globalSettings[g];
        if (!gs.sampleFilePath.isEmpty())
        {
            juce::File f (gs.sampleFilePath);
            if (f.existsAsFile())
            {
                globalLoopColumn.setSlotName (g, f.getFileName());
                globalLoopColumn.setSlotState (g, GlobalLoopColumn::SlotState::Loaded);
                preloadGlobalPadEngineAsync (g, f, gs);
            }
            else
            {
                globalLoopColumn.setSlotName (g, "(missing)");
                globalLoopColumn.setSlotState (g, GlobalLoopColumn::SlotState::Empty);
            }
        }
        else
        {
            globalLoopColumn.setSlotName (g, {});
            globalLoopColumn.setSlotState (g, GlobalLoopColumn::SlotState::Empty);
        }
    }

    // activeBank is already 0 from loadManifest() — do NOT set it here.
    // Setting it before switchGjmBank(0) would make activeBank == bankIdx inside
    // switchGjmBank, causing the outgoing-save guard to fire and overwrite bank 0's
    // freshly-parsed data with the empty startup padSettings.
    switchGjmBank (0);

    const int n = gjmManager.numBanksWithFiles();
    sampleCard.showTrimToast ("GJM loaded: " + gjmManager.gjmFile.getFileNameWithoutExtension()
                              + "  (" + juce::String (n) + " banks)", false);
}

void MainComponent::saveGjmToFile (const juce::File& file)
{
    captureSampleCardToPadSettings (padManager.selectedPadIndex);

    // Flush global loop pad settings from PadManager into GjmManager before writing
    for (int g = 0; g < PadManager::kNumGlobalPads; ++g)
        gjmManager.globalPads[g] = padManager.globalSettings[g];

    // Flush active bank's in-memory pads into gjmManager before writing
    {
        auto& activeB = gjmManager.banks[gjmManager.activeBank];
        for (int i = 0; i < PadManager::kMaxPads; ++i)
            activeB.pads[i] = padManager.padSettings[i];
        activeB.isReady = true;
    }

    // For each bank that has samples and no kit file path yet, auto-generate one
    for (int b = 0; b < GjmManager::kNumBanks; ++b)
    {
        auto& bank = gjmManager.banks[b];
        if (!bank.isReady) continue;

        bool hasSamples = false;
        for (auto& ps : bank.pads)
            if (!ps.sampleFilePath.isEmpty()) { hasSamples = true; break; }
        if (!hasSamples) continue;

        if (bank.kitFilePath.isEmpty())
        {
            // Use user-set display name if it's not a placeholder, otherwise auto-number
            const bool hasCustomName = bank.displayName.isNotEmpty()
                && bank.displayName != ("Bank " + juce::String (b + 1));

            juce::String kitFilename = hasCustomName
                ? (bank.displayName + "_kit.jai")
                : juce::String::formatted ("kit-%03d.jai", b + 1);

            bank.kitFilePath = file.getParentDirectory()
                                   .getChildFile (kitFilename)
                                   .getFullPathName();
            bank.displayName = juce::File (bank.kitFilePath).getFileNameWithoutExtension();
        }

        gjmManager.saveBankKit (b, file);
    }

    if (gjmManager.saveManifest (file))
    {
        gjmManager.gjmFile    = file;
        gjmManager.isUntitled = false;
        gjmManager.isDirty    = false;
        kitIsDirty = false;

        configManager->saveLastKitFolder (file.getParentDirectory().getFullPathName());
        sampleCard.showTrimToast ("Session saved: " + file.getFileName(), false);
        updateGjmUI();
    }
    else
    {
        sampleCard.showTrimToast ("Failed to save session", true);
    }
}

void MainComponent::switchGjmBank (int bankIdx)
{
    if (bankIdx < 0 || bankIdx >= GjmManager::kNumBanks) return;
    if (!gjmManager.isLoaded) return;

    // Safety fallback: if the bank somehow wasn't pre-parsed (e.g. file added after load),
    // parse it synchronously now (fast XML only, <5ms).
    gjmManager.ensureBankReady (bankIdx);

    // Save outgoing bank's current state back to gjmManager — but ONLY when actually
    // switching to a different bank. On initial load, activeBank == bankIdx == 0, and
    // skipping this prevents empty startup padSettings from overwriting the freshly-parsed
    // bank 0 data.
    if (gjmManager.activeBank != bankIdx)
    {
        // When a G pad is selected, captureSampleCardToPadSettings routes to
        // captureGlobalPadFromSampleCard which reads the SampleCard — but the
        // SampleCard may still hold stale kit-pad MIDI values from the previous
        // operation (before the G pad was displayed).  Use saveGlobalPadStateFromEngine
        // instead, which reads the engine for audio data and the SampleCard only when
        // this G pad is the one actually shown.
        if (padSelectionSource == PadSelectionSource::Bank)
        {
            captureSampleCardToPadSettings (padManager.selectedPadIndex);
        }
        else
        {
            saveGlobalPadStateFromEngine (selectedGlobalPadIndex);
            // kit pad settings in padManager.padSettings are already up-to-date
            // (captureSampleCardToPadSettings was called when the kit pad was last deselected)
        }
        auto& outgoing = gjmManager.banks[gjmManager.activeBank];
        for (int i = 0; i < PadManager::kMaxPads; ++i)
            outgoing.pads[i] = padManager.padSettings[i];
        outgoing.isReady = true;
    }

    // Swap padManager settings with the target bank's cached pads
    gjmManager.activeBank = bankIdx;
    const auto& incoming = gjmManager.banks[bankIdx];
    for (int i = 0; i < PadManager::kMaxPads; ++i)
        padManager.padSettings[i] = incoming.pads[i];

    // Sync MNFreeze atomics so MIDI intercept reflects the new bank instantly
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        auto& mf       = padMnFreeze[i];
        const auto& ps = padManager.padSettings[i];
        mf.enabled.store (ps.mnFreezeEnabled, std::memory_order_relaxed);
        mf.note   .store (ps.midiNote,        std::memory_order_relaxed);
        mf.ch     .store (ps.midiChannel,     std::memory_order_relaxed);
        mf.isActive.store (false,             std::memory_order_relaxed);
    }

    // Clear audio engines for pads that have no sample in the incoming bank.
    // Without this, MIDI events would still trigger playback from the previous bank's
    // loaded audio data — the engine holds sound even when the padSettings are swapped.
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        if (padManager.padSettings[i].sampleFilePath.isEmpty() && padManager.hasEngine (i))
        {
            auto& engine = padManager.getEngine (i);
            engine.muteOutput.store (true);
            engine.clearActiveSoundFlags();
            engine.forceStopAllVoices();
            engine.clearSoundsAndVoices();
            engine.allNotesOff (0, false);
            {
                juce::ScopedLock lock (engine.sampleLock);
                engine.samples.clear();
                engine.selectedSampleIndex = 0;
            }
            engine.clearPeakCache();
            engine.muteOutput.store (false);
        }
    }

    // Update grid name labels for all 16 pads
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        const auto& ps = padManager.padSettings[i];
        padGrid.setPadSampleName (i, ps.sampleFilePath.isEmpty()
                                       ? juce::String{}
                                       : juce::File (ps.sampleFilePath).getFileName());
    }

    const bool gPadSelected = (padSelectionSource == PadSelectionSource::Global);
    const int  savedGIdx    = gPadSelected ? selectedGlobalPadIndex : -1;

    // When a G pad is selected: re-display it immediately (before any loadSampleFileAsync
    // callAsync fires) and pre-warm all kit pad engines in the background, then return early.
    // The previous deferred-callAsync approach caused a race: loadSampleFileAsync's own
    // callAsync modified the SampleCard between the bank-switch and the deferred
    // selectGlobalPad, so the G pad display read stale SampleCard state.
    if (gPadSelected && savedGIdx >= 0)
    {
        // Pre-warm kit pad engines first (non-blocking background jobs) so MIDI
        // triggers respond instantly after the bank switch.
        for (int i = 0; i < PadManager::kMaxPads; ++i)
        {
            const auto& ps = padManager.padSettings[i];
            if (!ps.sampleFilePath.isEmpty())
            {
                juce::File f (ps.sampleFilePath);
                if (f.existsAsFile())
                    preloadPadEngineAsync (i, f, ps);
            }
        }

        // Now redisplay the G pad from clean globalSettings state.
        // globalSettings[savedGIdx] was saved by saveGlobalPadStateFromEngine above,
        // so it is authoritative and not contaminated by SampleCard display state.
        selectGlobalPad (savedGIdx);
        updateGjmUI();
        DBG ("[GJM] Switched to bank " + juce::String (bankIdx + 1) + ": " + incoming.displayName + " (G pad retained)");
        return;
    }

    // Kit pad selected path: load the selected kit pad's audio into the SampleCard.
    const int  curPad = padManager.selectedPadIndex;
    const auto& curPs = padManager.padSettings[curPad];
    if (!curPs.sampleFilePath.isEmpty())
    {
        juce::File f (curPs.sampleFilePath);
        if (f.existsAsFile())
        {
            currentFolder = f.getParentDirectory();
            { juce::ScopedWriteLock wlock (folderLock); folderAudioFiles.clear(); }
            currentFileIndex = -1;
            sampleCard.restoreZoomAndScroll (1.0, 0.0f);
            loadSampleFileAsync (f, /*autoPlay=*/false, true, false, padSettingsToSnapshot (curPs));
        }
        else
        {
            sampleCard.setEmptyState (true);
        }
    }
    else
    {
        sampleCard.setEmptyState (true);
    }

    // Pre-warm audio for all other pads in the new bank (background, non-blocking)
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        if (i == curPad) continue;
        const auto& ps = padManager.padSettings[i];
        if (!ps.sampleFilePath.isEmpty())
        {
            juce::File f (ps.sampleFilePath);
            if (f.existsAsFile())
                preloadPadEngineAsync (i, f, ps);
        }
    }

    updateGjmUI();
    DBG ("[GJM] Switched to bank " + juce::String (bankIdx + 1) + ": " + incoming.displayName);
}

void MainComponent::updateGjmUI()
{
    const int  bank1     = gjmManager.activeBank + 1;
    const bool untitled  = gjmManager.isUntitled;
    const bool dirty     = gjmManager.isDirty;

    // Kit button always labelled "Session"
    kitButton.setButtonText ("Session");

    // Session name in kitNameLabel
    const juce::String sessionName = untitled
        ? (dirty ? "Untitled*" : "New Session")
        : gjmManager.gjmFile.getFileNameWithoutExtension() + (dirty ? "*" : "");

    kitNameLabel.setText (sessionName, juce::dontSendNotification);
    kitNameLabel.setColour (juce::Label::backgroundColourId,
        untitled ? juce::Colour (0xFF222222) : juce::Colour (0xFF1A3050));
    kitNameLabel.setColour (juce::Label::textColourId,
        untitled ? juce::Colour (0xFF888888) : juce::Colour (0xFFB8DEFF));

    // GJM status label
    juce::String statusText = "Bank " + juce::String (bank1) + " / "
                            + juce::String (GjmManager::kNumBanks);
    if (!untitled)
        statusText = gjmManager.gjmFile.getFileNameWithoutExtension() + "  " + statusText;

    gjmStatusLabel.setText (statusText, juce::dontSendNotification);
    gjmStatusLabel.setColour (juce::Label::backgroundColourId,
        untitled ? juce::Colour (0xFF101820) : juce::Colour (0xFF1A3050));
    gjmStatusLabel.setColour (juce::Label::textColourId,
        untitled ? juce::Colour (0xFF3A5A7A) : juce::Colour (0xFFB8DEFF));

    // Bank nav controls
    globalControlsBar.setBankLabelText ("Bank " + juce::String (bank1));
    globalControlsBar.setBankIndex (bank1);
    globalControlsBar.setMaxBank (GjmManager::kNumBanks);
    globalControlsBar.setNavEnabled (!gjmParsing.load());
}

void MainComponent::saveSessionAction (bool forceDialog)
{
    captureSampleCardToPadSettings (padManager.selectedPadIndex);

    if (gjmManager.isUntitled || forceDialog || !gjmManager.gjmFile.existsAsFile())
    {
        // First save or "Save As" — ask the user to pick a location
        const juce::String savedFolder = configManager->getLastKitFolder();
        const juce::File startDir = savedFolder.isNotEmpty() && juce::File(savedFolder).isDirectory()
                                        ? juce::File(savedFolder)
                                        : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        gjmFileChooser = std::make_unique<juce::FileChooser> (
            "Save Session", startDir, "*.gjm");

        gjmFileChooser->launchAsync (
            juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto chosen = fc.getResult();
                if (chosen == juce::File{}) return;  // user cancelled
                if (chosen.getFileExtension().toLowerCase() != ".gjm")
                    chosen = chosen.withFileExtension ("gjm");
                configManager->saveLastKitFolder (chosen.getParentDirectory().getFullPathName());
                saveGjmToFile (chosen);
            });
    }
    else
    {
        // Named session — save directly, no dialog
        saveGjmToFile (gjmManager.gjmFile);
    }
}

void MainComponent::loadSessionAction()
{
    const juce::String savedFolder = configManager->getLastKitFolder();
    const juce::File startDir = savedFolder.isNotEmpty() && juce::File(savedFolder).isDirectory()
                                    ? juce::File(savedFolder)
                                    : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    gjmFileChooser = std::make_unique<juce::FileChooser> (
        "Load Session", startDir, "*.gjm");

    gjmFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            auto chosen = fc.getResult();
            if (chosen == juce::File{} || !chosen.existsAsFile()) return;
            configManager->saveLastKitFolder (chosen.getParentDirectory().getFullPathName());
            loadGjmFromFile (chosen);
        });
}

void MainComponent::saveBankKitAction()
{
    captureSampleCardToPadSettings (padManager.selectedPadIndex);

    // Flush into active GJM bank before saving
    auto& activeB = gjmManager.banks[gjmManager.activeBank];
    for (int i = 0; i < PadManager::kMaxPads; ++i)
        activeB.pads[i] = padManager.padSettings[i];
    activeB.isReady = true;

    const juce::String savedFolder = configManager->getLastKitFolder();
    const juce::File startDir = savedFolder.isNotEmpty() && juce::File(savedFolder).isDirectory()
                                    ? juce::File(savedFolder)
                                    : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    kitFileChooser = std::make_unique<juce::FileChooser> (
        "Save Bank Kit", startDir, "*.jai");

    kitFileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode |
        juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            auto chosen = fc.getResult();
            if (chosen == juce::File{}) return;
            if (chosen.getFileExtension().toLowerCase() != ".jai")
                chosen = chosen.withFileExtension ("jai");

            configManager->saveLastKitFolder (chosen.getParentDirectory().getFullPathName());

            // Store kit path in current bank
            gjmManager.banks[gjmManager.activeBank].kitFilePath = chosen.getFullPathName();
            gjmManager.banks[gjmManager.activeBank].displayName = chosen.getFileNameWithoutExtension();

            saveKitToFile (chosen);

            // If session is saved, also update the manifest to record the new kit path
            if (!gjmManager.isUntitled && gjmManager.gjmFile.existsAsFile())
                gjmManager.saveManifest (gjmManager.gjmFile);
        });
}

void MainComponent::loadBankKitAction()
{
    const juce::String savedFolder = configManager->getLastKitFolder();
    const juce::File startDir = savedFolder.isNotEmpty() && juce::File(savedFolder).isDirectory()
                                    ? juce::File(savedFolder)
                                    : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    kitFileChooser = std::make_unique<juce::FileChooser> (
        "Load Bank Kit", startDir, "*.jai");

    kitFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            auto chosen = fc.getResult();
            if (chosen == juce::File{} || !chosen.existsAsFile()) return;
            configManager->saveLastKitFolder (chosen.getParentDirectory().getFullPathName());

            loadKitFromFile (chosen);
            gjmManager.isDirty = false;  // loading a saved kit is not a dirty action
            updateGjmUI();
        });
}

MainComponent::TrimSettingsSnapshot MainComponent::padSettingsToSnapshot (const PadSettings& ps) const
{
    TrimSettingsSnapshot snap;
    snap.valid                    = true;
    snap.pitchCents               = ps.pitchCents;
    snap.basePitchOffset          = ps.basePitchOffset;
    snap.baseTuningHz             = ps.baseTuningHz;
    snap.pitchStepCents           = ps.pitchStepCents;
    snap.detectedNoteName         = ps.detectedNoteName;
    snap.detectedFreqHz           = ps.detectedFreqHz;
    snap.loopEnabled              = ps.loopEnabled;
    snap.oneShotEnabled           = ps.oneShotEnabled;
    snap.reverseEnabled           = ps.reverseEnabled;
    snap.bounceEnabled            = ps.bounceEnabled;
    snap.volumeLevel              = ps.volumeLevel;
    snap.normEnabled              = ps.normEnabled;
    snap.normTargetDb             = ps.normTargetDb;
    snap.adsrEnabled              = ps.adsrEnabled;
    snap.adsrAttackMs             = ps.adsrAttackMs;
    snap.adsrDecayMs              = ps.adsrDecayMs;
    snap.adsrSustain              = ps.adsrSustain;
    snap.adsrReleaseMs            = ps.adsrReleaseMs;
    snap.eqEnabled                = ps.eqEnabled;
    snap.eq1Freq  = ps.eq1Freq;  snap.eq1Gain = ps.eq1Gain; snap.eq1Q = ps.eq1Q; snap.eq1Mode = ps.eq1Mode;
    snap.eq2Freq  = ps.eq2Freq;  snap.eq2Gain = ps.eq2Gain; snap.eq2Q = ps.eq2Q; snap.eq2Mode = ps.eq2Mode;
    snap.eq3Freq  = ps.eq3Freq;  snap.eq3Gain = ps.eq3Gain; snap.eq3Q = ps.eq3Q; snap.eq3Mode = ps.eq3Mode;
    snap.padGain                   = ps.padGain;
    snap.transientDetectionEnabled = ps.transientDetectionEnabled;
    snap.transientThreshold        = ps.transientThreshold;
    snap.gridSnapEnabled           = ps.gridSnapEnabled;
    snap.gridResolutionIndex       = ps.gridResolutionIndex;
    snap.midiNote                  = ps.midiNote;
    snap.midiChannel               = ps.midiChannel;
    snap.startPointSeconds         = ps.startPointSeconds;
    snap.endPointSeconds           = ps.endPointSeconds;
    return snap;
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
                                   }
                                   else
                                   {
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

    // Step 1 — Save current sample state to disk BEFORE anything changes.
    // Must be synchronous here so the card still holds the true values.
    saveOutgoingSampleState();

    // Stop transient detection timer so it restarts cleanly after the new load.
    transientDetectionTimer.stopTimer();

    currentFileIndex = index;
    auto file = folderAudioFiles[index];

    // Step 2 — Show loading indicator immediately (before background thread starts).
    sampleCard.showLoadingState();

    // Pitch is NOT touched during Next/Prev — it stays exactly as the user left it.

    // Steps 3-7 happen inside loadSampleFileAsync on the background thread.
    // addJob() returns immediately — the message thread is NOT blocked.
    backgroundThreads.addJob([this, file, myGeneration]() {
        // Fix 1 — Stale job check: if a newer navigation has been triggered since this
        // job was queued, discard it immediately without reading any audio data.
        if (myGeneration != navigationGeneration.load())
        {
            return;
        }
        loadSampleFileAsync(file, true, false, /*deferTransients=*/true);
    });
}

void MainComponent::loadSampleFileAsync(const juce::File& file, bool autoPlay, bool resetZoom, bool deferTransients, TrimSettingsSnapshot trimSnapshot)
{
    // ── Background thread: read audio data ────────────────────────────────────────
    const juce::int64 tBgStart = (juce::int64)juce::Time::getMillisecondCounter();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr)
    {
        return;
    }


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
        delete sample;
        return;
    }
    sample->audioData = std::move(buffer);


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

    // ── Message thread: stop audio, restore state, rebuild sampler ────────────────
    juce::MessageManager::callAsync([this, sample, file, autoPlay, resetZoom, deferTransients,
                                      peaks = std::move(peaks), peakNumCh, peakNSamples, peakSR,
                                      tBgStart, trimSnapshot]() mutable {

        const juce::int64 tMsgStart = (juce::int64)juce::Time::getMillisecondCounter();

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

        // ── Step 4: Install new sample ────────────────────────────────────────────
        {
            juce::ScopedLock lock(pad().sampleLock);
            pad().samples.clear();
            pad().selectedSampleIndex = 0;
            pad().samples.add(sample);
        }

        // Cache peaks in engine immediately — needed for instant pad-switch display and
        // for the SampleCard delivery below (must happen before std::move of peaks).
        pad().storePeakCache(peaks.get(), kWaveformPeakBins, peakNumCh, peakNSamples, peakSR);

        // ── Guard: when a global pad is selected, skip all SampleCard UI updates ────
        // Audio is correctly installed in the kit-pad engine above.
        // Updating the SampleCard here would overwrite the global pad's display.
        if (padSelectionSource == PadSelectionSource::Global)
        {
            const auto& ps = padManager.padSettings[padManager.selectedPadIndex];
            sample->rootNote  = trimSnapshot.valid ? trimSnapshot.midiNote
                                                   : (ps.midiNote >= 0 ? ps.midiNote : 60);
            sample->lowNote   = sample->rootNote;
            sample->highNote  = sample->rootNote;
            sample->pitchOffset = trimSnapshot.valid
                ? (trimSnapshot.pitchCents + trimSnapshot.basePitchOffset * 100)
                : ps.pitchCents;
            sample->startPointSeconds = trimSnapshot.valid ? trimSnapshot.startPointSeconds
                                                           : ps.startPointSeconds;
            sample->endPointSeconds   = trimSnapshot.valid ? trimSnapshot.endPointSeconds
                                                           : ps.endPointSeconds;
            pad().loopEnabled.store (trimSnapshot.valid ? trimSnapshot.loopEnabled : ps.loopEnabled);
            pad().volumeGain.store  (trimSnapshot.valid ? trimSnapshot.volumeLevel  : ps.volumeLevel);
            updateSamplerSounds();
            pad().muteOutput.store(false);
            return;
        }

        // ── Step 3: Reset card to neutral state ───────────────────────────────────
        sampleCard.resetStartPoint();
        sampleCard.resetEndPoint();
        sampleCard.setLoopEnabled(false);
        sampleCard.resetFreeze();
        sampleCard.resetBounce();

        sampleCard.setSampleName(file.getFileName());
        sampleCard.setEmptyState(false);  // Clear "No sample loaded" overlay now that audio is ready

        // Deliver pre-computed peaks BEFORE setWaveform() so the very first repaint
        // uses the peak data — zero disk I/O in paint() from this point forward.
        sampleCard.setWaveformPeaks(std::move(peaks), peakNumCh, peakNSamples, peakSR);

        // Pass sample metadata already read on the background thread — this avoids
        // a second createReaderFor() call on the message thread (was the 7s freeze for MP3).
        // During navigation (deferTransients=true) skip the blocking detectTransients()
        // call — the deferred timer will run it 800ms after navigation stops.
        sampleCard.setWaveform(file, /*skipTransients=*/deferTransients,
                               /*knownTotalSamples=*/peakNSamples,
                               /*knownSampleRate=*/peakSR);
        sampleCard.setDuration(sample->lengthInSamples / sample->sampleRate);

        // ── Steps 5-7: Restore per-sample state ──────────────────────────────────
        // Start/end always reset to 0 / full-length (trim changes sample length).
        // For trim loads, all other settings come from the TrimSettingsSnapshot.
        // For normal loads, settings come from ConfigurationManager (per-sample key).
        double effectiveStart = 0.0;
        double effectiveEnd   = -1.0;   // -1 = full sample length
        // Seed from the pad's persisted volume so new samples inherit the pad level.
        // ConfigManager per-sample state and trim/kit snapshots will override this below.
        float  effectiveVol   = padManager.getSettings(padManager.selectedPadIndex).volumeLevel;
        bool   effectiveLoop  = false;


        // Helper lambda — applies ADSR/EQ/Norm state from fields (avoids code duplication)
        auto applyAdsrState = [&](bool adsrEn, float atk, float dcy, float sus, float rel)
        {
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

        auto applyPadGain = [&](float gain)
        {
            sampleCard.setEqGain(gain, /*notify=*/false);
            pad().padGain.store(juce::jlimit(0.0f, 2.0f, gain));
        };

        if (trimSnapshot.valid)
        {
            // ── TRIM LOAD / KIT LOAD: restore all settings from snapshot ──────────
            // Start/end: kit loads restore saved values; trim loads leave at defaults
            // (0.0 / -1.0) because the trimmed file's content IS the old start..end region.
            effectiveStart = trimSnapshot.startPointSeconds;
            effectiveEnd   = trimSnapshot.endPointSeconds;

            effectiveVol  = trimSnapshot.volumeLevel;
            effectiveLoop = trimSnapshot.loopEnabled;

            // MIDI routing — silent (no listener, no updateSamplerSounds rebuild)
            sampleCard.setMidiNote   (trimSnapshot.midiNote,    /*notify=*/false);
            sampleCard.setMidiChannel(trimSnapshot.midiChannel, /*notify=*/false);

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

            // Pad gain
            applyPadGain(trimSnapshot.padGain);

        }
        else if (configManager != nullptr)
        {
            // ── NORMAL LOAD: restore from per-sample config key ───────────────────
            auto state = configManager->getSampleState(file);
            if (state.exists)
            {
                effectiveStart = state.startPoint;
                effectiveEnd   = state.endPoint;
                // Volume is intentionally NOT restored from per-sample state.
                // Volume is a pad-level setting: it stays at whatever the pad knob
                // is set to, regardless of which sample file is loaded.
                effectiveLoop  = state.loopEnabled;
                sampleCard.setTransientThreshold(state.transientThreshold);
                sampleCard.setDetectedNoteName(state.detectedNoteName, state.detectedFreqHz);
                sampleCard.setBasePitchOffset(state.basePitchOffset);
                applyAdsrState(state.adsrEnabled, state.adsrAttackMs, state.adsrDecayMs,
                               state.adsrSustain, state.adsrReleaseMs);
                applyEqState(state.eqEnabled,
                             state.eq1Freq, state.eq1Gain, state.eq1Q, state.eq1Mode,
                             state.eq2Freq, state.eq2Gain, state.eq2Q, state.eq2Mode,
                             state.eq3Freq, state.eq3Gain, state.eq3Q, state.eq3Mode);
                applyNormState(state.normEnabled, state.normTargetDb);
                applyPadGain(1.0f);  // ConfigurationManager SampleState has no padGain — default to unity
            }
            else
            {
                // New file — reset all to defaults
                sampleCard.setTransientThreshold(4.0);
                sampleCard.setDetectedNoteName("", 0.0);
                sampleCard.setBasePitchOffset(0);
                applyEqState(false, 100.0f,0.0f,1.0f,2, 500.0f,0.0f,1.0f,2, 8000.0f,0.0f,1.0f,2);
                applyNormState(false, -6.0f);
                applyPadGain(1.0f);
            }
        }

        // MIDI routing: resolve rootNote AFTER all restore blocks so kit-loaded
        // and trim-preserved MIDI notes are correctly reflected.
        sample->rootNote = sampleCard.getMidiNote();
        sample->lowNote  = sample->rootNote;
        sample->highNote = sample->rootNote;

        // Pitch: total = global user offset + per-sample base (from Tune).
        // For trim loads, both were restored from the snapshot above.
        sample->pitchOffset = sampleCard.getPitchOffset() + sampleCard.getBasePitchOffset() * 100;

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
        updateSamplerSounds();

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
        }
        else
        {
            // Navigation or startup: restore previously saved zoom and scroll
            const float savedZoom   = configManager != nullptr ? configManager->getZoomLevel() : 1.0f;
            const float savedScroll = configManager != nullptr ? configManager->getZoomScrollPosition() : 0.0f;
            if (savedZoom > 1.001f)
            {
                sampleCard.restoreZoomAndScroll((double)savedZoom, savedScroll);
            }
        }

        if (deferTransients)
        {
            // Detect transients after navigation stops (800ms debounce).
            transientDetectionTimer.startTimer(800);
        }
        // No disk save — kit must be saved explicitly by the user.
        captureSampleCardToPadSettings(padManager.selectedPadIndex);

        // Part 2H — update the pad grid name for the currently selected pad.
        padManager.padSettings[padManager.selectedPadIndex].sampleFilePath =
            file.getFullPathName();
        padGrid.setPadSampleName(padManager.selectedPadIndex, file.getFileName());


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

    if (newNote >= 0)
    {
        // Valid note — update sample mapping and rebuild sounds.
        if (pad().selectedSampleIndex >= 0 && pad().selectedSampleIndex < (int)pad().samples.size())
        {
            auto* sample = pad().samples[pad().selectedSampleIndex];
            sample->rootNote = newNote;
            sample->lowNote  = newNote;
            sample->highNote = newNote;
            // LoopingSamplerSound stores the trigger noteRange — must rebuild to change it.
            updateSamplerSounds();
        }
    }
    else
    {
        // Disabled (-1) — clear sounds so no MIDI note triggers this pad.
        pad().muteOutput.store(true);
        pad().clearActiveSoundFlags();
        pad().forceStopAllVoices();
        pad().clearSoundsAndVoices();
        pad().muteOutput.store(false);
    }

    // Keep in-memory pad settings in sync — no disk write.
    padManager.padSettings[padManager.selectedPadIndex].midiNote    = newNote;
    padManager.padSettings[padManager.selectedPadIndex].midiChannel = sampleCard.getMidiChannel();

    // Sync per-pad MIDI note atomic for MNFreeze intercept.
    padMnFreeze[padManager.selectedPadIndex].note.store(newNote, std::memory_order_relaxed);
}

void MainComponent::midiChannelChanged(int newChannel)
{

    // The actual filtering happens in handleIncomingMidiMessage
    // No need to update samples, but we might want to stop currently playing notes
    // when changing channels to avoid stuck notes
    if (newChannel != sampleCard.getMidiChannel())
    {
        // Stop all notes when changing channels to avoid confusion
        pad().getSynthesiser().allNotesOff(1, false);
    }

    // Keep in-memory pad settings in sync — no disk write.
    padManager.padSettings[padManager.selectedPadIndex].midiNote    = sampleCard.getMidiNote();
    padManager.padSettings[padManager.selectedPadIndex].midiChannel = newChannel;

    // Sync per-pad channel atomic for MNFreeze intercept.
    padMnFreeze[padManager.selectedPadIndex].ch.store(newChannel, std::memory_order_relaxed);
}

void MainComponent::learningModeChanged(bool isLearning)
{
    isLearningMode = isLearning;
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

    // Keep in-memory pad settings in sync — no disk write.
    captureSampleCardToPadSettings(padManager.selectedPadIndex);

    juce::ignoreUnused(t0);
}

void MainComponent::volumeChanged(float volume)
{
    pad().volumeGain.store(volume);
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
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

    captureSampleCardToPadSettings(padManager.selectedPadIndex);

    const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
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

    captureSampleCardToPadSettings(padManager.selectedPadIndex);

    const juce::int64 elapsed = juce::Time::getMillisecondCounter() - t0;
}

void MainComponent::loopEnabledChanged(bool isLooping)
{
    pad().loopEnabled.store(isLooping);

    // Update the flag on all currently loaded sounds — no rebuild needed
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
            sound->loopEnabled.store(isLooping);

    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

void MainComponent::gridSnapChanged(bool isEnabled)
{
    if (configManager != nullptr)
        configManager->saveGridSnapEnabled(isEnabled);
}

void MainComponent::gridResolutionChanged(int index)
{
    if (configManager != nullptr)
    {
        configManager->saveGridResolutionIndex(index);
        // FIX 2: also save as milliseconds under key 'gridResolution'
        const double ms = sampleCard.getGridInterval() * 1000.0;
        configManager->saveGridResolutionMs(ms);
    }
}

void MainComponent::detectedNoteChanged(const juce::String& noteName, double freqHz)
{
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

void MainComponent::transientDetectionEnabledChanged(bool enabled)
{
    if (configManager != nullptr)
        configManager->saveTransientDetectionEnabled(enabled);
}

void MainComponent::pitchStepCentsChanged(int cents)
{
    if (configManager != nullptr)
        configManager->savePitchStepCents(cents);
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
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

void MainComponent::activeTabChanged(int tabIndex)
{
    if (configManager != nullptr)
        configManager->saveActiveTab(tabIndex);
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

    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

void MainComponent::eqFilterModesChanged(int mode1, int mode2, int mode3)
{
    const auto t0 = juce::Time::getMillisecondCounter();

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

    // FIX 7: Do NOT call resetEqState() here — same data race issue as eqParamsChanged.
    // Filter state decays naturally through the biquad equation.

    // NO updateSamplerSounds() — coefficients written atomically above.

    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

// computeNormGainFromAudio is defined in PadAudioEngine.h and forwarded
// via the inline wrapper in MainComponent.h.

void MainComponent::normChanged(bool enabled, float targetDb)
{
    if (!enabled)
    {
        pad().normGain.store(1.0f);
        sampleCard.setNormGainDisplay(0.0f);
        captureSampleCardToPadSettings(padManager.selectedPadIndex);
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
        const float gain   = pad().computeNormGainFromAudio(targetDb);
        pad().normGain.store(gain);
        const float gainDb = (gain > 0.0f) ? 20.0f * std::log10f(gain) : 0.0f;
        sampleCard.setNormGainDisplay(gainDb);
        captureSampleCardToPadSettings(padManager.selectedPadIndex);
    }
    else
    {
        // Large sample — run peak scan on background thread.
        sampleCard.setNormGainDisplay(0.0f);

        const float capturedTarget = targetDb;
        backgroundThreads.addJob([this, capturedTarget]()
        {
            const float gain   = pad().computeNormGainFromAudio(capturedTarget);
            const float gainDb = (gain > 0.0f) ? 20.0f * std::log10f(gain) : 0.0f;

            juce::MessageManager::callAsync([this, gain, gainDb]()
            {
                pad().normGain.store(gain);
                sampleCard.setNormGainDisplay(gainDb);
                captureSampleCardToPadSettings(padManager.selectedPadIndex);
            });
        });
    }
}

void MainComponent::padGainChanged(float gain)
{
    const float g = juce::jlimit(0.0f, 2.0f, gain);
    pad().padGain.store(g);
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
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

}

void MainComponent::bounceEnabledChanged(bool enabled)
{
    // Propagate to all live sounds atomically — no rebuild needed.
    for (int i = 0; i < pad().getSynthesiser().getNumSounds(); ++i)
        if (auto* sound = dynamic_cast<LoopingSamplerSound*>(pad().getSynthesiser().getSound(i).get()))
            sound->bounceEnabled.store(enabled);

    if (configManager != nullptr)
        configManager->saveBounceEnabled(enabled);

}


void MainComponent::mnFreezeEnabledChanged(bool enabled)
{
    const int pi = padManager.selectedPadIndex;
    auto& mf = padMnFreeze[pi];

    // Sync per-pad atomics so the MIDI callback thread can read them lock-free.
    mf.enabled.store(enabled, std::memory_order_relaxed);
    mf.note.store(sampleCard.getMidiNote(),    std::memory_order_relaxed);
    mf.ch  .store(sampleCard.getMidiChannel(), std::memory_order_relaxed);

    // If mode is being turned OFF while this pad's freeze is active, kill it.
    if (!enabled && mf.isActive.load(std::memory_order_relaxed))
    {
        mf.isActive.store(false, std::memory_order_relaxed);
        activateMnFreezeForPad(pi, false, 0.0f);
    }

    captureSampleCardToPadSettings(pi);
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
    }
}

//==============================================================================
// Navigation optimizations — deferred save and transient detection

void MainComponent::flushNavigationSave()
{
    // In-memory only — capture current state so it's ready for kit save.
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

void MainComponent::runDeferredTransientDetection()
{
    // Opt 3: Called once, 800ms after the last Prev/Next press.
    // Runs detectTransients() on the message thread (file I/O) for the sample
    // that is now current.  At this point the user has stopped navigating so
    // a brief message-thread stall is acceptable.
    sampleCard.runTransientDetection();
}

//==============================================================================
void MainComponent::performPanicReset()
{

    // Flash bright red for 200ms then restore dark red
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFFF0000));
    resetButton.repaint();
    juce::Timer::callAfterDelay(200, [this]
    {
        resetButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF8B0000));
        resetButton.repaint();
    });

    // 0. Stop any active recording, waiting state, standalone metro, pattern playback
    recIsActive.store        (false, std::memory_order_relaxed);
    recWaitForBeat.store     (false, std::memory_order_relaxed);
    recMetronomeOn.store     (false, std::memory_order_relaxed);
    metronomeStandalone.store(false, std::memory_order_relaxed);
    recSongBeatPos.store     (0.0,   std::memory_order_relaxed);
    recLastBeat = -1;
    sampleCard.setRecordingActive (false);
    patternPlayTimer.stopTimer();
    recPatternPlaying = false;

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
    snap.padGain = sampleCard.getEqGain();
    snap.transientDetectionEnabled = sampleCard.isTransientDetectionEnabled();
    snap.transientThreshold        = (float)sampleCard.getTransientThreshold();
    snap.gridSnapEnabled           = sampleCard.isGridSnapEnabled();
    snap.gridResolutionIndex       = sampleCard.getGridResolutionIndex();
    snap.midiNote                  = sampleCard.getMidiNote();
    snap.midiChannel               = sampleCard.getMidiChannel();


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

//==============================================================================
// Recording engine implementations

void MainComponent::beginRecording (double bpm, double quantInBeats,
                                    bool metronomeOn, int targetPadIndex, bool overdub)
{
    // Safety: recording only targets kit pads 1-16 (indices 0-15).
    // G pad recording (indices 16+) is not supported.
    if (targetPadIndex < 0 || targetPadIndex >= 16)
    {
        sampleCard.showTrimToast ("Recording targets kit pads 1-16 only", true);
        return;
    }

    // If the target pad already has a sample, go through the same drop-pad dialog
    // flow before starting the recording.  The user can delete, rename, or keep the
    // existing file; recording starts only if the pad is actually dropped.
    if (!padManager.getSettings (targetPadIndex).sampleFilePath.isEmpty())
    {
        dropTargetPadWithCallback (targetPadIndex,
            [this, bpm, quantInBeats, metronomeOn, targetPadIndex, overdub]()
            {
                proceedWithRecording (bpm, quantInBeats, metronomeOn, targetPadIndex, overdub);
            });
        return;
    }

    proceedWithRecording (bpm, quantInBeats, metronomeOn, targetPadIndex, overdub);
}

void MainComponent::proceedWithRecording (double bpm, double quantInBeats,
                                          bool metronomeOn, int targetPadIndex, bool overdub)
{
    printf ("[REC] beginRecording  bpm=%.1f  quant=%.4f  metro=%d  target=Pad%d\n",
            bpm, quantInBeats, (int)metronomeOn, targetPadIndex + 1);

    // Stop any running pattern playback / previous capture
    patternPlayTimer.stopTimer();
    recPatternPlaying = false;
    if (liveRenderActive.load (std::memory_order_relaxed))
    {
        liveRenderActive.store (false, std::memory_order_seq_cst);
        auto* old = liveRenderWriter.exchange (nullptr, std::memory_order_seq_cst);
        backgroundThreads.addJob ([old]() { delete old; });
    }

    // Store message-thread params before arming audio thread
    recTargetPad    = targetPadIndex;
    recQuantInBeats = quantInBeats;
    overdubMode     = overdub;
    recBpmAtomic.store (bpm, std::memory_order_relaxed);

    if (overdub && !recQuantised.empty())
        recEvents = recQuantised;
    else
        recEvents.clear();

    recSongBeatPos.store (0.0, std::memory_order_relaxed);
    recLastBeat       = -1;
    recMetroBeepLeft  = 0;
    recMetroBeepPhase = 0.0;

    // ── Save snapshot of target pad settings ──────────────────────────────────
    captureSampleCardToPadSettings (padManager.selectedPadIndex);
    liveTargetSnap = padSettingsToSnapshot (padManager.getSettings (targetPadIndex));
    liveTargetSnap.startPointSeconds = 0.0;
    liveTargetSnap.endPointSeconds   = -1.0;
    liveRenderTargetPad = targetPadIndex;

    // ── Choose output file path ───────────────────────────────────────────────
    {
        juce::File outDir;
        if (currentKitFile.getFullPathName().isNotEmpty())
            outDir = currentKitFile.getParentDirectory();
        else
        {
            for (int i = 0; i < PadManager::kMaxPads; ++i)
            {
                const auto& ps = padManager.getSettings (i);
                if (ps.sampleFilePath.isNotEmpty())
                {
                    outDir = juce::File (ps.sampleFilePath).getParentDirectory();
                    break;
                }
            }
        }
        if (outDir == juce::File{} || !outDir.isDirectory())
            outDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

        for (int suffix = 1; suffix < 1000; ++suffix)
        {
            liveRenderOutputFile = outDir.getChildFile (
                juce::String::formatted ("recording-%03d.wav", suffix));
            if (!liveRenderOutputFile.existsAsFile()) break;
        }
    }

    // ── Open WAV writer — audio thread will stream into it ───────────────────
    const double outSR = juce::jmax (44100.0, pad().getSampleRate());
    juce::WavAudioFormat wavFmt;
    auto* outStream = liveRenderOutputFile.createOutputStream().release();
    if (outStream == nullptr)
    {
        printf ("[REC] Could not open output file: %s\n",
                liveRenderOutputFile.getFullPathName().toRawUTF8());
        return;
    }
    auto* rawWriter = wavFmt.createWriterFor (outStream, outSR, 2u, 24, {}, 0);
    if (rawWriter == nullptr) { delete outStream; return; }

    if (!wavWriterThread.isThreadRunning())
        wavWriterThread.startThread (juce::Thread::Priority::background);

    auto* threadedWriter = new juce::AudioFormatWriter::ThreadedWriter (
        rawWriter, wavWriterThread, 65536);

    liveRenderWriter.store (threadedWriter, std::memory_order_release);
    liveRenderActive.store (true,           std::memory_order_release);

    printf ("[REC] Capture started — output: %s  SR=%.0f\n",
            liveRenderOutputFile.getFileName().toRawUTF8(), outSR);

    // Arm metronome
    recMetronomeOn.store (metronomeOn, std::memory_order_relaxed);
    recBeatSampleOffset.store (0, std::memory_order_relaxed);

    // If metronome is already running, wait for the next beat boundary before arming
    // event recording.  The WAV capture is already open so the pre-roll is captured
    // and later skipped via liveTargetSnap.startPointSeconds.
    if (metronomeOn && (metronomeStandalone.load (std::memory_order_relaxed)
                        || recSongBeatPos.load (std::memory_order_relaxed) > 0.0))
    {
        printf ("[REC] Metro running — waiting for next beat to arm recording\n");
        recWaitForBeat.store (true, std::memory_order_seq_cst);
        sampleCard.setRecStatus ("Waiting for beat...");
    }
    else
    {
        recIsActive.store (true, std::memory_order_seq_cst);
        sampleCard.setRecordingActive (true, targetPadIndex);
    }
}

void MainComponent::endRecording()
{
    printf ("[REC] endRecording — %d raw events captured\n", (int)recEvents.size());

    recIsActive.store    (false, std::memory_order_relaxed);
    recWaitForBeat.store (false, std::memory_order_relaxed);

    // Keep standalone metronome running after recording stops.
    if (!metronomeStandalone.load (std::memory_order_relaxed))
        recMetronomeOn.store (false, std::memory_order_relaxed);

    sampleCard.setRecordingActive (false);
    quantiseRecordedEvents();

    // Stop capture and finalize — audio quantization applied if events were recorded
    finalizeLiveRender();
}

void MainComponent::metronomeStandaloneChanged (bool on, double bpm)
{
    printf ("[METRO] Standalone metronome: %s  BPM=%.1f\n", on ? "ON" : "OFF", bpm);
    metronomeStandalone.store (on, std::memory_order_relaxed);
    recBpmAtomic.store (bpm, std::memory_order_relaxed);
    recMetronomeOn.store (on, std::memory_order_relaxed);

    if (!on)
    {
        // Stop the beat clock only if not actively recording
        if (!recIsActive.load (std::memory_order_relaxed))
        {
            recSongBeatPos.store (0.0, std::memory_order_relaxed);
            recLastBeat = -1;
        }
    }
}

void MainComponent::metronomeVolumeChanged (float vol)
{
    metronomeVolume.store (juce::jlimit (0.0f, 1.0f, vol), std::memory_order_relaxed);
}

void MainComponent::playbackQuantisedEvents()
{
    printf ("[REC] playbackQuantisedEvents — %d events\n", (int)recQuantised.size());
    fflush (stdout);

    if (recQuantised.empty())
    {
        printf ("[REC] Pattern is empty — nothing to play\n");
        fflush (stdout);
        return;
    }

    recPatternPlaying   = true;
    recPatternStartMs   = juce::Time::currentTimeMillis();
    recPatternIdx       = 0;
    patternPlayTimer.startTimer (10);  // 10ms resolution
}

void MainComponent::quantiseRecordedEvents()
{
    recQuantised.clear();
    if (recEvents.empty()) return;
    if (recQuantInBeats <= 0.0) return;  // "None" — skip quantization entirely

    std::sort (recEvents.begin(), recEvents.end(),
               [](const RecordedEvent& a, const RecordedEvent& b) { return a.beatTime < b.beatTime; });

    const double grid = recQuantInBeats;
    for (const auto& ev : recEvents)
    {
        const double snapped = std::round (ev.beatTime / grid) * grid;

        const bool duplicate = std::any_of (recQuantised.begin(), recQuantised.end(),
            [&](const RecordedEvent& q)
            { return q.padIndex == ev.padIndex && std::abs (q.beatTime - snapped) < 0.001; });

        if (!duplicate)
            recQuantised.push_back ({ ev.padIndex, snapped });
    }

    std::sort (recQuantised.begin(), recQuantised.end(),
               [](const RecordedEvent& a, const RecordedEvent& b) { return a.beatTime < b.beatTime; });

    printf ("[REC] Quantised %d raw events -> %d unique events\n",
            (int)recEvents.size(), (int)recQuantised.size());
    for (const auto& ev : recQuantised)
        printf ("[REC]   pad=%d  beat=%.3f\n", ev.padIndex + 1, ev.beatTime);
    fflush (stdout);
}


void MainComponent::finalizeLiveRender()
{
    // Tell audio thread to stop writing immediately
    liveRenderActive.store (false, std::memory_order_seq_cst);
    auto* writerToClose = liveRenderWriter.exchange (nullptr, std::memory_order_seq_cst);

    // Capture quantization data by value so the background job can use them safely
    const juce::File rawFile      = liveRenderOutputFile;
    const int        targetPad    = liveRenderTargetPad;
    auto             targetSnap   = liveTargetSnap;
    const double     bpm          = recBpmAtomic.load (std::memory_order_relaxed);
    const double     quantInBeats = recQuantInBeats;

    // Pre-roll offset: WAV samples captured before beat 0 (non-zero when quantized
    // start was used).  Set as startPointSeconds so the pad skips the silence.
    const int64_t prerollSamples = recBeatSampleOffset.load (std::memory_order_relaxed);
    const double  outSR          = juce::jmax (44100.0, pad().getSampleRate());
    if (prerollSamples > 0)
    {
        targetSnap.startPointSeconds = (double)prerollSamples / outSR;
        printf ("[REC] Pre-roll: %lld samples = %.3f s — skipping via startPoint\n",
                (long long)prerollSamples, targetSnap.startPointSeconds);
    }

    // Build 1:1 quantized positions from the raw events (no dedup — audio quantization
    // needs each raw event to map directly to its snapped counterpart).
    // When quantInBeats == 0.0 ("None"), skip this entirely and load the raw file.
    auto rawEvents   = std::make_shared<std::vector<RecordedEvent>> (recEvents);
    auto quantEvents = std::make_shared<std::vector<RecordedEvent>>();
    if (quantInBeats > 0.0)
    {
        quantEvents->reserve (rawEvents->size());
        for (const auto& ev : *rawEvents)
            quantEvents->push_back ({ ev.padIndex,
                                      std::round (ev.beatTime / quantInBeats) * quantInBeats });
    }

    // Wait 80ms so the audio thread can finish any in-flight write() call,
    // then close the writer and (optionally) quantize on a background thread.
    juce::Timer::callAfterDelay (80, [this, writerToClose, rawFile, targetPad,
                                      targetSnap, bpm, quantInBeats,
                                      rawEvents, quantEvents]()
    {
        backgroundThreads.addJob ([this, writerToClose, rawFile, targetPad,
                                   targetSnap, bpm, quantInBeats,
                                   rawEvents, quantEvents]()
        {
            // 1. Flush + close the raw WAV capture
            delete writerToClose;

            if (!rawFile.existsAsFile() || rawFile.getSize() < 200)
            {
                juce::MessageManager::callAsync ([this]()
                {
                    sampleCard.showTrimToast ("Record failed — no audio captured", false);
                });
                return;
            }

            // 2. Apply audio quantization if we have events to work with
            juce::File finalFile = rawFile;

            const bool hasEvents = !rawEvents->empty();
            const bool needsShift = [&]() -> bool {
                if (!hasEvents || quantEvents->empty()) return false;
                if (quantInBeats <= 0.0) return false;  // "None" — always use raw
                const double bps = bpm / 60.0;
                for (size_t i = 0; i < rawEvents->size(); ++i)
                {
                    const double diff = std::abs ((*quantEvents)[i].beatTime
                                                  - (*rawEvents)[i].beatTime);
                    if (diff * bps > 0.0015)  // > ~66 samples @ 44.1kHz
                        return true;
                }
                return false;
            }();

            if (needsShift)
            {
                printf ("[REC] Applying audio quantization — %d events  bpm=%.1f  grid=%.4f\n",
                        (int)rawEvents->size(), bpm, quantInBeats);

                // Read raw WAV into a buffer
                std::unique_ptr<juce::AudioFormatReader> reader (
                    formatManager.createReaderFor (rawFile));

                if (reader != nullptr)
                {
                    const int   totalSamples = (int)reader->lengthInSamples;
                    const int   numCh        = (int)reader->numChannels;
                    const double sr          = reader->sampleRate;
                    const double bps         = bpm / 60.0;
                    const int    kFade       = juce::jmin (512, (int)(0.008 * sr));

                    juce::AudioBuffer<float> raw (numCh, totalSamples);
                    reader->read (&raw, 0, totalSamples, 0, true, true);
                    reader.reset();

                    juce::AudioBuffer<float> output (numCh, totalSamples);
                    output.clear();

                    const int n = (int)juce::jmin (rawEvents->size(), quantEvents->size());
                    for (int i = 0; i < n; ++i)
                    {
                        const int rawStart  = (int)((*rawEvents)[i].beatTime  / bps * sr);
                        const int quantStart = (int)((*quantEvents)[i].beatTime / bps * sr);
                        const int rawEnd    = (i < n - 1)
                            ? (int)((*rawEvents)[i + 1].beatTime / bps * sr)
                            : totalSamples;
                        const int sliceLen  = juce::jmax (0, rawEnd - rawStart);

                        for (int s = 0; s < sliceLen; ++s)
                        {
                            const int inPos  = rawStart  + s;
                            const int outPos = quantStart + s;
                            if (inPos < 0 || inPos >= totalSamples) continue;
                            if (outPos < 0 || outPos >= totalSamples) continue;
                            const float fade = (s < kFade) ? (float)s / (float)kFade : 1.0f;
                            for (int ch = 0; ch < numCh; ++ch)
                                output.getWritePointer (ch)[outPos] =
                                    raw.getReadPointer (ch)[inPos] * fade;
                        }
                    }

                    // Write quantized WAV alongside the raw one
                    juce::File qFile = rawFile.getSiblingFile (
                        rawFile.getFileNameWithoutExtension() + "-q.wav");

                    juce::WavAudioFormat wavFmt;
                    auto* oStream = qFile.createOutputStream().release();
                    auto* writer  = (oStream != nullptr)
                        ? wavFmt.createWriterFor (oStream, sr, (unsigned int)numCh, 24, {}, 0)
                        : nullptr;

                    if (writer != nullptr)
                    {
                        std::unique_ptr<juce::AudioFormatWriter> wr (writer);
                        wr->writeFromAudioSampleBuffer (output, 0, totalSamples);
                        wr.reset();
                        finalFile = qFile;
                        printf ("[REC] Quantized WAV: %s\n",
                                qFile.getFileName().toRawUTF8());
                    }
                    else if (oStream != nullptr)
                    {
                        delete oStream;
                        printf ("[REC] Could not create quantized WAV writer\n");
                    }
                }
            }

            // 3. Load result to target pad on message thread
            juce::MessageManager::callAsync ([this, finalFile, targetPad,
                                              targetSnap]()
            {
                // Safety: only kit pads 0-15 are valid recording targets.
                if (targetPad < 0 || targetPad >= 16)
                {
                    printf ("[REC] Invalid target pad %d — aborting finalize\n", targetPad);
                    return;
                }

                captureSampleCardToPadSettings (padManager.selectedPadIndex);
                saveOutgoingSampleState();

                // Switch engine and visual grid selection to the target bank pad.
                padSelectionSource = PadSelectionSource::Bank;
                globalLoopColumn.clearSelection();
                padManager.selectPad (targetPad);
                padGrid.selectPadQuiet (targetPad);

                // Reset folder navigation so Prev/Next browses the recorded file's folder.
                currentFolder = finalFile.getParentDirectory();
                {
                    juce::ScopedWriteLock wlock (folderLock);
                    folderAudioFiles.clear();
                }
                currentFileIndex = -1;

                sampleCard.restoreZoomAndScroll (1.0, 0.0f);
                loadSampleFileAsync (finalFile, /*autoPlay=*/true, /*resetZoom=*/true,
                                     /*deferTransients=*/false, targetSnap);

                padGrid.setPadSampleName (targetPad, finalFile.getFileName());
                sampleCard.showTrimToast ("Recorded -> " + finalFile.getFileName(), false);
            });
        });
    });
}

void MainComponent::tickPatternPlayback()
{
    if (!recPatternPlaying || recQuantised.empty())
    {
        patternPlayTimer.stopTimer();
        recPatternPlaying = false;
        return;
    }

    const double elapsedSec   = (double)(juce::Time::currentTimeMillis() - recPatternStartMs) / 1000.0;
    const double elapsedBeats = elapsedSec * (recBpmAtomic.load() / 60.0);

    while (recPatternIdx < (int)recQuantised.size())
    {
        const double evBeat = recQuantised[recPatternIdx].beatTime;
        if (evBeat > elapsedBeats) break;

        const int padIdx = recQuantised[recPatternIdx].padIndex;
        if (padManager.hasEngine (padIdx))
        {
            const int note = padManager.getSettings (padIdx).midiNote;
            padManager.getEngine (padIdx).getSynthesiser().noteOn (1, note, 0.8f);
            juce::Timer::callAfterDelay (500, [this, padIdx, note]()
            {
                if (padManager.hasEngine (padIdx))
                    padManager.getEngine (padIdx).getSynthesiser().noteOff (1, note, 0.0f, true);
            });
            printf ("[REC] Pattern: pad=%d  beat=%.3f\n", padIdx + 1, evBeat);
            fflush (stdout);
        }
        ++recPatternIdx;
    }

    if (recPatternIdx >= (int)recQuantised.size())
    {
        patternPlayTimer.stopTimer();
        recPatternPlaying = false;
        printf ("[REC] Pattern playback complete\n");
    }
}

//==============================================================================
// Pattern persistence Listener overrides

void MainComponent::savePattern()
{
    saveCurrentPattern();
}

void MainComponent::loadPattern()
{
    loadPatternFromPad();
}

void MainComponent::clearPattern()
{
    recEvents.clear();
    recQuantised.clear();
    sampleCard.setRecStatus ("Pattern cleared");
    printf ("[REC] Pattern cleared\n");
    fflush (stdout);
}

void MainComponent::executeDropPad(int padIdx)
{
    // Flush current pad state before any changes.
    captureSampleCardToPadSettings (padManager.selectedPadIndex);

    // Stop and clear the target engine.
    auto& engine = padManager.getEngine (padIdx);
    engine.muteOutput.store (true);
    engine.clearActiveSoundFlags();
    engine.forceStopAllVoices();
    engine.clearSoundsAndVoices();
    engine.allNotesOff (0, false);
    {
        juce::ScopedLock lock (engine.sampleLock);
        engine.samples.clear();
        engine.selectedSampleIndex = 0;
    }
    engine.clearPeakCache();
    engine.muteOutput.store (false);

    // Reset sample-data fields to factory defaults so a subsequent sample load
    // doesn't inherit stale start/end/pitch/ADSR/EQ from the old pad.
    // MIDI note/channel are NOT reset — see PadSettings::resetToDefaults().
    auto& settings = padManager.getSettings (padIdx);
    settings.resetToDefaults();

    // Update the pad grid label.
    padGrid.setPadSampleName (padIdx, {});

    // If the target pad is currently selected, reset the SampleCard UI to defaults too.
    if (padIdx == padManager.selectedPadIndex)
    {
        sampleCard.updateUIFromSettings (settings);
        sampleCard.setEmptyState (true);
        currentFolder = juce::File{};
        {
            juce::ScopedWriteLock wlock (folderLock);
            folderAudioFiles.clear();
        }
        currentFileIndex = -1;
    }

    sampleCard.showTrimToast ("Pad " + juce::String (padIdx + 1) + " cleared", false);
    printf ("[DROP] Pad %d cleared — MIDI note %d / ch %d preserved\n",
            padIdx + 1, settings.midiNote, settings.midiChannel);
    fflush (stdout);
}

void MainComponent::dropTargetPadWithCallback (int padIndex, std::function<void()> onDropComplete)
{
    // Stop any active recording first.
    if (recIsActive.load (std::memory_order_relaxed))
    {
        recIsActive.store    (false, std::memory_order_relaxed);
        recMetronomeOn.store (false, std::memory_order_relaxed);
        sampleCard.setRecordingActive (false);
    }

    const int padIdx = padIndex;
    auto& settings   = padManager.getSettings (padIdx);

    if (settings.sampleFilePath.isEmpty())
    {
        sampleCard.showTrimToast ("Pad " + juce::String (padIdx + 1) + " is already empty", false);
        return;
    }

    const juce::File sampleFile (settings.sampleFilePath);
    const bool fileOnDisk = sampleFile.existsAsFile();

    // Helper: find the next available auto-name "rec-pattern-NNN.wav" in dir.
    auto findNextAutoName = [](const juce::File& dir) -> juce::File
    {
        for (int n = 1; n <= 999; ++n)
        {
            juce::File candidate = dir.getChildFile (
                "rec-pattern-" + juce::String (n).paddedLeft ('0', 3) + ".wav");
            if (!candidate.existsAsFile())
                return candidate;
        }
        return {};
    };

    // Helper: build a safe custom name, avoiding collisions.
    auto buildCustomName = [](const juce::File& dir, const juce::String& base) -> juce::File
    {
        juce::File f = dir.getChildFile (base + "-rec-pattern.wav");
        if (!f.existsAsFile()) return f;
        for (int n = 2; n <= 999; ++n)
        {
            f = dir.getChildFile (base + "-rec-pattern-" + juce::String (n) + ".wav");
            if (!f.existsAsFile()) return f;
        }
        return {};
    };

    // ── Dialog 1: Drop Pad / Delete / Keep / Cancel ───────────────────────────
    auto* d1 = new juce::AlertWindow (
        "Drop Pad: " + sampleFile.getFileName(),
        "What do you want to do with the sample file?",
        juce::AlertWindow::QuestionIcon);
    d1->addButton ("Drop Pad",   3);   // clear pad immediately, file untouched
    d1->addButton ("Delete file", 1);
    d1->addButton ("Keep file",   2);
    d1->addButton ("Cancel",      0);

    d1->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [this, padIdx, sampleFile, fileOnDisk, findNextAutoName, buildCustomName,
             onDropComplete](int r1)
            {
                if (r1 == 0)   // Cancel — leave pad unchanged
                    return;

                if (r1 == 3)   // Drop Pad only — file left completely untouched on disk
                {
                    executeDropPad (padIdx);
                    if (onDropComplete) onDropComplete();
                    return;
                }

                if (r1 == 1)   // Delete file from disk, then drop
                {
                    if (fileOnDisk)
                        sampleFile.deleteFile();
                    executeDropPad (padIdx);
                    if (onDropComplete) onDropComplete();
                    return;
                }

                // Keep file (r1 == 2) -> ask about renaming
                const juce::File dir = sampleFile.getParentDirectory();

                // ── Dialog 2: Rename? ────────────────────────────────────────
                auto* d2 = new juce::AlertWindow (
                    "Keep File",
                    "Do you want to rename the file?\n\n"
                    "Yes  -> enter a custom name (saved as <name>-rec-pattern.wav)\n"
                    "No   -> auto-name as rec-pattern-001.wav",
                    juce::AlertWindow::QuestionIcon);
                d2->addButton ("Yes - rename",   1);
                d2->addButton ("No - auto-name", 2);
                d2->addButton ("Cancel",         0);

                d2->enterModalState (true,
                    juce::ModalCallbackFunction::create (
                        [this, padIdx, sampleFile, dir, fileOnDisk,
                         findNextAutoName, buildCustomName, onDropComplete](int r2)
                        {
                            if (r2 == 0)  // Cancel — abort entirely
                                return;

                            // After a successful rename: offer reload (normal context) or just
                            // drop (recording context — no "Reload pad" option offered).
                            auto askReloadOrDrop = [this, padIdx, onDropComplete](const juce::File& dest)
                            {
                                const bool recordingCtx = (bool)onDropComplete;
                                auto* d = new juce::AlertWindow (
                                    "File Renamed",
                                    "Renamed to: " + dest.getFileName() + "\n\n"
                                    + (recordingCtx
                                       ? "Drop the pad and start recording?"
                                       : "Do you want to reload the pad with the renamed file,\n"
                                         "or drop the pad?"),
                                    juce::AlertWindow::QuestionIcon);
                                if (!recordingCtx)
                                    d->addButton ("Reload pad", 1);
                                d->addButton (recordingCtx ? "Drop and record" : "Drop pad", 2);
                                d->addButton ("Cancel", 0);
                                d->enterModalState (true,
                                    juce::ModalCallbackFunction::create (
                                        [this, padIdx, dest, onDropComplete](int rd)
                                        {
                                            if (rd == 0)  // Cancel
                                                return;

                                            if (rd == 1)  // Reload pad (normal context only)
                                            {
                                                if (padIdx != padManager.selectedPadIndex)
                                                {
                                                    captureSampleCardToPadSettings (padManager.selectedPadIndex);
                                                    padManager.selectPad (padIdx);
                                                    padGrid.selectPadQuiet (padIdx);
                                                }
                                                currentFolder = dest.getParentDirectory();
                                                {
                                                    juce::ScopedWriteLock wlock (folderLock);
                                                    folderAudioFiles.clear();
                                                }
                                                currentFileIndex = -1;
                                                loadSampleFileAsync (dest, true, false);
                                            }
                                            else  // Drop pad (+ fire recording callback if set)
                                            {
                                                executeDropPad (padIdx);
                                                if (onDropComplete) onDropComplete();
                                            }
                                        }),
                                    true);
                            };

                            if (r2 == 2)  // Auto-name
                            {
                                if (fileOnDisk)
                                {
                                    juce::File dest = findNextAutoName (dir);
                                    if (dest != juce::File{} && sampleFile.moveFileTo (dest))
                                    {
                                        askReloadOrDrop (dest);
                                        return;
                                    }
                                }
                                executeDropPad (padIdx);
                                if (onDropComplete) onDropComplete();
                                return;
                            }

                            // Yes — custom name (r2 == 1)
                            // ── Dialog 3: Enter name ────────────────────────
                            auto* d3 = new juce::AlertWindow (
                                "Rename File",
                                "Enter a name (file will be saved as <name>-rec-pattern.wav):",
                                juce::AlertWindow::NoIcon);
                            d3->addTextEditor ("name",
                                sampleFile.getFileNameWithoutExtension(), "Name:");
                            d3->addButton ("OK",     1,
                                juce::KeyPress (juce::KeyPress::returnKey));
                            d3->addButton ("Cancel", 0,
                                juce::KeyPress (juce::KeyPress::escapeKey));

                            d3->enterModalState (true,
                                juce::ModalCallbackFunction::create (
                                    [this, padIdx, d3, sampleFile, dir,
                                     fileOnDisk, findNextAutoName, buildCustomName,
                                     askReloadOrDrop, onDropComplete](int r3)
                                    {
                                        juce::String customName;
                                        if (r3 == 1)
                                            customName = d3->getTextEditorContents ("name").trim();
                                        delete d3;

                                        if (r3 == 0)  // Cancel — abort entirely
                                            return;

                                        if (fileOnDisk)
                                        {
                                            juce::File dest;
                                            if (customName.isNotEmpty())
                                                dest = buildCustomName (dir, customName);
                                            else
                                                dest = findNextAutoName (dir);  // fallback

                                            if (dest != juce::File{} && sampleFile.moveFileTo (dest))
                                            {
                                                askReloadOrDrop (dest);
                                                return;
                                            }
                                        }
                                        executeDropPad (padIdx);
                                        if (onDropComplete) onDropComplete();
                                    }),
                                true);
                        }),
                    true);
            }),
        true);
}

void MainComponent::dropTargetPad (int padIndex)
{
    dropTargetPadWithCallback (padIndex, {});
}

//==============================================================================
// Pattern persistence helpers

void MainComponent::saveCurrentPattern()
{
    if (recQuantised.empty())
    {
        sampleCard.setRecStatus ("Nothing to save — record first");
        return;
    }

    // Prompt for a name using a modal AlertWindow
    auto* dialog = new juce::AlertWindow ("Save Pattern",
                                          "Enter a name for this pattern:",
                                          juce::AlertWindow::NoIcon);
    dialog->addTextEditor ("name", "Pattern 1", "Name:");
    dialog->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // Capture everything by value — dialog lives until the callback fires.
    const int targetPad = recTargetPad;
    std::vector<RecordedEvent> snap = recQuantised;

    dialog->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, dialog, targetPad, snap](int result)
        {
            if (result == 1)
            {
                juce::String name = dialog->getTextEditorContents ("name").trim();
                if (name.isEmpty()) name = "Pattern";

                // Convert RecordedEvent → PatternEvent and store in PadSettings
                std::vector<PatternEvent> events;
                events.reserve (snap.size());
                for (auto& ev : snap)
                    events.push_back ({ ev.padIndex, ev.beatTime });

                padManager.getSettings (targetPad).addPattern (name, events);

                // Persist to the currently loaded kit file if one is active
                if (currentKitFile.existsAsFile())
                    saveKitToFile (currentKitFile);

                sampleCard.setRecStatus ("Saved: " + name
                    + " (" + juce::String ((int)events.size()) + " events)");
                printf ("[REC] Pattern saved: '%s'  target pad=%d\n", name.toRawUTF8(), targetPad + 1);
                fflush (stdout);
            }
            delete dialog;
        }), true);
}

void MainComponent::loadPatternFromPad()
{
    const auto& settings = padManager.getSettings (recTargetPad);
    const juce::StringArray names = settings.getPatternNames();

    if (names.isEmpty())
    {
        sampleCard.setRecStatus ("No patterns saved for pad " + juce::String (recTargetPad));
        return;
    }

    juce::PopupMenu menu;
    for (int i = 0; i < names.size(); ++i)
        menu.addItem (i + 1, names[i]);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sampleCard),
        [this, names](int result)
        {
            if (result > 0 && result <= names.size())
            {
                const juce::String name = names[result - 1];
                const auto events = padManager.getSettings (recTargetPad).getPattern (name);

                recQuantised.clear();
                recQuantised.reserve (events.size());
                for (auto& e : events)
                    recQuantised.push_back ({ e.padIndex, e.beatTime });

                sampleCard.setRecStatus ("Loaded: " + name
                    + " (" + juce::String ((int)recQuantised.size()) + " events)");
                printf ("[REC] Pattern loaded: '%s'\n", name.toRawUTF8());
                fflush (stdout);
            }
        });
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
    }
}

void MainComponent::activateMnFreezeForPad(int padIdx, bool nowActive, float velocity)
{
    // Message thread only. Activates or deactivates MNFreeze on an arbitrary pad engine
    // so freeze loops continue running regardless of which pad is selected in the UI.
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (!padManager.hasEngine(padIdx)) return;

    auto& engine = padManager.getEngine(padIdx);
    auto& mf     = padMnFreeze[padIdx];
    const bool isSelected = (padIdx == padManager.selectedPadIndex);

    if (nowActive)
    {
        // Save pre-freeze state (message thread — no race).
        mf.preVol  = engine.volumeGain.load();
        mf.preLoop = padManager.getSettings(padIdx).loopEnabled;

        // Scale volume by velocity × pad knob level.
        const float knobVol = padManager.getSettings(padIdx).volumeLevel;
        engine.volumeGain.store(juce::jlimit(0.0f, 1.0f, velocity * knobVol));

        // Enable freeze+loop on all sounds in this engine.
        for (int i = 0; i < engine.getSynthesiser().getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(engine.getSynthesiser().getSound(i).get()))
            {
                snd->freezeActive.store(true);
                snd->loopEnabled.store(true);
            }

        // Inject phantom noteOn if no voice is currently playing.
        if (!engine.isAnyVoiceActive() && !engine.samples.isEmpty() && engine.samples[0]->isValid())
        {
            const int noteToPlay = mf.note.load(std::memory_order_relaxed);
            engine.getSynthesiser().noteOn(1, noteToPlay, 0.8f);
        }

        padGrid.setMidiFreezeLocked(padIdx, true);

        // If the selected pad is the one being frozen, sync the sampleCard UI.
        if (isSelected)
        {
            sampleCard.setLoopButtonStateQuiet(true);
            sampleCard.setFreezeButtonStateQuiet(true);
        }
    }
    else
    {
        // Step 1: Silence audio thread immediately.
        engine.muteOutput.store(true);

        // Step 2: Clear freezeActive so stopNote() works normally.
        for (int i = 0; i < engine.getSynthesiser().getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(engine.getSynthesiser().getSound(i).get()))
                snd->freezeActive.store(false);

        // Step 3: Force-stop all voices.
        engine.forceStopAllVoices();

        // Step 4: Belt-and-suspenders JUCE voice-state reset.
        engine.getSynthesiser().allNotesOff(0, false);

        // Step 5: Restore loop state that was active before freeze.
        const bool loopOn = mf.preLoop;
        engine.loopEnabled.store(loopOn);
        for (int i = 0; i < engine.getSynthesiser().getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*>(engine.getSynthesiser().getSound(i).get()))
                snd->loopEnabled.store(loopOn);

        // Step 6: Restore volume.
        engine.volumeGain.store(mf.preVol);

        // Step 7: Resume audio output.
        engine.muteOutput.store(false);

        padGrid.setMidiFreezeLocked(padIdx, false);

        // If selected, sync sampleCard UI back to saved loop state.
        if (isSelected)
        {
            sampleCard.setLoopButtonStateQuiet(loopOn);
            sampleCard.setFreezeButtonStateQuiet(false);
        }
    }
}

void MainComponent::handleMidiLearn(int noteNumber)
{
    // Runs on the message thread (dispatched via callAsync in handleIncomingMidiMessage).
    // ALL work here is message-thread-safe.
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    if (!isLearningMode)
        return;

    // setMidiNoteFromLearn:
    //   1. Updates the card note display
    //   2. Fires midiNoteChanged listener → updates sample->rootNote + calls updateSamplerSounds()
    //      (exactly one rebuild — do NOT call updateSamplerSounds() again below)
    //   3. Deactivates learn mode and fires learningModeChanged(false)
    sampleCard.setMidiNoteFromLearn(noteNumber);

    // updateSamplerSounds() was already called inside midiNoteChanged listener above.
    // Pad settings are captured inside midiNoteChanged.
}

//==============================================================================
// Session persistence methods
void MainComponent::loadLastSession()
{
    // App starts fresh — no samples or kit loaded automatically.
    // Only audio and MIDI device selections are restored (system-level settings).

    // ── Restore MIDI device ────────────────────────────────────────────────────────
    juce::String savedDevice = configManager->getMidiDevice();
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
                    midiInput->start();
                break;
            }
        }
    }

    // Start with an empty untitled in-memory session — 16 banks, no files, no disk writes.
    gjmManager.reset();
    globalControlsBar.setBankIndex (1);
    globalControlsBar.setMaxBank (GjmManager::kNumBanks);
    updateGjmUI();

    padGrid.selectPad(0);
}

void MainComponent::saveOutgoingSampleState()
{
    // In-memory only — capture outgoing pad state so pad switching is instant.
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

void MainComponent::saveCurrentSampleState()
{
    // In-memory only — no disk writes during runtime.
    // State is only written to disk when the user explicitly saves a kit.
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
    kitIsDirty          = true;
    gjmManager.isDirty  = true;
    updateGjmUI();
}

void MainComponent::selectGlobalPad (int slotIdx)
{
    if (slotIdx < 0 || slotIdx >= PadManager::kNumGlobalPads) return;

    // Save outgoing state before switching source.
    captureSampleCardToPadSettings (padManager.selectedPadIndex);

    // Deselect current kit pad — restore to Loaded/Empty color without firing onPadSelected.
    padGrid.clearSelection();

    padSelectionSource      = PadSelectionSource::Global;
    selectedGlobalPadIndex  = slotIdx;
    globalLoopColumn.selectSlot (slotIdx);

    auto& gs = padManager.globalSettings[slotIdx];

    // Show in SampleCard — read from the global engine if audio is loaded.
    auto& engine = padManager.getGlobalEngine (slotIdx);

    // Always update MIDI/settings display from globalSettings — even for empty engines.
    // Without this, MIDI labels from the previously selected kit pad persist in the SampleCard.
    sampleCard.updateUIFromSettings (gs);
    baseTuningLabel.setHz (gs.baseTuningHz);

    if (engine.hasSampleLoaded())
    {
        sampleCard.setEmptyState (false);

        sampleCard.setWaveformFileOnly (juce::File (gs.sampleFilePath),
                                        engine.getPeakNumSamples(),
                                        engine.getPeakSampleRate());
        sampleCard.setAudioPeaksFromBuffer (engine.getPeakBins(),
                                             engine.getPeakNumBins(),
                                             engine.getPeakNumCh(),
                                             engine.getPeakNumSamples(),
                                             engine.getPeakSampleRate());
        sampleCard.setStartPoint (gs.startPointSeconds);
        if (gs.endPointSeconds > 0.0)
            sampleCard.setEndPoint (gs.endPointSeconds);
        sampleCard.restoreZoomAndScroll (gs.zoomLevel, gs.zoomScrollPosition);

        // Push EQ to engine
        engine.eqActive.store (gs.eqEnabled);
        engine.eqFilterModes[0] = gs.eq1Mode;
        engine.eqFilterModes[1] = gs.eq2Mode;
        engine.eqFilterModes[2] = gs.eq3Mode;
        engine.padGain.store (juce::jlimit (0.0f, 2.0f, gs.padGain));
        const double sr = engine.getSampleRate() > 0.0 ? engine.getSampleRate() : 44100.0;
        PadAudioEngine::EqCoeffDoubleBuffer::Coeffs newCoeffs[3];
        newCoeffs[0] = engine.computeEqCoeffs (gs.eq1Freq, gs.eq1Gain, gs.eq1Q, gs.eq1Mode, sr);
        newCoeffs[1] = engine.computeEqCoeffs (gs.eq2Freq, gs.eq2Gain, gs.eq2Q, gs.eq2Mode, sr);
        newCoeffs[2] = engine.computeEqCoeffs (gs.eq3Freq, gs.eq3Gain, gs.eq3Q, gs.eq3Mode, sr);
        engine.eqCoeffDB.writeFromUI (newCoeffs);
    }
    else
    {
        sampleCard.setEmptyState (true);
    }

    // Switch Rec tab to G-pad transfer mode.
    sampleCard.setRecGlobalPadMode(true, slotIdx);
}

void MainComponent::toggleGlobalPadPlayback (int slotIdx)
{
    if (slotIdx < 0 || slotIdx >= PadManager::kNumGlobalPads) return;

    auto& gs     = padManager.globalSettings[slotIdx];
    auto& engine = padManager.getGlobalEngine (slotIdx);

    if (!engine.hasSampleLoaded()) return;

    if (globalPadPlaying[slotIdx])
    {
        // Stop — MNFreeze-style: mute → clear freeze → force-stop → restore loop state.
        globalPadPlaying[slotIdx] = false;
        engine.muteOutput.store (true);

        for (int i = 0; i < engine.getSynthesiser().getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*> (engine.getSynthesiser().getSound (i).get()))
                snd->freezeActive.store (false);

        engine.forceStopAllVoices();
        engine.allNotesOff (0, false);

        // Restore the loop state that was configured in the pad settings.
        const bool loopOn = gs.loopEnabled;
        engine.loopEnabled.store (loopOn);
        for (int i = 0; i < engine.getSynthesiser().getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*> (engine.getSynthesiser().getSound (i).get()))
                snd->loopEnabled.store (loopOn);

        engine.muteOutput.store (false);
        globalLoopColumn.setSlotPlaying (slotIdx, false);

        // Sync SampleCard loop/freeze buttons when this G pad is currently displayed.
        if (padSelectionSource == PadSelectionSource::Global && selectedGlobalPadIndex == slotIdx)
        {
            sampleCard.setLoopButtonStateQuiet (gs.loopEnabled);
            sampleCard.setFreezeButtonStateQuiet (false);
        }
    }
    else
    {
        // Start — MNFreeze-style: set freeze+loop on sounds, inject noteOn if silent.
        globalPadPlaying[slotIdx] = true;

        for (int i = 0; i < engine.getSynthesiser().getNumSounds(); ++i)
            if (auto* snd = dynamic_cast<LoopingSamplerSound*> (engine.getSynthesiser().getSound (i).get()))
            {
                snd->freezeActive.store (true);
                snd->loopEnabled.store (true);
            }
        engine.loopEnabled.store (true);

        // Inject a phantom noteOn if no voice is currently active.
        // MUST use the sample's rootNote, not gs.midiNote — the LoopingSamplerSound's
        // noteRange has only the rootNote bit set, so a noteOn on any other number
        // finds no matching sound and produces silence.
        if (!engine.isAnyVoiceActive())
        {
            int note = 60;
            {
                juce::ScopedLock lock (engine.sampleLock);
                if (engine.selectedSampleIndex >= 0 &&
                    engine.selectedSampleIndex < engine.samples.size())
                    note = juce::jlimit (0, 127,
                        engine.samples[engine.selectedSampleIndex]->rootNote);
            }
            engine.getSynthesiser().noteOn (1, note, 0.8f);
        }

        globalLoopColumn.setSlotPlaying (slotIdx, true);

        // Sync SampleCard loop/freeze buttons when this G pad is currently displayed.
        if (padSelectionSource == PadSelectionSource::Global && selectedGlobalPadIndex == slotIdx)
        {
            sampleCard.setLoopButtonStateQuiet (true);
            sampleCard.setFreezeButtonStateQuiet (true);
        }
    }
}

void MainComponent::captureGlobalPadFromSampleCard (int slotIdx)
{
    if (slotIdx < 0 || slotIdx >= PadManager::kNumGlobalPads) return;
    auto& gs     = padManager.globalSettings[slotIdx];
    auto& engine = padManager.getGlobalEngine (slotIdx);

    // Sample file path
    {
        juce::ScopedLock lock (engine.sampleLock);
        if (engine.selectedSampleIndex >= 0 &&
            engine.selectedSampleIndex < engine.samples.size())
        {
            gs.sampleFilePath = engine.samples[engine.selectedSampleIndex]->file.getFullPathName();
        }
    }

    gs.pitchCents      = sampleCard.getPitchOffset();
    gs.basePitchOffset = sampleCard.getBasePitchOffset();
    gs.baseTuningHz    = sampleCard.getBaseTuningHz();

    gs.loopEnabled    = sampleCard.isLoopEnabled();
    gs.oneShotEnabled = sampleCard.isOneShotEnabled();
    gs.reverseEnabled = sampleCard.isReverseEnabled();
    gs.bounceEnabled  = sampleCard.isBounceEnabled();

    gs.volumeLevel  = sampleCard.getVolume();
    gs.normEnabled  = sampleCard.isNormEnabled();
    gs.normTargetDb = sampleCard.getNormTargetDb();

    gs.adsrEnabled   = sampleCard.isAdsrEnabled();
    gs.adsrAttackMs  = sampleCard.getAdsrAttackMs();
    gs.adsrDecayMs   = sampleCard.getAdsrDecayMs();
    gs.adsrSustain   = sampleCard.getAdsrSustain();
    gs.adsrReleaseMs = sampleCard.getAdsrReleaseMs();

    gs.padGain   = sampleCard.getEqGain();
    gs.eqEnabled = sampleCard.isEqEnabled();
    gs.eq1Freq   = sampleCard.getEqBandFreq(0); gs.eq1Gain = sampleCard.getEqBandGain(0);
    gs.eq1Q      = sampleCard.getEqBandQ(0);    gs.eq1Mode = engine.eqFilterModes[0];
    gs.eq2Freq   = sampleCard.getEqBandFreq(1); gs.eq2Gain = sampleCard.getEqBandGain(1);
    gs.eq2Q      = sampleCard.getEqBandQ(1);    gs.eq2Mode = engine.eqFilterModes[1];
    gs.eq3Freq   = sampleCard.getEqBandFreq(2); gs.eq3Gain = sampleCard.getEqBandGain(2);
    gs.eq3Q      = sampleCard.getEqBandQ(2);    gs.eq3Mode = engine.eqFilterModes[2];

    // Markers
    {
        juce::ScopedLock lock (engine.sampleLock);
        if (engine.selectedSampleIndex >= 0 &&
            engine.selectedSampleIndex < engine.samples.size())
        {
            gs.startPointSeconds = engine.samples[engine.selectedSampleIndex]->startPointSeconds;
            gs.endPointSeconds   = engine.samples[engine.selectedSampleIndex]->endPointSeconds;
        }
    }

    gs.midiNote    = sampleCard.getMidiNote();
    gs.midiChannel = sampleCard.getMidiChannel();
    gs.activeTab   = sampleCard.getActiveTabIndex();

    // Update column label
    globalLoopColumn.setSlotName (slotIdx,
        gs.sampleFilePath.isEmpty()
            ? juce::String{}
            : juce::File (gs.sampleFilePath).getFileName());
}

void MainComponent::saveGlobalPadStateFromEngine (int slotIdx)
{
    // Saves globalSettings[slotIdx] in a way that is safe to call even when a different
    // pad is displayed in the SampleCard.
    //
    // The key difference from captureGlobalPadFromSampleCard:
    //   - Audio state (sample path, markers) is always read from the engine — authoritative.
    //   - SampleCard state (pitch, ADSR, EQ, MIDI, etc.) is read ONLY when this G pad is
    //     the one currently shown.  When a different pad is shown, the existing values in
    //     globalSettings[slotIdx] are left untouched — they were set correctly the last
    //     time this G pad was displayed.
    if (slotIdx < 0 || slotIdx >= PadManager::kNumGlobalPads) return;

    auto& gs     = padManager.globalSettings[slotIdx];
    auto& engine = padManager.getGlobalEngine (slotIdx);

    // Always read audio / sample path from the engine — it is authoritative.
    {
        juce::ScopedLock lock (engine.sampleLock);
        if (engine.selectedSampleIndex >= 0 &&
            engine.selectedSampleIndex < engine.samples.size())
        {
            gs.sampleFilePath    = engine.samples[engine.selectedSampleIndex]->file.getFullPathName();
            gs.startPointSeconds = engine.samples[engine.selectedSampleIndex]->startPointSeconds;
            gs.endPointSeconds   = engine.samples[engine.selectedSampleIndex]->endPointSeconds;
        }
    }

    // Read SampleCard values only when this G pad is the one currently displayed.
    // If a kit pad or a different G pad is shown, the SampleCard holds stale/foreign data
    // and reading it would corrupt this G pad's settings (particularly midiNote/midiChannel).
    if (padSelectionSource == PadSelectionSource::Global && selectedGlobalPadIndex == slotIdx)
    {
        captureGlobalPadFromSampleCard (slotIdx);
    }
}

void MainComponent::updateGlobalPadVisuals()
{
    for (int g = 0; g < PadManager::kNumGlobalPads; ++g)
    {
        const auto& gs = padManager.globalSettings[g];
        if (gs.sampleFilePath.isEmpty())
        {
            globalLoopColumn.setSlotName  (g, {});
            globalLoopColumn.setSlotState (g, GlobalLoopColumn::SlotState::Empty);
        }
        else
        {
            globalLoopColumn.setSlotName  (g, juce::File(gs.sampleFilePath).getFileName());
            globalLoopColumn.setSlotPlaying (g, globalPadPlaying[g]);
        }
    }
}

void MainComponent::transferKitPadToGlobal(int kitPadIdx, int globalPadIdx)
{
    if (kitPadIdx < 0 || kitPadIdx >= PadManager::kMaxPads) return;
    if (globalPadIdx < 0 || globalPadIdx >= PadManager::kNumGlobalPads) return;

    auto& srcEngine   = padManager.getEngine(kitPadIdx);
    auto& dstEngine   = padManager.getGlobalEngine(globalPadIdx);
    auto& srcSettings = padManager.padSettings[kitPadIdx];
    auto& dstSettings = padManager.globalSettings[globalPadIdx];

    // Read source sample under lock.
    std::shared_ptr<juce::AudioBuffer<float>> srcAudio;
    double srcSampleRate  = 0.0;
    int    srcNumChannels = 0;
    int    srcRootNote    = 60;
    double srcStartSec    = 0.0, srcEndSec = -1.0;
    juce::File srcFile;
    {
        juce::ScopedLock lock(srcEngine.sampleLock);
        if (srcEngine.samples.size() == 0 || srcEngine.selectedSampleIndex < 0)
        {
            juce::MessageManager::callAsync([this, kitPadIdx] {
                sampleCard.showTrimToast("Kit Pad " + juce::String(kitPadIdx + 1) + " has no sample", true);
            });
            return;
        }
        const auto* smp = srcEngine.samples[srcEngine.selectedSampleIndex];
        srcAudio       = smp->audioData;
        srcSampleRate  = smp->sampleRate;
        srcNumChannels = smp->numChannels;
        srcRootNote    = smp->rootNote;
        srcStartSec    = smp->startPointSeconds;
        srcEndSec      = smp->endPointSeconds;
        srcFile        = smp->file;
    }

    if (!srcAudio || srcAudio->getNumSamples() == 0)
    {
        sampleCard.showTrimToast("Kit Pad " + juce::String(kitPadIdx + 1) + " has no audio", true);
        return;
    }

    // Stop any playing loop on the destination global pad.
    if (globalPadPlaying[globalPadIdx])
    {
        globalPadPlaying[globalPadIdx] = false;
        dstEngine.muteOutput.store(true);
        dstEngine.clearActiveSoundFlags();
        dstEngine.forceStopAllVoices();
        dstEngine.allNotesOff(0, false);
        dstEngine.muteOutput.store(false);
        globalLoopColumn.setSlotPlaying(globalPadIdx, false);
    }

    // Copy playback settings from source kit pad, but preserve the global pad's own
    // MIDI assignment. Global pads have independent MIDI routing; inheriting the kit
    // pad's midiNote would cause them to intercept that note away from the kit pad.
    const int savedMidiNote    = dstSettings.midiNote;
    const int savedMidiChannel = dstSettings.midiChannel;
    dstSettings              = srcSettings;
    dstSettings.padIndex     = PadManager::kMaxPads + globalPadIdx;
    dstSettings.sampleFilePath = srcFile.getFullPathName();
    dstSettings.midiNote    = savedMidiNote;
    dstSettings.midiChannel = savedMidiChannel;

    // Install copied sample in destination engine.
    dstEngine.muteOutput.store(true);
    dstEngine.clearActiveSoundFlags();
    dstEngine.forceStopAllVoices();
    dstEngine.clearSoundsAndVoices();

    {
        juce::ScopedLock lock(dstEngine.sampleLock);
        dstEngine.samples.clear();
        dstEngine.selectedSampleIndex = -1;

        auto* copy          = new PadAudioEngine::MappedSample();
        copy->file          = srcFile;
        copy->name          = srcFile.getFileNameWithoutExtension();
        copy->rootNote      = srcRootNote;
        copy->lowNote       = srcRootNote;
        copy->highNote      = srcRootNote;
        copy->audioData     = srcAudio;           // shared_ptr — safe, read-only
        copy->sampleRate    = srcSampleRate;
        copy->numChannels   = srcNumChannels;
        copy->lengthInSamples = srcAudio->getNumSamples();
        copy->startPointSeconds = srcStartSec;
        copy->endPointSeconds   = srcEndSec;
        dstEngine.samples.add(copy);
        dstEngine.selectedSampleIndex = 0;
    }

    // Build the LoopingSamplerSound for the destination engine.
    {
        juce::ScopedLock lock(dstEngine.sampleLock);
        const auto* smp = dstEngine.samples[0];

        class DummyAudioReader : public juce::AudioFormatReader
        {
        public:
            DummyAudioReader(double sr, unsigned int ch)
                : juce::AudioFormatReader(nullptr, "Dummy")
            { sampleRate = sr; numChannels = ch; lengthInSamples = 1; bitsPerSample = 32; usesFloatingPointData = true; }
            bool readSamples(int* const* dest, int numDest, int startOff,
                             juce::int64, int num) override
            {
                for (int ch = 0; ch < numDest; ++ch)
                    if (dest[ch]) memset(reinterpret_cast<float*>(dest[ch]) + startOff, 0, (size_t)num * sizeof(float));
                return true;
            }
        };
        DummyAudioReader dummyReader(smp->sampleRate, (unsigned int)smp->numChannels);

        const juce::int64 bufTotal  = (juce::int64)smp->audioData->getNumSamples();
        juce::int64 startSmp = 0;
        juce::int64 endSmp   = juce::jmax((juce::int64)1, bufTotal - 1);
        if (smp->startPointSeconds > 0.0 && smp->sampleRate > 0)
            startSmp = juce::jlimit((juce::int64)0, endSmp - 1, (juce::int64)(smp->startPointSeconds * smp->sampleRate));
        if (smp->endPointSeconds > 0.0 && smp->sampleRate > 0)
            endSmp = juce::jlimit(startSmp + 1, bufTotal - 1, (juce::int64)(smp->endPointSeconds * smp->sampleRate));

        juce::BigInteger noteRange;
        noteRange.setRange(0, 128, false);
        noteRange.setBit(juce::jlimit(0, 127, smp->rootNote));

        auto* sound = new LoopingSamplerSound(
            smp->name, dummyReader, noteRange,
            smp->rootNote, smp->attack, smp->release, 10.0);
        sound->fullAudioData = smp->audioData;
        sound->startSampleAtomic.store(startSmp);
        sound->endSampleAtomic.store(endSmp);
        sound->loopEnabled.store(dstSettings.loopEnabled);
        sound->pitchOffsetAtomic.store(dstSettings.pitchCents);
        sound->oneShotEnabled.store(dstSettings.oneShotEnabled);
        sound->reverseEnabled.store(dstSettings.reverseEnabled);
        sound->bounceEnabled.store(dstSettings.bounceEnabled);
        sound->customAdsrEnabled.store(dstSettings.adsrEnabled);
        sound->customAdsrAttackMs.store(dstSettings.adsrAttackMs);
        sound->customAdsrDecayMs.store(dstSettings.adsrDecayMs);
        sound->customAdsrSustain.store(dstSettings.adsrSustain);
        sound->customAdsrReleaseMs.store(dstSettings.adsrReleaseMs);
        dstEngine.getSynthesiser().addSound(sound);
    }
    dstEngine.muteOutput.store(false);

    // Mirror peak cache so waveform shows without disk I/O.
    dstEngine.storePeakCache(srcEngine.getPeakBins(),
                              srcEngine.getPeakNumBins(),
                              srcEngine.getPeakNumCh(),
                              srcEngine.getPeakNumSamples(),
                              srcEngine.getPeakSampleRate());

    // Update G pad column visual.
    globalLoopColumn.setSlotName(globalPadIdx, srcFile.getFileName());

    // If this G pad is currently selected, refresh the SampleCard waveform.
    if (padSelectionSource == PadSelectionSource::Global
        && selectedGlobalPadIndex == globalPadIdx)
    {
        sampleCard.setEmptyState(false);
        sampleCard.updateUIFromSettings(dstSettings);
        sampleCard.setWaveformFileOnly(srcFile,
                                        dstEngine.getPeakNumSamples(),
                                        dstEngine.getPeakSampleRate());
        sampleCard.setAudioPeaksFromBuffer(dstEngine.getPeakBins(),
                                            dstEngine.getPeakNumBins(),
                                            dstEngine.getPeakNumCh(),
                                            dstEngine.getPeakNumSamples(),
                                            dstEngine.getPeakSampleRate());
        sampleCard.setStartPoint(srcStartSec);
        if (srcEndSec > 0.0) sampleCard.setEndPoint(srcEndSec);
    }

    sampleCard.showTrimToast("Transferred Pad " + juce::String(kitPadIdx + 1)
                              + " -> G" + juce::String(globalPadIdx + 1));

    // Drop the source kit pad — its sample now lives in the G pad engine.
    // This mirrors switchGjmBank's empty-pad clearing logic.
    srcEngine.muteOutput.store (true);
    srcEngine.clearActiveSoundFlags();
    srcEngine.forceStopAllVoices();
    srcEngine.clearSoundsAndVoices();
    srcEngine.allNotesOff (0, false);
    {
        juce::ScopedLock lock (srcEngine.sampleLock);
        srcEngine.samples.clear();
        srcEngine.selectedSampleIndex = 0;
    }
    srcEngine.clearPeakCache();
    srcEngine.muteOutput.store (false);

    padManager.padSettings[kitPadIdx].sampleFilePath = {};
    padGrid.setPadSampleName (kitPadIdx, {});

    // If this kit pad is currently displayed in the SampleCard, show empty state.
    if (padSelectionSource == PadSelectionSource::Bank
        && padManager.selectedPadIndex == kitPadIdx)
    {
        sampleCard.setEmptyState (true);
    }
}

void MainComponent::dropGlobalPad(int globalPadIdx)
{
    if (globalPadIdx < 0 || globalPadIdx >= PadManager::kNumGlobalPads) return;

    auto& engine = padManager.getGlobalEngine(globalPadIdx);

    // Stop playback if running.
    if (globalPadPlaying[globalPadIdx])
    {
        globalPadPlaying[globalPadIdx] = false;
        engine.muteOutput.store(true);
        engine.clearActiveSoundFlags();
        engine.forceStopAllVoices();
        engine.allNotesOff(0, false);
        engine.muteOutput.store(false);
        globalLoopColumn.setSlotPlaying(globalPadIdx, false);
    }

    // Clear audio engine.
    engine.muteOutput.store(true);
    engine.clearActiveSoundFlags();
    engine.forceStopAllVoices();
    engine.clearSoundsAndVoices();
    {
        juce::ScopedLock lock(engine.sampleLock);
        engine.samples.clear();
        engine.selectedSampleIndex = -1;
    }
    engine.clearPeakCache();
    engine.muteOutput.store(false);

    // Reset settings — disable MIDI routing so the empty G pad does not
    // accidentally fire on the default midiNote=60 from PadSettings{}.
    padManager.globalSettings[globalPadIdx] = PadSettings{};
    padManager.globalSettings[globalPadIdx].padIndex     = PadManager::kMaxPads + globalPadIdx;
    padManager.globalSettings[globalPadIdx].midiNote     = -1;
    padManager.globalSettings[globalPadIdx].midiChannel  = -1;

    // Update column visual.
    globalLoopColumn.setSlotName (globalPadIdx, {});
    globalLoopColumn.setSlotState(globalPadIdx, GlobalLoopColumn::SlotState::Empty);

    // If this G pad is currently selected, show empty state.
    if (padSelectionSource == PadSelectionSource::Global
        && selectedGlobalPadIndex == globalPadIdx)
    {
        sampleCard.setEmptyState(true);
    }
}

void MainComponent::captureSampleCardToPadSettings(int padIdx)
{
    // If a global loop pad is currently selected in the SampleCard, capture to that instead.
    if (padSelectionSource == PadSelectionSource::Global)
    {
        if (selectedGlobalPadIndex >= 0 && selectedGlobalPadIndex < PadManager::kNumGlobalPads)
            captureGlobalPadFromSampleCard (selectedGlobalPadIndex);
        return;
    }

    if (padIdx < 0 || padIdx >= PadManager::kMaxPads) return;
    auto& ps = padManager.getSettings(padIdx);
    auto& engine = padManager.getEngine(padIdx);

    // Sample file path
    {
        juce::ScopedLock lock(engine.sampleLock);
        if (engine.selectedSampleIndex >= 0 &&
            engine.selectedSampleIndex < engine.samples.size())
        {
            ps.sampleFilePath = engine.samples[engine.selectedSampleIndex]->file.getFullPathName();
        }
    }

    // Pitch
    ps.pitchCents      = sampleCard.getPitchOffset();
    ps.basePitchOffset = sampleCard.getBasePitchOffset();
    ps.baseTuningHz    = sampleCard.getBaseTuningHz();
    ps.pitchStepCents  = 100; // getGridInterval()-style — use existing SampleCard if needed

    // Detected note (may be empty for unpitched material)
    ps.detectedNoteName = sampleCard.getDetectedNoteName();
    ps.detectedFreqHz   = sampleCard.getDetectedFreqHz();

    // Playback modes
    ps.loopEnabled    = sampleCard.isLoopEnabled();
    ps.oneShotEnabled = sampleCard.isOneShotEnabled();
    ps.reverseEnabled = sampleCard.isReverseEnabled();
    ps.bounceEnabled  = sampleCard.isBounceEnabled();
    ps.mnFreezeEnabled = sampleCard.isMnFreezeEnabled();

    // Volume + normalize
    ps.volumeLevel  = sampleCard.getVolume();
    ps.normEnabled  = sampleCard.isNormEnabled();
    ps.normTargetDb = sampleCard.getNormTargetDb();

    // ADSR
    ps.adsrEnabled   = sampleCard.isAdsrEnabled();
    ps.adsrAttackMs  = sampleCard.getAdsrAttackMs();
    ps.adsrDecayMs   = sampleCard.getAdsrDecayMs();
    ps.adsrSustain   = sampleCard.getAdsrSustain();
    ps.adsrReleaseMs = sampleCard.getAdsrReleaseMs();

    // EQ
    ps.padGain   = sampleCard.getEqGain();
    ps.eqEnabled = sampleCard.isEqEnabled();
    ps.eq1Freq   = sampleCard.getEqBandFreq(0); ps.eq1Gain = sampleCard.getEqBandGain(0);
    ps.eq1Q      = sampleCard.getEqBandQ(0);    ps.eq1Mode = engine.eqFilterModes[0];
    ps.eq2Freq   = sampleCard.getEqBandFreq(1); ps.eq2Gain = sampleCard.getEqBandGain(1);
    ps.eq2Q      = sampleCard.getEqBandQ(1);    ps.eq2Mode = engine.eqFilterModes[1];
    ps.eq3Freq   = sampleCard.getEqBandFreq(2); ps.eq3Gain = sampleCard.getEqBandGain(2);
    ps.eq3Q      = sampleCard.getEqBandQ(2);    ps.eq3Mode = engine.eqFilterModes[2];

    // Markers
    {
        juce::ScopedLock lock(engine.sampleLock);
        if (engine.selectedSampleIndex >= 0 &&
            engine.selectedSampleIndex < engine.samples.size())
        {
            ps.startPointSeconds = engine.samples[engine.selectedSampleIndex]->startPointSeconds;
            ps.endPointSeconds   = engine.samples[engine.selectedSampleIndex]->endPointSeconds;
        }
    }

    // Transient detection
    ps.transientDetectionEnabled = sampleCard.isTransientDetectionEnabled();
    ps.transientThreshold        = (float)sampleCard.getTransientThreshold();

    // Grid snap
    ps.gridSnapEnabled     = sampleCard.isGridSnapEnabled();
    ps.gridResolutionIndex = sampleCard.getGridResolutionIndex();

    // MIDI routing
    ps.midiNote    = sampleCard.getMidiNote();
    ps.midiChannel = sampleCard.getMidiChannel();

    // Active tab
    ps.activeTab = sampleCard.getActiveTabIndex();
}

void MainComponent::saveCurrentSession()
{
    // No-op — runtime state is only written to disk when the user saves a kit explicitly.
    captureSampleCardToPadSettings(padManager.selectedPadIndex);
}

bool MainComponent::hasAnySamplesLoaded() const
{
    // Check the active pad first.
    {
        juce::ScopedLock lock(pad().sampleLock);
        if (!pad().samples.isEmpty())
            return true;
    }

    // Check all other pads' in-memory settings for a saved file path.
    for (int i = 0; i < PadManager::kMaxPads; ++i)
    {
        if (i == padManager.selectedPadIndex) continue;
        if (padManager.padSettings[i].sampleFilePath.isNotEmpty())
            return true;
    }

    return false;
}

void MainComponent::requestQuit()
{
    if (!gjmManager.isDirty && !kitIsDirty)
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return;
    }

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::AlertWindow::QuestionIcon)
            .withTitle ("Save Session?")
            .withMessage ("You have unsaved changes. Save the session before closing?")
            .withButton ("Save Session")
            .withButton ("Close Without Saving")
            .withButton ("Cancel"),
        [this] (int result)
        {
            if (result == 1)
            {
                // Save session (may open a file dialog if untitled)
                saveSessionAction (false);
                // Quit after save — for titled sessions this is instant
                // (no dialog), so we can quit immediately.
                // For untitled sessions the file dialog is async; the user
                // will close the app again after saving.
                if (!gjmManager.isUntitled)
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }
            else if (result == 2)
            {
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }
            // result == 0 or 3 = Cancel — do nothing
        });
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



