#pragma once

#include <juce_core/juce_core.h>
#include "SampleCard.h"

class ConfigurationManager
{
public:
    ConfigurationManager()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "JaivaSampler";
        options.filenameSuffix = ".settings";
        options.folderName = "JaivaSampler";
        options.osxLibrarySubFolder = "Application Support";
        // Large value — we control all flushes explicitly via flush().
        // saveIfNeeded() calls inside setters are intentionally removed.
        options.millisecondsBeforeSaving = 60000;

        propertiesFile = std::make_unique<juce::PropertiesFile>(options);
    }

    ~ConfigurationManager()
    {
        propertiesFile->save();   // Final flush on shutdown
    }

    // Flush all pending in-memory changes to disk — call this once after a batch of setValue() calls.
    void flush()
    {
        propertiesFile->save();
    }

    // Legacy alias kept for existing call sites in MainComponent.cpp
    void saveNow() { flush(); }

    //==============================================================================
    // All setters below are in-memory only — they do NOT flush to disk.
    // Call flush() (or saveNow()) once after a batch of saves.

    void saveLastDirectory(const juce::File& directory)
    {
        propertiesFile->setValue("lastDirectory", directory.getFullPathName());
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
        propertiesFile->setValue("midiNote",    midiNote);
        propertiesFile->setValue("midiChannel", midiChannel);
        propertiesFile->setValue("midiDevice",  midiDevice);
    }

    void savePitchOffset(int pitchOffsetCents)
    {
        propertiesFile->setValue("pitchOffsetCents", pitchOffsetCents);
    }

    void saveVolume(float volume)
    {
        propertiesFile->setValue("volume", (double)volume);
    }

    void saveStartPoint(double startPointSeconds)
    {
        propertiesFile->setValue("startPointSeconds", startPointSeconds);
    }

    double getStartPoint()
    {
        return propertiesFile->getDoubleValue("startPointSeconds", 0.0);
    }

    void saveEndPoint(double endPointSeconds)
    {
        propertiesFile->setValue("endPointSeconds", endPointSeconds);
    }

    double getEndPoint()
    {
        return propertiesFile->getDoubleValue("endPointSeconds", -1.0);
    }

    void saveMasterVolume(float volume)
    {
        propertiesFile->setValue("masterVolume", (double)volume);
    }

    float getMasterVolume()
    {
        return (float)propertiesFile->getDoubleValue("masterVolume", 0.7);
    }

    void saveLoopEnabled(bool loopEnabled)
    {
        propertiesFile->setValue("loopEnabled", loopEnabled);
    }

    bool getLoopEnabled()
    {
        return propertiesFile->getBoolValue("loopEnabled", false);
    }

    void saveGridSnapEnabled(bool enabled)
    {
        propertiesFile->setValue("gridSnapEnabled", enabled);
    }

    bool getGridSnapEnabled()
    {
        return propertiesFile->getBoolValue("gridSnapEnabled", false);
    }

    void saveGridResolutionIndex(int index)
    {
        propertiesFile->setValue("gridResolutionIndex", index);
    }

    int getGridResolutionIndex()
    {
        return propertiesFile->getIntValue("gridResolutionIndex", 5);
    }

    void saveOneShotEnabled(bool enabled)
    {
        propertiesFile->setValue("oneShotEnabled", enabled);
    }

    bool getOneShotEnabled()
    {
        return propertiesFile->getBoolValue("oneShotEnabled", false);
    }

    void saveReverseEnabled(bool enabled)
    {
        propertiesFile->setValue("reverseEnabled", enabled);
    }

    bool getReverseEnabled()
    {
        return propertiesFile->getBoolValue("reverseEnabled", false);
    }

    void saveBounceEnabled(bool enabled)
    {
        propertiesFile->setValue("bounceEnabled", enabled);
    }

    bool getBounceEnabled()
    {
        return propertiesFile->getBoolValue("bounceEnabled", false);
    }

    void saveBaseTuningHz(double hz)
    {
        propertiesFile->setValue("baseTuningHz", hz);
    }

    double getBaseTuningHz()
    {
        return propertiesFile->getDoubleValue("baseTuningHz", 440.0);
    }

    void savePitchStepCents(int cents)
    {
        propertiesFile->setValue("pitchStepCents", cents);
    }

    int getPitchStepCents()
    {
        return propertiesFile->getIntValue("pitchStepCents", 100);
    }

    void saveTransientDetectionEnabled(bool enabled)
    {
        propertiesFile->setValue("transientDetectionEnabled", enabled);
    }

    bool getTransientDetectionEnabled()
    {
        return propertiesFile->getBoolValue("transientDetectionEnabled", true);
    }

    void saveActiveTab(int tabIndex)
    {
        propertiesFile->setValue("activeTab", tabIndex);
    }

    int getActiveTab()
    {
        return propertiesFile->getIntValue("activeTab", 0);
    }

    void saveGridResolutionMs(double ms)
    {
        propertiesFile->setValue("gridResolution", ms);
    }

    double getGridResolutionMs()
    {
        return propertiesFile->getDoubleValue("gridResolution", -1.0);
    }

    void saveZoomLevel(float zoomLevel)
    {
        propertiesFile->setValue("zoomLevel", (double)zoomLevel);
    }

    float getZoomLevel()
    {
        return (float)propertiesFile->getDoubleValue("zoomLevel", 1.0);
    }

    void saveZoomScrollPosition(float normalizedScroll)
    {
        propertiesFile->setValue("zoomScrollPosition", (double)normalizedScroll);
    }

    float getZoomScrollPosition()
    {
        return (float)propertiesFile->getDoubleValue("zoomScrollPosition", 0.0);
    }

    void saveAudioSettings(int bufferSize, double sampleRate,
                           const juce::String& deviceType,
                           const juce::String& outputDeviceName)
    {
        propertiesFile->setValue("audioBufferSize",   bufferSize);
        propertiesFile->setValue("audioSampleRate",   sampleRate);
        propertiesFile->setValue("audioDeviceType",   deviceType);
        propertiesFile->setValue("audioOutputDevice", outputDeviceName);
        // Audio settings are saved only when the device actually changes,
        // so flush immediately here (rare event, acceptable latency).
        flush();
    }

    int getMidiNote()
    {
        return propertiesFile->getIntValue("midiNote", 60);
    }

    int getMidiChannel()
    {
        return propertiesFile->getIntValue("midiChannel", 1);
    }

    juce::String getMidiDevice()
    {
        return propertiesFile->getValue("midiDevice");
    }

    int getPitchOffset()
    {
        if (propertiesFile->containsKey("pitchOffsetCents"))
            return propertiesFile->getIntValue("pitchOffsetCents", 0);
        int semitones = propertiesFile->getIntValue("pitchOffset", 0);
        return semitones * 100;
    }

    float getVolume()
    {
        return (float)propertiesFile->getDoubleValue("volume", 1.0);
    }

    int getAudioBufferSize()
    {
        return propertiesFile->getIntValue("audioBufferSize", 512);
    }

    double getAudioSampleRate()
    {
        return propertiesFile->getDoubleValue("audioSampleRate", 44100.0);
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
    struct SampleState
    {
        double startPoint         = 0.0;
        double endPoint           = -1.0;
        float  volume             = 1.0f;
        bool   loopEnabled        = false;
        double transientThreshold = 4.0;
        juce::String detectedNoteName;
        double detectedFreqHz     = 0.0;
        int    basePitchOffset    = 0;
        bool   exists             = false;
        // ADSR
        bool   adsrEnabled        = false;
        float  adsrAttackMs       = 0.0f;
        float  adsrDecayMs        = 0.0f;
        float  adsrSustain        = 1.0f;
        float  adsrReleaseMs      = 0.0f;
        // EQ (3 bands)
        bool   eqEnabled          = false;
        float  eq1Freq = 100.0f,  eq1Gain = 0.0f, eq1Q = 1.0f; int eq1Mode = 2;
        float  eq2Freq = 500.0f,  eq2Gain = 0.0f, eq2Q = 1.0f; int eq2Mode = 2;
        float  eq3Freq = 8000.0f, eq3Gain = 0.0f, eq3Q = 1.0f; int eq3Mode = 2;
        // Normalize
        bool  normEnabled  = false;
        float normTargetDb = -6.0f;
    };

    // Store per-sample values in memory only. Call flush() after this if you want an immediate write.
    void saveSampleState(const juce::File& file, const SampleState& s)
    {
        auto k = sampleKey(file);
        propertiesFile->setValue(k + "_start",     s.startPoint);
        propertiesFile->setValue(k + "_end",       s.endPoint);
        propertiesFile->setValue(k + "_vol",       (double)s.volume);
        propertiesFile->setValue(k + "_loop",      s.loopEnabled);
        propertiesFile->setValue(k + "_thresh",    s.transientThreshold);
        propertiesFile->setValue(k + "_notename",  s.detectedNoteName);
        propertiesFile->setValue(k + "_notehz",    s.detectedFreqHz);
        propertiesFile->setValue(k + "_basepitch", s.basePitchOffset);
        propertiesFile->setValue(k + "_adsren",    s.adsrEnabled);
        propertiesFile->setValue(k + "_adsratk",   (double)s.adsrAttackMs);
        propertiesFile->setValue(k + "_adsrdcy",   (double)s.adsrDecayMs);
        propertiesFile->setValue(k + "_adsrsus",   (double)s.adsrSustain);
        propertiesFile->setValue(k + "_adsrrel",   (double)s.adsrReleaseMs);
        propertiesFile->setValue(k + "_eqen",      s.eqEnabled);
        propertiesFile->setValue(k + "_eq1f",      (double)s.eq1Freq);
        propertiesFile->setValue(k + "_eq1g",      (double)s.eq1Gain);
        propertiesFile->setValue(k + "_eq1q",      (double)s.eq1Q);
        propertiesFile->setValue(k + "_eq1mode",   s.eq1Mode);
        propertiesFile->setValue(k + "_eq2f",      (double)s.eq2Freq);
        propertiesFile->setValue(k + "_eq2g",      (double)s.eq2Gain);
        propertiesFile->setValue(k + "_eq2q",      (double)s.eq2Q);
        propertiesFile->setValue(k + "_eq2mode",   s.eq2Mode);
        propertiesFile->setValue(k + "_eq3f",      (double)s.eq3Freq);
        propertiesFile->setValue(k + "_eq3g",      (double)s.eq3Gain);
        propertiesFile->setValue(k + "_eq3q",      (double)s.eq3Q);
        propertiesFile->setValue(k + "_eq3mode",   s.eq3Mode);
        propertiesFile->setValue(k + "_normen",    s.normEnabled);
        propertiesFile->setValue(k + "_normtarget",(double)s.normTargetDb);
        // No flush here — caller decides when to flush.
    }

    SampleState getSampleState(const juce::File& file) const
    {
        auto k = sampleKey(file);
        SampleState s;
        s.exists             = propertiesFile->containsKey(k + "_start");
        s.startPoint         = propertiesFile->getDoubleValue(k + "_start",    0.0);
        s.endPoint           = propertiesFile->getDoubleValue(k + "_end",      -1.0);
        s.volume             = (float)propertiesFile->getDoubleValue(k + "_vol",    1.0);
        s.loopEnabled        = propertiesFile->getBoolValue  (k + "_loop",     false);
        s.transientThreshold = propertiesFile->getDoubleValue(k + "_thresh",   4.0);
        s.detectedNoteName   = propertiesFile->getValue      (k + "_notename", "");
        s.detectedFreqHz     = propertiesFile->getDoubleValue(k + "_notehz",   0.0);
        s.basePitchOffset    = propertiesFile->getIntValue   (k + "_basepitch", 0);
        s.adsrEnabled        = propertiesFile->getBoolValue  (k + "_adsren",   false);
        s.adsrAttackMs       = (float)propertiesFile->getDoubleValue(k + "_adsratk", 0.0);
        s.adsrDecayMs        = (float)propertiesFile->getDoubleValue(k + "_adsrdcy", 0.0);
        s.adsrSustain        = (float)propertiesFile->getDoubleValue(k + "_adsrsus", 1.0);
        s.adsrReleaseMs      = (float)propertiesFile->getDoubleValue(k + "_adsrrel", 0.0);
        s.eqEnabled          = propertiesFile->getBoolValue  (k + "_eqen",     false);
        s.eq1Freq            = (float)propertiesFile->getDoubleValue(k + "_eq1f",  100.0);
        s.eq1Gain            = (float)propertiesFile->getDoubleValue(k + "_eq1g",    0.0);
        s.eq1Q               = (float)propertiesFile->getDoubleValue(k + "_eq1q",    1.0);
        s.eq1Mode            = propertiesFile->getIntValue   (k + "_eq1mode",   2);
        s.eq2Freq            = (float)propertiesFile->getDoubleValue(k + "_eq2f",  500.0);
        s.eq2Gain            = (float)propertiesFile->getDoubleValue(k + "_eq2g",    0.0);
        s.eq2Q               = (float)propertiesFile->getDoubleValue(k + "_eq2q",    1.0);
        s.eq2Mode            = propertiesFile->getIntValue   (k + "_eq2mode",   2);
        s.eq3Freq            = (float)propertiesFile->getDoubleValue(k + "_eq3f", 8000.0);
        s.eq3Gain            = (float)propertiesFile->getDoubleValue(k + "_eq3g",    0.0);
        s.eq3Q               = (float)propertiesFile->getDoubleValue(k + "_eq3q",    1.0);
        s.eq3Mode            = propertiesFile->getIntValue   (k + "_eq3mode",   2);
        s.normEnabled        = propertiesFile->getBoolValue  (k + "_normen",    false);
        s.normTargetDb       = (float)propertiesFile->getDoubleValue(k + "_normtarget", -6.0);
        return s;
    }

private:
    juce::String sampleKey(const juce::File& file) const
    {
        return "p" + juce::String::toHexString(file.getFullPathName().hashCode64());
    }

    std::unique_ptr<juce::PropertiesFile> propertiesFile;
};
