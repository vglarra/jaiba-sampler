#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <memory>

#include "UIComponents.h"
#include "SampleCard.h"
#include "ConfigurationManager.h"
#include "MidiActivityLight.h"
#include "LoopingSampler.h"
#include "PadManager.h"
#include "TrianglePadGrid.h"

class MainComponent : public juce::AudioAppComponent,
                      public juce::Button::Listener,
                      public juce::ChangeListener,
                      public juce::MidiInputCallback,
                      public juce::Slider::Listener,
                      public MidiSelectorComponent::OwnerInterface,
                      public MappingComponent::OwnerInterface,
                      public SampleCard::Listener  // Add this
{
public:
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    //==============================================================================
    void paint(juce::Graphics& g) override;
    void resized() override;

    //==============================================================================
    void buttonClicked(juce::Button* button) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void sliderValueChanged(juce::Slider* slider) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    //==============================================================================
    // SampleListModel access methods
    int getSamplesCount() const { return pad().samples.size(); }
    juce::String getSampleName(int index) const
    {
        if (index >= 0 && index < pad().samples.size())
            return pad().samples[index]->name;
        return juce::String();
    }
    int getSampleLowNote(int index) const
    {
        if (index >= 0 && index < pad().samples.size())
            return pad().samples[index]->lowNote;
        return 0;
    }
    int getSampleHighNote(int index) const
    {
        if (index >= 0 && index < pad().samples.size())
            return pad().samples[index]->highNote;
        return 0;
    }
    int getSampleRootNote(int index) const
    {
        if (index >= 0 && index < pad().samples.size())
            return pad().samples[index]->rootNote;
        return 0;
    }
    int getSelectedSampleIndex() const { return pad().selectedSampleIndex; }
    void setSelectedSampleIndex(int index)
    {
        pad().selectedSampleIndex = index;
        updateMappingUI();
    }

private:
    //==============================================================================
    // Snapshot of all SampleCard settings captured just before a Trim write begins.
    // Passed to loadSampleFileAsync so the trimmed file inherits everything except
    // start/end points (which reset to 0/full since the file length changes).
    // Must be declared before loadSampleFileAsync which uses it as a default parameter.
    struct TrimSettingsSnapshot
    {
        bool valid = false;

        // Pitch
        int    pitchCents      = 0;
        int    basePitchOffset = 0;
        double baseTuningHz    = 440.0;
        int    pitchStepCents  = 100;
        juce::String detectedNoteName;
        double       detectedFreqHz = 0.0;

        // Playback modes
        bool loopEnabled    = false;
        bool oneShotEnabled = false;
        bool reverseEnabled = false;
        bool bounceEnabled  = false;

        // Volume
        float volumeLevel  = 1.0f;

        // Normalize
        bool  normEnabled  = false;
        float normTargetDb = -6.0f;

        // ADSR
        bool  adsrEnabled  = false;
        float adsrAttackMs = 0.0f;
        float adsrDecayMs  = 0.0f;
        float adsrSustain  = 1.0f;
        float adsrReleaseMs= 0.0f;

        // EQ
        bool  eqEnabled = false;
        float eq1Freq = 100.0f, eq1Gain = 0.0f, eq1Q = 1.0f; int eq1Mode = 2;
        float eq2Freq = 500.0f, eq2Gain = 0.0f, eq2Q = 1.0f; int eq2Mode = 2;
        float eq3Freq = 8000.0f,eq3Gain = 0.0f, eq3Q = 1.0f; int eq3Mode = 2;

        // Transient
        bool  transientDetectionEnabled = true;
        float transientThreshold        = 4.0f;

        // Grid
        bool gridSnapEnabled    = false;
        int  gridResolutionIndex= 5;
    };

    //==============================================================================
    // Sample loading and management
    void loadSampleFile(const juce::File& file);
    // deferTransients=true during Prev/Next navigation: skips detectTransients() on the
    // message thread and fires it via transientDetectionTimer 800ms after navigation stops.
    // trimSnapshot: when valid=true, restore all settings from snapshot instead of config lookup.
    void loadSampleFileAsync(const juce::File& file, bool autoPlay = true, bool resetZoom = false,
                              bool deferTransients = false, TrimSettingsSnapshot trimSnapshot = {});
    // Loads a sample into a specific pad engine WITHOUT touching the SampleCard UI.
    // Used at startup to restore all saved pads into RAM so pad switching is instant.
    void preloadPadEngineAsync(int padIdx, juce::File file, PadSettings settings);
    void updateSamplerSounds();

    // Audio device management
    void showAudioDeviceSettings();
    void updateDeviceInfo();
    void saveAudioSettings();
    void loadAudioSettings();

    // MIDI device management
    void showMidiDeviceSettings();
    void updateMidiDeviceList();

    // Sine wave test
    void toggleSineWave();

    // Multi-sample mapping interface
    void showMappingInterface();
    void addSampleToMap();
    void removeSelectedSample();
    void clearAllSamples();
    void updateMappingUI();

    //==============================================================================
    // Panic reset — hard-cuts all audio immediately
    void performPanicReset();

    //==============================================================================
    // Trim — writes a new WAV containing only the Start-to-End region.
    // Validates, shows confirmation dialog, writes on background thread, then loads.
    void performTrimAsync();

    //==============================================================================
    // Normalize — delegates to pad().computeNormGainFromAudio()
    float computeNormGainFromAudio(float targetDb) const
    {
        return pad().computeNormGainFromAudio(targetDb);
    }

    //==============================================================================
    // Playhead — called 60fps from SampleCard's PlayheadTimer.
    // Iterates voices, reads playheadPositionAtomic / totalSamplesAtomic (both atomic),
    // returns normalized [0,1] position or -1.0 when no voice is active.
    double getPlayheadPositionNormalized()
    {
        return pad().getPlayheadPositionNormalized();
    }

    //==============================================================================
    // Navigation optimizations
    // Flushes the deferred save (called 500ms after last navigation press stops).
    void flushNavigationSave();
    // Runs transient detection on the current sample (called 800ms after navigation stops).
    void runDeferredTransientDetection();

    //==============================================================================
    // New UI functionality
    void showSettingsMenu();
    void showKitMenu();
    void saveKitToFile (const juce::File& file);
    void loadKitFromFile (const juce::File& file);
    TrimSettingsSnapshot padSettingsToSnapshot (const PadSettings& ps) const;
    void scanCurrentFolderForAudioFiles();
    void navigateToFile(int index);
    void loadNextSample();
    void loadPrevSample();
    
    //==============================================================================
    // SampleCard::Listener implementation
    void midiNoteChanged(int newNote) override;
    void midiChannelChanged(int newChannel) override;
    void learningModeChanged(bool isLearning) override;
    void pitchOffsetChanged(int pitchOffset) override;
    void volumeChanged(float volume) override;
    void startPointChanged(double startPointSeconds) override;
    void endPointChanged(double endPointSeconds) override;
    void loopEnabledChanged(bool isLooping) override;
    void freezeChanged(bool isFrozen) override;
    void gridSnapChanged(bool isEnabled) override;
    void gridResolutionChanged(int index) override;
    void detectedNoteChanged(const juce::String& noteName, double freqHz) override;
    void transientDetectionEnabledChanged(bool enabled) override;
    void oneShotEnabledChanged(bool enabled) override;
    void reverseEnabledChanged(bool enabled) override;
    void bounceEnabledChanged(bool enabled) override;
    void pitchStepCentsChanged(int cents) override;
    void adsrParamsChanged(bool enabled, float attackMs, float decayMs, float sustain, float releaseMs) override;
    void activeTabChanged(int tabIndex) override;
    void eqParamsChanged(bool enabled,
                         float f1, float g1, float q1,
                         float f2, float g2, float q2,
                         float f3, float g3, float q3) override;
    void eqFilterModesChanged(int mode1, int mode2, int mode3) override;
    void normChanged(bool enabled, float targetDb) override;

    //==============================================================================
    // MIDI Learn handling
    void handleMidiLearn(int noteNumber);

    //==============================================================================
    // Draggable label for base tuning frequency (400–480 Hz, default 440 Hz).
    // Drag up = increase Hz, drag down = decrease Hz.
    // Hold Shift while dragging for coarser 1.0 Hz steps (default 0.1 Hz).
    class DraggableHzLabel : public juce::Label
    {
    public:
        std::function<void(double newHz)> onHzChanged;
        std::function<void()>            onDragFinished;

        void mouseEnter(const juce::MouseEvent&) override
        { isHovered = true; setMouseCursor(juce::MouseCursor::UpDownResizeCursor); repaint(); }
        void mouseExit(const juce::MouseEvent&) override
        { isHovered = false; setMouseCursor(juce::MouseCursor::NormalCursor); repaint(); }

        void mouseDown(const juce::MouseEvent& e) override
        { dragStartY = e.getScreenPosition().y; hzAtDragStart = currentHz; isDragging = true; repaint(); }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            int deltaY = dragStartY - e.getScreenPosition().y;
            double step = e.mods.isShiftDown() ? 1.0 : 0.1;
            double raw  = hzAtDragStart + deltaY * step;
            currentHz   = juce::jlimit(400.0, 480.0, std::round(raw / step) * step);
            updateHzDisplay();
            if (onHzChanged) onHzChanged(currentHz);
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            int deltaY = dragStartY - e.getScreenPosition().y;
            double step = e.mods.isShiftDown() ? 1.0 : 0.1;
            double raw  = hzAtDragStart + deltaY * step;
            currentHz   = juce::jlimit(400.0, 480.0, std::round(raw / step) * step);
            updateHzDisplay();
            if (onHzChanged) onHzChanged(currentHz);
            if (onDragFinished) onDragFinished();
            isDragging = false; repaint();
        }

        void setHz(double hz)
        { currentHz = juce::jlimit(400.0, 480.0, hz); updateHzDisplay(); }

        double getHz() const { return currentHz; }

        void paint(juce::Graphics& g) override
        {
            juce::Label::paint(g);
            if (isHovered || isDragging)
            { g.setColour(juce::Colour(0x44FFFFFF)); g.drawRect(getLocalBounds().reduced(1), 1); }
        }

    private:
        void updateHzDisplay()
        { setText(juce::String(currentHz, 1) + " Hz", juce::dontSendNotification); }

        double currentHz      = 440.0;
        double hzAtDragStart  = 440.0;
        int    dragStartY     = 0;
        bool   isHovered      = false;
        bool   isDragging     = false;
    };

    //==============================================================================
    // Compact knob LookAndFeel — shared with SampleCard knobs (defined in KnobLookAndFeel.h)
    CompactKnobLookAndFeel compactKnobLaf;

    // ── Pad grid UI (Part 1 — visual only) ──────────────────
    GlobalControlsBar globalControlsBar;
    TrianglePadGrid   padGrid;

    // UI Components
    juce::TextButton menuButton{ "Menu" };
    juce::TextButton kitButton { "Kit"  };
    juce::TextButton resetButton{ "Reset" };
    juce::TextButton testToneButton{ "Test tone" };
    juce::Slider masterVolumeKnob;
    juce::Label  masterVolumeLabel;
    juce::Label  masterVolValueLabel;  // percentage display to the right of the master vol knob
    DraggableHzLabel baseTuningLabel;      // Draggable display of base tuning frequency
    SampleCard sampleCard{formatManager};  // New card component with waveform support
    juce::Label audioDeviceInfoLabel;
    juce::Label midiDeviceInfoLabel;
    juce::Label cpuUsageLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::FileChooser> kitFileChooser;  // kept alive during async kit dialogs
    AudioPreviewComponent* activePreviewComp = nullptr;  // non-owning; JUCE owns via fileChooser
    
    //==============================================================================
    // Folder navigation
    juce::File currentFolder;
    juce::Array<juce::File> folderAudioFiles;
    int currentFileIndex = -1;
    std::atomic<bool> isScanning{false};
    juce::ReadWriteLock folderLock;  // Protect folderAudioFiles
    
    //==============================================================================
    // Audio components
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;
    juce::ThreadPool backgroundThreads{1};  // 1 thread — prevents stale loads finishing after newer ones

    // Phase 2: PadManager owns all audio processing for all pads.
    // padManager must be declared AFTER formatManager (PadManager ctor takes a ref to it).
    PadManager padManager { formatManager };

    // Convenience accessor — returns engine for the currently selected pad.
    // padManager.selectedPadIndex tracks which pad is active in the UI.
    PadAudioEngine& pad() { return padManager.getEngine(padManager.selectedPadIndex); }
    const PadAudioEngine& pad() const { return padManager.getEngine(padManager.selectedPadIndex); }
    
    //==============================================================================
    // MIDI components
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::StringArray midiInputNames;
    juce::String currentMidiDeviceName;
    juce::CriticalSection midiLock;  // Thread safety for MIDI device management
    
    // MIDI Learn mode
    bool isLearningMode = false;
    
    //==============================================================================
    // Volume — master volume lives in PadManager; per-pad volume lives in PadAudioEngine.
    // These thin wrappers keep the existing MainComponent callsites unchanged.
    std::atomic<float> masterVolumeGain { 0.7f };  // mirrored to padManager for the knob callback

    //==============================================================================
    // Sine wave generation
    bool sineWaveActive = false;
    double sineWavePhase = 0.0;
    double sineWaveFrequency = 440.0;
    float sineWaveAmplitude = 0.2f;
    
    // MappedSample is now PadAudioEngine::MappedSample.
    // Use pad().samples, pad().selectedSampleIndex, pad().sampleLock throughout.
    // This typedef keeps existing code in .cpp that uses MappedSample by name compiling.
    using MappedSample = PadAudioEngine::MappedSample;
    
    //==============================================================================
    // Mapping UI components
    std::unique_ptr<::SampleListModel> sampleListModel;
    juce::ListBox sampleListBox;
    
    juce::TextButton addSampleButton{ "Add Sample" };
    juce::TextButton removeSampleButton{ "Remove Sample" };
    juce::TextButton clearAllButton{ "Clear All" };
    juce::Slider rootNoteSlider;
    juce::Slider lowNoteSlider;
    juce::Slider highNoteSlider;
    juce::Label rootNoteLabel{ "Root Note:" };
    juce::Label lowNoteLabel{ "Low Note:" };
    juce::Label highNoteLabel{ "High Note:" };
    juce::Label mappingInstructions;
    
    //==============================================================================
    // Timer for updates
    class CpuTimer : public juce::Timer
    {
    public:
        CpuTimer(MainComponent& owner) : mainOwner(owner) {}
        void timerCallback() override { mainOwner.updateDeviceInfo(); }
    private:
        MainComponent& mainOwner;
    };
    
    CpuTimer cpuTimer;
    double lastCPU = 0.0;
    int cpuUpdateCounter = 0;

    //==============================================================================
    // One-shot tail detection — polls voice activity to know when the tail has finished.
    class OneShotTailTimer : public juce::Timer
    {
    public:
        OneShotTailTimer(MainComponent& owner) : mainOwner(owner) {}
        void timerCallback() override { mainOwner.checkOneShotTailDone(); }
    private:
        MainComponent& mainOwner;
    };

    OneShotTailTimer oneShotTailTimer;
    bool isOneShotTailPlaying = false;

    void checkOneShotTailDone();

    //==============================================================================
    // Opt 2 — deferred settings flush: writes once after 500ms of navigation idle
    class NavSaveTimer : public juce::Timer
    {
    public:
        NavSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.flushNavigationSave(); }
    private:
        MainComponent& owner;
    };
    NavSaveTimer navSaveTimer { *this };

    // Opt 3 — deferred transient detection: runs once after 800ms of navigation idle
    class TransientDetectionTimer : public juce::Timer
    {
    public:
        TransientDetectionTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.runDeferredTransientDetection(); }
    private:
        MainComponent& owner;
    };
    TransientDetectionTimer transientDetectionTimer { *this };

    // Deferred pitch save — fires 300ms after the last pitch Up/Down press.
    // Keeps the message thread free during rapid button presses; one disk write when idle.
    class PitchSaveTimer : public juce::Timer
    {
    public:
        PitchSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    PitchSaveTimer pitchSaveTimer { *this };

    // Deferred EQ save — fires 400ms after the last EQ drag event ends.
    // Prevents flooding disk with saves during control point dragging.
    class EqSaveTimer : public juce::Timer
    {
    public:
        EqSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    EqSaveTimer eqSaveTimer { *this };

    // Deferred marker save — fires 400ms after the last Start/End knob or waveform drag.
    // The atomic sound update in startPointChanged/endPointChanged is instant; only the
    // disk flush is deferred.  One write fires after the user stops dragging.
    class MarkerSaveTimer : public juce::Timer
    {
    public:
        MarkerSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    MarkerSaveTimer markerSaveTimer { *this };

    // Deferred volume save — fires 400ms after the last Vol knob change.
    // The atomic store in volumeChanged() is instant; only the disk flush is deferred.
    class VolSaveTimer : public juce::Timer
    {
    public:
        VolSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    VolSaveTimer volSaveTimer { *this };

    // Deferred ADSR save — fires 400ms after the last ADSR knob drag ends.
    // Atomic propagation to sounds is instant; only the disk flush is deferred.
    class AdsrSaveTimer : public juce::Timer
    {
    public:
        AdsrSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    AdsrSaveTimer adsrSaveTimer { *this };

    // Deferred normalize save — fires 400ms after the last Norm target button click.
    // Peak scan is instant (or async for large files); only the disk flush is deferred.
    class NormSaveTimer : public juce::Timer
    {
    public:
        NormSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    NormSaveTimer normSaveTimer { *this };

    // Deferred filter-mode save — fires 400ms after the last filter mode button click.
    // Coefficient recompute is instant (UI thread math); only the disk flush is deferred.
    class FilterModeSaveTimer : public juce::Timer
    {
    public:
        FilterModeSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    FilterModeSaveTimer filterModeSaveTimer { *this };

    // Deferred loop-state save — fires 400ms after the last Loop toggle.
    // The atomic store in loopEnabledChanged() is instant; only the disk flush is deferred.
    class LoopSaveTimer : public juce::Timer
    {
    public:
        LoopSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.saveCurrentSampleState(); }
    private:
        MainComponent& owner;
    };
    LoopSaveTimer loopSaveTimer { *this };

    class MidiSaveTimer : public juce::Timer
    {
    public:
        MidiSaveTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override
        {
            stopTimer();
            // Always flush — MIDI settings must persist even when the pad has no sample loaded.
            if (owner.configManager != nullptr)
                owner.configManager->flush();
            // Also save full sample state if a sample is loaded.
            owner.saveCurrentSampleState();
        }
    private:
        MainComponent& owner;
    };
    MidiSaveTimer midiSaveTimer { *this };

    // MIDI-to-audio latency measurement atomics live in PadAudioEngine (pad().midiNoteOnTicks etc.)
    // Thin accessors kept here so existing MainComponent.cpp references compile unchanged.

    // Timing: millisecond counter captured on Prev/Next press; printed when audio is ready.
    juce::int64 navStartTimeMs = 0;

    // Fix 1 — generation counter: incremented on each new navigation press.
    // Background jobs capture their generation; stale jobs abort before doing any work.
    std::atomic<int> navigationGeneration { 0 };

    // Fix 2 — 50ms debounce: rapid presses update currentFileIndex immediately but only
    // one load job fires after the presses stop.
    class NavDebounceTimer : public juce::Timer
    {
    public:
        NavDebounceTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { stopTimer(); owner.fireDebounceNavigation(); }
    private:
        MainComponent& owner;
    };
    NavDebounceTimer navDebounceTimer { *this };
    void fireDebounceNavigation();

    //==============================================================================
    // Heartbeat thread — prints every 100ms on its own OS thread.
    // During a message-thread freeze the heartbeat keeps printing, confirming
    // that the process is alive and pinpointing which operation caused the hang.
    class HeartbeatThread : public juce::Thread
    {
    public:
        HeartbeatThread() : juce::Thread("UI Heartbeat") {}
        void run() override
        {
            while (!threadShouldExit())
            {
                wait(100);
                if (!threadShouldExit())
                {
                }
            }
        }
    };
    HeartbeatThread heartbeatThread;

    // Called once after scanCurrentFolderForAudioFiles() completes (lazy scan on first Prev/Next).
    std::function<void()> postScanAction;

    //==============================================================================
    // MidiSelectorComponent::OwnerInterface implementation
    juce::String getCurrentMidiDeviceName() const override 
    { 
        juce::ScopedLock lock(midiLock);
        return currentMidiDeviceName; 
    }
    void setCurrentMidiDeviceName(const juce::String& deviceName) override 
    { 
        juce::ScopedLock lock(midiLock);
        currentMidiDeviceName = deviceName; 
    }
    void stopMidiInput() override 
    { 
        juce::ScopedLock lock(midiLock);
        
        if (midiInput != nullptr)
        {
            // First stop the input
            midiInput->stop();
            
            // Then reset the unique_ptr to properly delete the object
            midiInput.reset();
        }
        
        // Reset the collector with current sample rate (or default if not playing)
        double sampleRate = pad().getSampleRate();
        if (sampleRate <= 0)
            sampleRate = 44100.0; // Default if not set
            
        midiCollector.reset(sampleRate);
    }
    void startMidiInput(const juce::String& deviceName) override
    {
        juce::ScopedLock lock(midiLock);
        
        auto devices = juce::MidiInput::getAvailableDevices();
        for (auto& device : devices)
        {
            if (device.name == deviceName)
            {
                midiInput = juce::MidiInput::openDevice(device.identifier, this);
                if (midiInput != nullptr)
                {
                    midiInput->start();
                }
                else
                {
                }
                break;
            }
        }
    }

    //==============================================================================
    // MappingComponent::OwnerInterface implementation
    juce::ListBox& getSampleListBox() override { return sampleListBox; }
    juce::TextButton& getAddSampleButton() override { return addSampleButton; }
    juce::TextButton& getRemoveSampleButton() override { return removeSampleButton; }
    juce::TextButton& getClearAllButton() override { return clearAllButton; }
    juce::Slider& getLowNoteSlider() override { return lowNoteSlider; }
    juce::Slider& getHighNoteSlider() override { return highNoteSlider; }
    juce::Slider& getRootNoteSlider() override { return rootNoteSlider; }
    juce::Label& getLowNoteLabel() override { return lowNoteLabel; }
    juce::Label& getHighNoteLabel() override { return highNoteLabel; }
    juce::Label& getRootNoteLabel() override { return rootNoteLabel; }
    juce::Label& getMappingInstructions() override { return mappingInstructions; }

    //==============================================================================
    // Session persistence
    void saveOutgoingSampleState();  // Save current sample's state BEFORE loading a new one
    void saveCurrentSampleState();   // Save current sample's state (called on every change)
    // Capture current SampleCard UI state + sample file path into padManager.padSettings[padIdx].
    // Called on pad switch (outgoing) and in saveCurrentSampleState (current pad).
    void captureSampleCardToPadSettings(int padIdx);
    std::unique_ptr<ConfigurationManager> configManager;
    MidiActivityLight midiActivityLight;
    
    //==============================================================================
    // Session management methods
    void loadLastSession();
    void saveCurrentSession();
    bool isValidMidiDevice(const juce::String& deviceName);
    void midiDeviceChanged(const juce::String& newDevice);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

