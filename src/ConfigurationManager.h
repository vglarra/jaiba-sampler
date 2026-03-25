#pragma once

#include <juce_core/juce_core.h>
#include "SampleCard.h"

class ConfigurationManager
{
public:
    ConfigurationManager()
    {
        // Set up the properties file
        juce::PropertiesFile::Options options;
        options.applicationName = "JaivaSampler";
        options.filenameSuffix = ".settings";
        options.folderName = "JaivaSampler";
        options.osxLibrarySubFolder = "Application Support";
        // Default is 3000ms — means saveIfNeeded() skips the write if called within 3s
        // of the last save.  Set to 0 so every saveIfNeeded() call writes to disk immediately.
        options.millisecondsBeforeSaving = 0;

        propertiesFile = std::make_unique<juce::PropertiesFile>(options);
    }
    
    ~ConfigurationManager()
    {
        propertiesFile->saveIfNeeded();
    }
    
    void saveLastDirectory(const juce::File& directory)
    {
        propertiesFile->setValue("lastDirectory", directory.getFullPathName());
        propertiesFile->saveIfNeeded();
    }
    
    juce::File getLastDirectory()
    {
        juce::String path = propertiesFile->getValue("lastDirectory");
        if (path.isNotEmpty())
        {
            juce::File dir(path);
            if (dir.exists() && dir.isDirectory())
                return dir;
        }
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    }
    
    void saveLastSample(const juce::File& sample)
    {
        propertiesFile->setValue("lastSample", sample.getFullPathName());
        propertiesFile->saveIfNeeded();
    }
    
    juce::File getLastSample()
    {
        juce::String path = propertiesFile->getValue("lastSample");
        if (path.isNotEmpty())
        {
            juce::File file(path);
            if (file.existsAsFile())
                return file;
        }
        return juce::File();
    }
    
    void saveMidiSettings(int midiNote, int midiChannel, const juce::String& midiDevice)
    {
        propertiesFile->setValue("midiNote", midiNote);
        propertiesFile->setValue("midiChannel", midiChannel);
        propertiesFile->setValue("midiDevice", midiDevice);
        propertiesFile->saveIfNeeded();
    }
    
    void savePitchOffset(int pitchOffsetCents)
    {
        propertiesFile->setValue("pitchOffsetCents", pitchOffsetCents);
        propertiesFile->saveIfNeeded();  // writes immediately because millisecondsBeforeSaving=0
    }

    // Force a full flush to disk — call from destructor and any critical save points.
    void saveNow()
    {
        propertiesFile->save();
    }

    void saveVolume(float volume)
    {
        propertiesFile->setValue("volume", (double)volume);
        propertiesFile->saveIfNeeded();
    }

    void saveStartPoint(double startPointSeconds)
    {
        propertiesFile->setValue("startPointSeconds", startPointSeconds);
        propertiesFile->saveIfNeeded();
    }

    double getStartPoint()
    {
        return propertiesFile->getDoubleValue("startPointSeconds", 0.0);
    }

    void saveEndPoint(double endPointSeconds)
    {
        propertiesFile->setValue("endPointSeconds", endPointSeconds);
        propertiesFile->saveIfNeeded();
    }

    // Returns -1.0 if not saved (sentinel meaning "use full sample length")
    double getEndPoint()
    {
        return propertiesFile->getDoubleValue("endPointSeconds", -1.0);
    }
    
    void saveMasterVolume(float volume)
    {
        propertiesFile->setValue("masterVolume", (double)volume);
        propertiesFile->saveIfNeeded();
    }

    float getMasterVolume()
    {
        return (float)propertiesFile->getDoubleValue("masterVolume", 0.7); // Default 70%
    }

    void saveLoopEnabled(bool loopEnabled)
    {
        propertiesFile->setValue("loopEnabled", loopEnabled);
        propertiesFile->saveIfNeeded();
    }

    bool getLoopEnabled()
    {
        return propertiesFile->getBoolValue("loopEnabled", false);
    }

    void saveGridSnapEnabled(bool enabled)
    {
        propertiesFile->setValue("gridSnapEnabled", enabled);
        propertiesFile->saveIfNeeded();
    }

    bool getGridSnapEnabled()
    {
        return propertiesFile->getBoolValue("gridSnapEnabled", false);
    }

    void saveGridResolutionIndex(int index)
    {
        propertiesFile->setValue("gridResolutionIndex", index);
        propertiesFile->saveIfNeeded();
    }

    int getGridResolutionIndex()
    {
        return propertiesFile->getIntValue("gridResolutionIndex", 5); // default 1s
    }

    void saveOneShotEnabled(bool enabled)
    {
        propertiesFile->setValue("oneShotEnabled", enabled);
        propertiesFile->saveIfNeeded();
    }

    bool getOneShotEnabled()
    {
        return propertiesFile->getBoolValue("oneShotEnabled", false); // default OFF
    }

    // Base tuning frequency — key 'baseTuningHz' (default 440.0 Hz, range 400–480)
    void saveBaseTuningHz(double hz)
    {
        propertiesFile->setValue("baseTuningHz", hz);
        propertiesFile->saveIfNeeded();
    }

    double getBaseTuningHz()
    {
        return propertiesFile->getDoubleValue("baseTuningHz", 440.0);
    }

    // Microtonal pitch step size — key 'pitchStepCents' (default 100 = 1 semitone)
    void savePitchStepCents(int cents)
    {
        propertiesFile->setValue("pitchStepCents", cents);
        propertiesFile->saveIfNeeded();
    }

    int getPitchStepCents()
    {
        return propertiesFile->getIntValue("pitchStepCents", 100);
    }

    // FIX 3: transient detection on/off — key 'transientDetectionEnabled'
    void saveTransientDetectionEnabled(bool enabled)
    {
        propertiesFile->setValue("transientDetectionEnabled", enabled);
        propertiesFile->saveIfNeeded();
    }

    bool getTransientDetectionEnabled()
    {
        return propertiesFile->getBoolValue("transientDetectionEnabled", true); // default ON
    }

    // Active tab index — key 'activeTab' (default 0 = Controls)
    void saveActiveTab(int tabIndex)
    {
        propertiesFile->setValue("activeTab", tabIndex);
        propertiesFile->saveIfNeeded();
    }

    int getActiveTab()
    {
        return propertiesFile->getIntValue("activeTab", 0);
    }

    // FIX 2: user-selected resolution in milliseconds — key 'gridResolution'
    // Returns -1.0 if the user has never manually selected a resolution.
    void saveGridResolutionMs(double ms)
    {
        propertiesFile->setValue("gridResolution", ms);
        propertiesFile->saveIfNeeded();
    }

    double getGridResolutionMs()
    {
        return propertiesFile->getDoubleValue("gridResolution", -1.0); // -1 = never set
    }

    // NEW: Save audio device settings
    void saveAudioSettings(int bufferSize, double sampleRate, const juce::String& deviceType, const juce::String& outputDeviceName)
    {
        propertiesFile->setValue("audioBufferSize", bufferSize);
        propertiesFile->setValue("audioSampleRate", sampleRate);
        propertiesFile->setValue("audioDeviceType", deviceType);
        propertiesFile->setValue("audioOutputDevice", outputDeviceName);
        propertiesFile->saveIfNeeded();
    }
    
    int getMidiNote()
    {
        return propertiesFile->getIntValue("midiNote", 60); // Default to middle C
    }
    
    int getMidiChannel()
    {
        return propertiesFile->getIntValue("midiChannel", 1); // Default to channel 1
    }
    
    juce::String getMidiDevice()
    {
        return propertiesFile->getValue("midiDevice");
    }
    
    int getPitchOffset()
    {
        // New key stores cents directly; fall back to old semitone key × 100 for migration
        if (propertiesFile->containsKey("pitchOffsetCents"))
            return propertiesFile->getIntValue("pitchOffsetCents", 0);
        int semitones = propertiesFile->getIntValue("pitchOffset", 0);
        return semitones * 100;
    }

    float getVolume()
    {
        return (float)propertiesFile->getDoubleValue("volume", 1.0); // Default to full volume
    }
    
    // NEW: Get audio settings
    int getAudioBufferSize()
    {
        return propertiesFile->getIntValue("audioBufferSize", 512); // Default to 512
    }
    
    double getAudioSampleRate()
    {
        return propertiesFile->getDoubleValue("audioSampleRate", 44100.0); // Default to 44.1kHz
    }
    
    juce::String getAudioDeviceType()
    {
        return propertiesFile->getValue("audioDeviceType");
    }
    
    juce::String getAudioOutputDevice()
    {
        return propertiesFile->getValue("audioOutputDevice");
    }
    
    juce::String getSettingsFilePath() const
    {
        return propertiesFile->getFile().getFullPathName();
    }

    //==============================================================================
    // Per-sample state — each sample file has its own independently saved values.
    // Key is a hex hash of the absolute file path (safe for use as XML attribute name).
    // Per-sample state — start/end points, volume, loop only.
    // Pitch is NOT per-sample — it is a single global session value (see savePitchOffset/getPitchOffset).
    struct SampleState
    {
        double startPoint       = 0.0;
        double endPoint         = -1.0;  // sentinel: -1.0 = full length
        float  volume           = 1.0f;
        bool   loopEnabled      = false;
        double transientThreshold = 4.0; // RMS multiplier for transient detection
        juce::String detectedNoteName;   // e.g. "D3" — empty if never tuned
        double detectedFreqHz   = 0.0;   // detected frequency in Hz
        int    basePitchOffset  = 0;     // hidden tune correction; user pitch 0 = this note
        bool   exists           = false; // false = no saved state found for this file
        // ADSR envelope per sample
        bool   adsrEnabled      = false;
        float  adsrAttackMs     = 0.0f;
        float  adsrDecayMs      = 0.0f;
        float  adsrSustain      = 1.0f;
        float  adsrReleaseMs    = 0.0f;
    };

    void saveSampleState(const juce::File& file, const SampleState& s)
    {
        auto k = sampleKey(file);
        propertiesFile->setValue(k + "_start",    s.startPoint);
        propertiesFile->setValue(k + "_end",      s.endPoint);
        propertiesFile->setValue(k + "_vol",      (double)s.volume);
        propertiesFile->setValue(k + "_loop",     s.loopEnabled);
        propertiesFile->setValue(k + "_thresh",   s.transientThreshold);
        propertiesFile->setValue(k + "_notename",   s.detectedNoteName);
        propertiesFile->setValue(k + "_notehz",    s.detectedFreqHz);
        propertiesFile->setValue(k + "_basepitch", s.basePitchOffset);
        // ADSR per sample
        propertiesFile->setValue(k + "_adsren",    s.adsrEnabled);
        propertiesFile->setValue(k + "_adsratk",   (double)s.adsrAttackMs);
        propertiesFile->setValue(k + "_adsrdcy",   (double)s.adsrDecayMs);
        propertiesFile->setValue(k + "_adsrsus",   (double)s.adsrSustain);
        propertiesFile->setValue(k + "_adsrrel",   (double)s.adsrReleaseMs);
        propertiesFile->saveIfNeeded();
    }

    SampleState getSampleState(const juce::File& file) const
    {
        auto k = sampleKey(file);
        SampleState s;
        s.exists              = propertiesFile->containsKey(k + "_start");
        s.startPoint          = propertiesFile->getDoubleValue(k + "_start",    0.0);
        s.endPoint            = propertiesFile->getDoubleValue(k + "_end",      -1.0);
        s.volume              = (float)propertiesFile->getDoubleValue(k + "_vol",     1.0);
        s.loopEnabled         = propertiesFile->getBoolValue  (k + "_loop",     false);
        s.transientThreshold  = propertiesFile->getDoubleValue(k + "_thresh",   4.0);
        s.detectedNoteName    = propertiesFile->getValue      (k + "_notename",   "");
        s.detectedFreqHz      = propertiesFile->getDoubleValue(k + "_notehz",   0.0);
        s.basePitchOffset     = propertiesFile->getIntValue   (k + "_basepitch", 0);
        // ADSR per sample
        s.adsrEnabled         = propertiesFile->getBoolValue  (k + "_adsren",    false);
        s.adsrAttackMs        = (float)propertiesFile->getDoubleValue(k + "_adsratk", 0.0);
        s.adsrDecayMs         = (float)propertiesFile->getDoubleValue(k + "_adsrdcy", 0.0);
        s.adsrSustain         = (float)propertiesFile->getDoubleValue(k + "_adsrsus", 1.0);
        s.adsrReleaseMs       = (float)propertiesFile->getDoubleValue(k + "_adsrrel", 0.0);
        return s;
    }

private:
    // Encode file path as a safe XML attribute key by hashing the full path.
    // Prefix "p" ensures it never starts with a digit (XML attribute requirement).
    juce::String sampleKey(const juce::File& file) const
    {
        return "p" + juce::String::toHexString(file.getFullPathName().hashCode64());
    }

    std::unique_ptr<juce::PropertiesFile> propertiesFile;
};


