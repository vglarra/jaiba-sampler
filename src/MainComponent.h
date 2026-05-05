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
#include "GjmManager.h"
#include "GlobalLoopColumn.h"

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
    void paintOverChildren(juce::Graphics& g) override;
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

    // Called by Main.cpp closeButtonPressed — shows save-before-close dialog if needed.
    void requestQuit();

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
        float padGain = 1.0f;

        // Transient
        bool  transientDetectionEnabled = true;
        float transientThreshold        = 4.0f;

        // Grid
        bool gridSnapEnabled    = false;
        int  gridResolutionIndex= 5;

        // MIDI routing
        int midiNote    = 60;
        int midiChannel = 1;

        // Start / end markers — restored for kit loads; trim loads leave at defaults (0.0 / -1.0)
        double startPointSeconds = 0.0;
        double endPointSeconds   = -1.0;
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
    void preloadGlobalPadEngineAsync(int globalPadIdx, juce::File file, PadSettings settings);
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

    bool hasAnySamplesLoaded() const;
    void showSettingsMenu();
    void showKitMenu();
    void newKitAction();
    void clearAllPadsForNewKit();
    void saveKitToFile   (const juce::File& file);
    void loadKitFromFile (const juce::File& file);
    void navigateKit     (int direction);   // -1 = prev, +1 = next .jai in same folder

    // GJM — Global Jaiva Map (16-bank manifest)
    void loadGjmFromFile  (const juce::File& file);
    void saveGjmToFile    (const juce::File& file);
    void switchGjmBank    (int bankIdx);   // 0-based; instant swap + async audio pre-warm
    void finishGjmLoad    ();             // called on message thread after background parse
    void updateGjmUI      ();
    void saveSessionAction (bool forceDialog);
    void loadSessionAction ();
    void saveBankKitAction ();
    void loadBankKitAction ();
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
    void mnFreezeEnabledChanged(bool enabled) override;
    void pitchStepCentsChanged(int cents) override;
    void adsrParamsChanged(bool enabled, float attackMs, float decayMs, float sustain, float releaseMs) override;
    void activeTabChanged(int tabIndex) override;
    void eqParamsChanged(bool enabled,
                         float f1, float g1, float q1,
                         float f2, float g2, float q2,
                         float f3, float g3, float q3) override;
    void eqFilterModesChanged(int mode1, int mode2, int mode3) override;
    void normChanged(bool enabled, float targetDb) override;
    void padGainChanged(float gain) override;
    void beginRecording(double bpm, double quantInBeats,
                        bool metronomeOn, int targetPadIndex, bool overdub) override;
    void proceedWithRecording(double bpm, double quantInBeats,
                              bool metronomeOn, int targetPadIndex, bool overdub);
    void endRecording() override;
    void playbackQuantisedEvents() override;
    void metronomeStandaloneChanged (bool on, double bpm) override;
    void metronomeVolumeChanged     (float vol) override;
    void savePattern() override;
    void loadPattern() override;
    void clearPattern() override;
    void dropTargetPad(int padIndex) override;
    void dropTargetPadWithCallback(int padIndex, std::function<void()> onDropComplete);
    void executeDropPad(int padIdx);  // runs the actual drop after file-disposition dialogs

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
    GlobalLoopColumn  globalLoopColumn;

    // UI Components
    juce::TextButton menuButton{ "Menu" };
    juce::TextButton kitButton     { "Kit" };
    juce::Label      kitNameLabel;              // shiny display showing the loaded kit name
    juce::TextButton kitPrevButton { "<" };     // navigate to previous .jai file in same folder
    juce::TextButton kitNextButton { ">" };     // navigate to next .jai file in same folder
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
    std::unique_ptr<juce::FileChooser> gjmFileChooser;  // kept alive during async GJM dialogs
    AudioPreviewComponent* activePreviewComp = nullptr;  // non-owning; JUCE owns via fileChooser
    
    //==============================================================================
    // Kit navigation
    juce::File currentKitFile;   // last successfully loaded/saved .jai file; empty if none
    bool kitIsDirty = false;     // true when pads changed since last save/load/new-kit

    //==============================================================================
    // Global Loop Pads — 5 persistent loop-pad slots that survive bank switching.
    enum class PadSelectionSource { Bank, Global };
    PadSelectionSource padSelectionSource  = PadSelectionSource::Bank;
    int                selectedGlobalPadIndex = -1;
    bool               globalPadPlaying[PadManager::kNumGlobalPads] = {};

    void selectGlobalPad          (int slotIdx);
    void toggleGlobalPadPlayback  (int slotIdx);
    void captureGlobalPadFromSampleCard (int slotIdx);
    void saveGlobalPadStateFromEngine   (int slotIdx);
    void updateGlobalPadVisuals   ();
    void transferKitPadToGlobal   (int kitPadIdx, int globalPadIdx);
    void dropGlobalPad            (int globalPadIdx);

    //==============================================================================
    // GJM — Global Jaiva Map
    GjmManager  gjmManager;
    juce::Label gjmStatusLabel;              // shows "No GJM" or "filename ● N/16"
    std::atomic<bool> gjmParsing { false };  // true while background kit-parse job runs

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
    // When a global loop pad is selected (padSelectionSource == Global), routes to
    // padManager.getGlobalEngine(selectedGlobalPadIndex) instead.
    PadAudioEngine& pad()
    {
        if (padSelectionSource == PadSelectionSource::Global && selectedGlobalPadIndex >= 0)
            return padManager.getGlobalEngine(selectedGlobalPadIndex);
        return padManager.getEngine(padManager.selectedPadIndex);
    }
    const PadAudioEngine& pad() const
    {
        if (padSelectionSource == PadSelectionSource::Global && selectedGlobalPadIndex >= 0)
            return padManager.getGlobalEngine(selectedGlobalPadIndex);
        return padManager.getEngine(padManager.selectedPadIndex);
    }
    
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
    // MNFreeze — MIDI Note Freeze: per-pad tracking so any pad's freeze keeps
    // working independently of which pad is currently selected in the UI.
    // Atomics are written on the MIDI callback thread; non-atomic fields are
    // message-thread-only.
    struct PadMnFreezeState
    {
        std::atomic<bool> enabled  { false };  // MNFreeze button is ON for this pad
        std::atomic<bool> isActive { false };  // freeze is currently engaged on this pad
        std::atomic<int>  note     { 60 };     // MIDI note this pad listens to
        std::atomic<int>  ch       { 1 };      // MIDI channel (0 = any)
        float             preVol   = 1.0f;     // volumeGain before freeze (message thread)
        bool              preLoop  = false;    // loop state before freeze (message thread)
    };
    PadMnFreezeState padMnFreeze[16];

    // Applies or removes MNFreeze for an arbitrary pad engine.
    // Must be called on the message thread.
    void activateMnFreezeForPad (int padIdx, bool nowActive, float velocity);

    //==============================================================================
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

    //==============================================================================
    // Recording engine

    struct RecordedEvent { int padIndex = 0; double beatTime = 0.0; };

    // Cross-thread atomics
    std::atomic<bool>    recIsActive         { false };  // armed: events + WAV active
    std::atomic<bool>    recWaitForBeat      { false };  // WAV open but waiting for beat 0
    std::atomic<bool>    recMetronomeOn      { false };  // beep wanted (rec or standalone)
    std::atomic<bool>    metronomeStandalone { false };  // standalone metro (no recording)
    std::atomic<float>   metronomeVolume     { 0.5f  };  // 0–1, independent beep gain
    std::atomic<double>  recSongBeatPos      { 0.0   };  // shared beat clock
    std::atomic<double>  recBpmAtomic        { 120.0 };  // BPM for beat clock + metro
    std::atomic<int64_t> recBeatSampleOffset { 0 };      // WAV samples before beat 0

    // Message-thread-only parameters (written before arming)
    int    recTargetPad    = 15;
    double recQuantInBeats = 0.0625;
    bool   overdubMode     = false;  // true = merge new events with existing recQuantised

    // Audio-thread-only metronome state (no sync needed)
    int    recLastBeat       = -1;
    int    recMetroBeepLeft  = 0;
    double recMetroBeepPhase = 0.0;

    // Message-thread-only event storage
    std::vector<RecordedEvent> recEvents;
    std::vector<RecordedEvent> recQuantised;

    // Pattern playback (message thread)
    bool        recPatternPlaying = false;
    juce::int64 recPatternStartMs = 0;
    int         recPatternIdx     = 0;

    class PatternPlayTimer : public juce::Timer
    {
    public:
        PatternPlayTimer(MainComponent& o) : owner(o) {}
        void timerCallback() override { owner.tickPatternPlayback(); }
    private:
        MainComponent& owner;
    };
    PatternPlayTimer patternPlayTimer { *this };

    // Live output capture — taps master mix into WAV while user plays live.
    // Audio thread writes to this writer; message thread owns lifetime.
    juce::TimeSliceThread              wavWriterThread { "WAV Writer" };
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> liveRenderWriter { nullptr };
    std::atomic<bool> liveRenderActive { false };
    juce::File        liveRenderOutputFile;
    TrimSettingsSnapshot liveTargetSnap;   // settings to apply to target pad after render
    int               liveRenderTargetPad { -1 };

    void quantiseRecordedEvents();
    void finalizeLiveRender();   // called by endRecording(); closes WAV + optionally quantizes
    void tickPatternPlayback();  // only used by "Play Pattern" preview button
    void saveCurrentPattern();   // shows input dialog, saves to padManager.getSettings(recTargetPad)
    void loadPatternFromPad();   // shows popup menu, loads into recQuantised

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

