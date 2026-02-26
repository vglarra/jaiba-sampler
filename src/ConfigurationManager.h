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
    
private:
    std::unique_ptr<juce::PropertiesFile> propertiesFile;
};