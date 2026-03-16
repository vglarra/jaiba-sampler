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
    
    void savePitchOffset(int pitchOffset)
    {
        propertiesFile->setValue("pitchOffset", pitchOffset);
        propertiesFile->saveIfNeeded();
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
        return propertiesFile->getIntValue("pitchOffset", 0); // Default to 0 (no transposition)
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
    
private:
    std::unique_ptr<juce::PropertiesFile> propertiesFile;
};


