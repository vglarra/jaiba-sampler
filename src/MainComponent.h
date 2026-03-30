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

    //==============================================================================
    // SampleListModel access methods
    int getSamplesCount() const { return samples.size(); }
    juce::String getSampleName(int index) const 
    { 
        if (index >= 0 && index < samples.size())
            return samples[index]->name;
        return juce::String();
    }
    int getSampleLowNote(int index) const
    {
        if (index >= 0 && index < samples.size())
            return samples[index]->lowNote;
        return 0;
    }
    int getSampleHighNote(int index) const
    {
        if (index >= 0 && index < samples.size())
            return samples[index]->highNote;
        return 0;
    }
    int getSampleRootNote(int index) const
    {
        if (index >= 0 && index < samples.size())
            return samples[index]->rootNote;
        return 0;
    }
    int getSelectedSampleIndex() const { return selectedSampleIndex; }
    void setSelectedSampleIndex(int index) 
    { 
        selectedSampleIndex = index; 
        updateMappingUI();
    }

private:
    //==============================================================================
    // Sample loading and management
    void loadSampleFile(const juce::File& file);
    // deferTransients=true during Prev/Next navigation: skips detectTransients() on the
    // message thread and fires it via transientDetectionTimer 800ms after navigation stops.
    void loadSampleFileAsync(const juce::File& file, bool autoPlay = true, bool resetZoom = false, bool deferTransients = false);
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
    // Playhead — called 60fps from SampleCard's PlayheadTimer.
    // Iterates voices, reads playheadPositionAtomic / totalSamplesAtomic (both atomic),
    // returns normalized [0,1] position or -1.0 when no voice is active.
    double getPlayheadPositionNormalized()
    {
        for (int i = 0; i < sampler.getNumVoices(); ++i)
        {
            if (auto* voice = dynamic_cast<LoopingSamplerVoice*>(sampler.getVoice(i)))
            {
                const juce::int64 pos   = voice->playheadPositionAtomic.load();
                const juce::int64 total = voice->totalSamplesAtomic.load();
                if (pos >= 0 && total > 0)
                    return (double)pos / (double)total;
            }
        }
        return -1.0;
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
    void pitchStepCentsChanged(int cents) override;
    void adsrParamsChanged(bool enabled, float attackMs, float decayMs, float sustain, float releaseMs) override;
    void activeTabChanged(int tabIndex) override;
    void eqParamsChanged(bool enabled,
                         float f1, float g1, float q1,
                         float f2, float g2, float q2,
                         float f3, float g3, float q3) override;
    void eqFilterModesChanged(int mode1, int mode2, int mode3) override;

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

    // UI Components
    juce::TextButton menuButton{ "Menu" };
    juce::TextButton resetButton{ "Reset" };
    juce::TextButton testToneButton{ "Test tone" };
    juce::Slider masterVolumeKnob;
    juce::Label masterVolumeLabel;
    DraggableHzLabel baseTuningLabel;      // Draggable display of base tuning frequency
    SampleCard sampleCard{formatManager};  // New card component with waveform support
    juce::Label audioDeviceInfoLabel;
    juce::Label midiDeviceInfoLabel;
    juce::Label cpuUsageLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
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
    juce::Synthesiser sampler;
    juce::AudioFormatManager formatManager;
    juce::MidiMessageCollector midiCollector;
    juce::ThreadPool backgroundThreads{1};  // 1 thread — prevents stale loads finishing after newer ones
    
    //==============================================================================
    // MIDI components
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::StringArray midiInputNames;
    juce::String currentMidiDeviceName;
    juce::CriticalSection midiLock;  // Thread safety for MIDI device management
    
    // MIDI Learn mode
    bool isLearningMode = false;
    
    //==============================================================================
    // Volume
    std::atomic<float> volumeGain { 1.0f };
    std::atomic<float> masterVolumeGain { 0.7f };
    std::atomic<bool>  loopEnabled { false };
    // Set true during sample-change to silence the audio thread immediately.
    // Audio thread checks this at the top of getNextAudioBlock and returns a zeroed buffer.
    std::atomic<bool>  muteOutput { false };

    //==============================================================================
    // EQ — three-band parametric biquad filters
    // OPT 2: Coefficients are computed on the UI thread and stored in a lock-free
    // double buffer.  Audio thread swaps buffers in nanoseconds — zero coefficient
    // math on the audio thread.
    struct EqCoeffDoubleBuffer
    {
        struct Coeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

        Coeffs buf[2][3] {};           // [ping-pong index][band 0-2]
        std::atomic<bool> updated { false };
        int front = 0;                 // audio-thread owned; never accessed from UI thread

        // UI thread: write all 3 bands to back buffer, then set flag.
        void writeFromUI(const Coeffs newCoeffs[3]) noexcept
        {
            const int back = 1 - front;
            for (int i = 0; i < 3; ++i) buf[back][i] = newCoeffs[i];
            updated.store(true, std::memory_order_release);
        }

        // Audio thread: swap if updated; return pointer to current front band array.
        const Coeffs* swapIfUpdated() noexcept
        {
            if (updated.load(std::memory_order_acquire))
            {
                front = 1 - front;
                updated.store(false, std::memory_order_relaxed);
            }
            return buf[front];
        }

        EqCoeffDoubleBuffer() = default;
        EqCoeffDoubleBuffer(const EqCoeffDoubleBuffer&) = delete;
        EqCoeffDoubleBuffer& operator=(const EqCoeffDoubleBuffer&) = delete;
    };

    EqCoeffDoubleBuffer  eqCoeffDB;
    std::atomic<bool>    eqActive   { false };
    int                  eqFilterModes[3] = { 2, 2, 2 };  // message-thread only; 0-5 per band

    // Per-channel filter state — audio thread only, no locking needed.
    double eqZ1[3][2] {};  // [band][channel]
    double eqZ2[3][2] {};

    // Resets filter state (called from prepareToPlay and on EQ toggle).
    // Safe to call from message thread only when muteOutput=true (audio thread not running).
    void resetEqState()
    {
        for (int b = 0; b < 3; ++b)
            for (int ch = 0; ch < 2; ++ch)
                eqZ1[b][ch] = eqZ2[b][ch] = 0.0;
    }

    // OPT 2: Computes biquad coefficients for one band on the UI thread and returns them.
    // Caller assembles all 3 bands and calls eqCoeffDB.writeFromUI().
    // mode: 0=LowCut, 1=LowShelf, 2=Bell, 3=Notch, 4=HighShelf, 5=HighCut
    EqCoeffDoubleBuffer::Coeffs computeEqCoeffs(float freqHz, float gainDb, float q, int filterMode, double sampleRate);

    //==============================================================================
    // FFT spectrum analyzer — lock-free double buffer
    static constexpr int kFFTOrder  = 11;               // 2^11 = 2048
    static constexpr int kFFTSize   = 1 << kFFTOrder;   // 2048
    static constexpr int kSpecBins  = kFFTSize / 2;     // 1024 output bins

    std::unique_ptr<juce::dsp::FFT> fft;                // initialized in prepareToPlay

    // OPT 1: Lock-free circular buffer — audio thread writes mono samples here (< 1us);
    // FFT worker thread reads and processes without touching the audio thread.
    juce::AbstractFifo fftAbstractFifo { kFFTSize * 2 };
    float              fftCircularBuffer[kFFTSize * 2] {};

    // Hann window applied before FFT to reduce spectral leakage.
    float   fftWindow[kFFTSize] {};

    // Scratch buffer — used only by the FFT worker thread (never touched by audio thread).
    float   fftScratch[kFFTSize * 2] {};

    // Double-buffered magnitude spectrum (after smoothing).
    // FFT worker thread writes to buffer[1 - specFront]; UI reads from buffer[specFront].
    float             specBuffers[2][kSpecBins] {};
    std::atomic<int>  specFront     { 0 };
    std::atomic<bool> hasNewFFTData { false };  // set by FFT thread, cleared by UI timer

    // Exponential smoothing factors for spectrum display.
    static constexpr float kSpecSmoothUp   = 0.7f;   // fast attack
    static constexpr float kSpecSmoothDown = 0.3f;   // slower decay

    // OPT 1: Background FFT worker thread — wakes every 33ms, processes one FFT frame.
    class FftWorkerThread : public juce::Thread
    {
    public:
        FftWorkerThread(MainComponent& o) : juce::Thread("FFT Worker"), owner(o) {}
        void run() override
        {
            while (!threadShouldExit())
            {
                wait(33);
                if (!threadShouldExit())
                    owner.processFftOnWorkerThread();
            }
        }
    private:
        MainComponent& owner;
    };
    std::unique_ptr<FftWorkerThread> fftThread;

    // Called on the FFT worker thread — reads from fftCircularBuffer, runs FFT, updates specBuffers.
    void processFftOnWorkerThread();

    // Called by EQDisplay's getSpectrumCallback — copies front buffer to dest.
    // Returns true if new FFT data was available (consumed), false if no update since last call.
    bool getSpectrumSnapshot(float* dest, int numBins);

    //==============================================================================
    // Sine wave generation
    bool sineWaveActive = false;
    double sineWavePhase = 0.0;
    double sineWaveFrequency = 440.0;
    float sineWaveAmplitude = 0.2f;
    
    //==============================================================================
    // Sample management for multi-sampling
    struct MappedSample
    {
        juce::File file;
        juce::String name;
        int rootNote = 60;
        int lowNote = 48;
        int highNote = 60;
        double attack = 0.1;
        double release = 0.1;
        bool isSelected = false;
        int pitchOffset = 0;  // Add pitch offset per sample
        double startPointSeconds = 0.0;  // Sample start point offset
        double endPointSeconds   = -1.0; // Sample end point offset (-1 = full length)
        
        // Cached audio data — shared_ptr so LoopingSamplerSound can safely outlive a reload
        std::shared_ptr<juce::AudioBuffer<float>> audioData;
        double sampleRate = 0;
        int numChannels = 0;
        juce::int64 lengthInSamples = 0;
        
        // Add a flag to indicate if sample is loaded
        bool isValid() const { return audioData != nullptr && audioData->getNumSamples() > 0; }
        
        ~MappedSample() = default;
    };
    
    juce::OwnedArray<MappedSample> samples;
    int selectedSampleIndex = -1;
    juce::CriticalSection sampleLock;  // Thread safety for sample array access
    
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
                    printf("[HEARTBEAT] %lld\n",
                           (long long)juce::Time::getMillisecondCounter());
                    fflush(stdout);
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
        double sampleRate = sampler.getSampleRate();
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
                    printf("MIDI input started: %s\n", deviceName.toRawUTF8());
                }
                else
                {
                    printf("ERROR: Failed to open MIDI device: %s\n", deviceName.toRawUTF8());
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

