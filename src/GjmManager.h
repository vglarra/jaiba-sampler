#pragma once

#include <array>
#include <juce_core/juce_core.h>
#include "PadSettings.h"

//==============================================================================
// GjmBank — one slot in a 16-bank Global Jaiba Map
//==============================================================================
struct GjmBank
{
    juce::String               kitFilePath;   // resolved absolute path to .jai (empty = unassigned)
    juce::String               displayName;   // short name for UI (file stem by default)
    std::array<PadSettings, 16> pads;         // cached pad settings populated after parse
    bool                        isReady = false;

    GjmBank()
    {
        for (int i = 0; i < 16; ++i)
            pads[i].padIndex = i;
    }

    bool hasFile() const { return kitFilePath.isNotEmpty(); }

    void reset()
    {
        kitFilePath.clear();
        displayName.clear();
        isReady = false;
        for (int i = 0; i < 16; ++i) { pads[i] = PadSettings{}; pads[i].padIndex = i; }
    }
};

//==============================================================================
// GjmManager — owns a 16-bank Global Jaiba Map manifest and its cached pads
//
// Threading model (important — read before modifying):
//
//   loadManifest()   — message thread only, fast XML parse of manifest
//   parseAllBanks()  — called from ONE background job queued by loadGjmFromFile();
//                      writes to banks[i].pads sequentially, sets isReady=true
//                      when finished calls callAsync(finishGjmLoad)
//   ensureBankReady()— message thread only fallback (nav buttons disabled during
//                      background parse so this is only a safety net)
//   switchGjmBank()  — message thread only, reads banks[i].pads (safe because
//                      called only after finishGjmLoad() / ensureBankReady())
//   saveManifest() / saveBankKit() — message thread only, fast XML writes
//
//   MIDI routing atomics and padManager.padSettings are updated on the message
//   thread inside switchGjmBank() — no audio-thread access to gjmManager.
//==============================================================================
class GjmManager
{
public:
    GjmManager() = default;

    static constexpr int kNumBanks      = 16;
    static constexpr int kNumGlobalPads = 5;   // persistent loop pads in .gjm file

    std::array<GjmBank, kNumBanks> banks;
    PadSettings globalPads[kNumGlobalPads];    // saved/loaded from <GlobalLoopPads> section

    juce::File gjmFile;
    int  activeBank = 0;   // 0-based
    bool isLoaded   = false;
    bool isUntitled = true;
    bool isDirty    = false;

    //==========================================================================
    // Reset to an empty untitled in-memory session (does not touch disk).
    void reset()
    {
        for (auto& b : banks) b.reset();
        for (int i = 0; i < kNumGlobalPads; ++i)
        {
            globalPads[i] = PadSettings{};
            globalPads[i].padIndex    = kNumBanks + i;  // indices 16-20
            // Disable MIDI routing for global pads in new sessions — avoids collisions
            // with kit pad defaults (midiNote=60 on all).
            globalPads[i].midiNote    = -1;
            globalPads[i].midiChannel = -1;
        }
        gjmFile    = juce::File{};
        activeBank = 0;
        isLoaded   = true;
        isUntitled = true;
        isDirty    = false;
    }

    //==========================================================================
    // Thread-safe static .jai parser — call from any thread.
    // Returns all-default PadSettings for missing or invalid files.
    static std::array<PadSettings, 16> parseKitFile (const juce::File& file)
    {
        std::array<PadSettings, 16> result;
        for (int i = 0; i < 16; ++i)
            result[i].padIndex = i;

        if (!file.existsAsFile()) return result;

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || (xml->getTagName() != "JaibaKit" && xml->getTagName() != "JaivaKit")) return result;

        for (auto* padEl : xml->getChildIterator())
        {
            if (padEl->getTagName() != "Pad") continue;
            const int idx = padEl->getIntAttribute ("index", -1);
            if (idx < 0 || idx >= 16) continue;
            result[idx].loadFromXml (*padEl);
            result[idx].padIndex = idx;
        }
        return result;
    }

    //==========================================================================
    // Parse the manifest XML, populate banks[].kitFilePath and displayName.
    // Does NOT load kit files — call ensureBankReady() or parseAllBanks() next.
    bool loadManifest (const juce::File& file)
    {
        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || (xml->getTagName() != "GlobalJaibaMap" && xml->getTagName() != "GlobalJaivaMap"))
            return false;

        gjmFile    = file;
        isLoaded   = false;
        activeBank = 0;

        for (auto& b : banks)
            b.reset();

        // Reset global pads to MIDI-disabled before loading so any pad absent from
        // the XML does not accidentally inherit the PadSettings default of midiNote=60.
        for (int i = 0; i < kNumGlobalPads; ++i)
        {
            globalPads[i] = PadSettings{};
            globalPads[i].padIndex    = kNumBanks + i;
            globalPads[i].midiNote    = -1;
            globalPads[i].midiChannel = -1;
        }

        for (auto* bankEl : xml->getChildIterator())
        {
            if (bankEl->getTagName() == "Bank")
            {
                const int idx = bankEl->getIntAttribute ("index", -1);
                if (idx < 0 || idx >= kNumBanks) continue;
                banks[idx].kitFilePath = resolveKitPath (bankEl->getStringAttribute ("kitPath"), file).getFullPathName();
                banks[idx].displayName = bankEl->getStringAttribute (
                    "name", "Bank " + juce::String (idx + 1));
            }
            else if (bankEl->getTagName() == "GlobalLoopPads")
            {
                for (auto* padEl : bankEl->getChildIterator())
                {
                    if (padEl->getTagName() != "GlobalPad") continue;
                    const int idx = padEl->getIntAttribute ("index", -1);
                    if (idx < 0 || idx >= kNumGlobalPads) continue;
                    globalPads[idx].loadFromXml (*padEl);
                    globalPads[idx].padIndex = kNumBanks + idx;
                }
            }
        }

        isLoaded   = true;
        isUntitled = false;
        isDirty    = false;
        return true;
    }

    //==========================================================================
    // Parse all assigned bank kit files sequentially.
    // Safe to call from a background thread — no other thread touches banks[].pads.
    void parseAllBanks()
    {
        for (int i = 0; i < kNumBanks; ++i)
            ensureBankReady (i);
    }

    //==========================================================================
    // Synchronously ensure one bank's pads are populated.
    // Message thread only (fast XML parse, <5 ms per bank).
    void ensureBankReady (int bankIdx)
    {
        if (bankIdx < 0 || bankIdx >= kNumBanks) return;
        auto& b = banks[bankIdx];
        if (b.isReady || !b.hasFile()) return;
        b.pads    = parseKitFile (juce::File (b.kitFilePath));
        b.isReady = true;
    }

    //==========================================================================
    // Save the GJM manifest XML (kit file paths + names).
    bool saveManifest (const juce::File& file) const
    {
        auto root = std::make_unique<juce::XmlElement> ("GlobalJaibaMap");
        root->setAttribute ("version",   1);
        root->setAttribute ("savedDate", juce::Time::getCurrentTime().toString (true, true));

        for (int i = 0; i < kNumBanks; ++i)
        {
            auto* bankEl = root->createNewChildElement ("Bank");
            bankEl->setAttribute ("index",   i);
            bankEl->setAttribute ("kitPath", storeKitPath (juce::File (banks[i].kitFilePath), file));
            bankEl->setAttribute ("name",    banks[i].displayName);
        }

        auto* globalEl = root->createNewChildElement ("GlobalLoopPads");
        for (int i = 0; i < kNumGlobalPads; ++i)
        {
            auto* padEl = globalEl->createNewChildElement ("GlobalPad");
            padEl->setAttribute ("index", i);
            globalPads[i].saveToXml (*padEl);
        }

        return root->writeTo (file);
    }

    //==========================================================================
    // Write one bank's pads to its .jai file, auto-generating a filename if
    // the bank has no kitFilePath yet (uses gjmFile's directory).
    juce::File saveBankKit (int bankIdx, const juce::File& gjmFileDest)
    {
        jassert (bankIdx >= 0 && bankIdx < kNumBanks);
        auto& b = banks[bankIdx];

        if (b.kitFilePath.isEmpty())
        {
            const juce::String stem = gjmFileDest.getFileNameWithoutExtension()
                + "_bank" + juce::String (bankIdx + 1).paddedLeft ('0', 2);
            b.kitFilePath = gjmFileDest.getParentDirectory()
                                       .getChildFile (stem + ".jai")
                                       .getFullPathName();
            b.displayName = stem;
        }

        juce::File kitFile (b.kitFilePath);
        auto root = std::make_unique<juce::XmlElement> ("JaibaKit");
        root->setAttribute ("version",   1);
        root->setAttribute ("savedDate", juce::Time::getCurrentTime().toString (true, true));
        for (int i = 0; i < 16; ++i)
        {
            auto* padEl = root->createNewChildElement ("Pad");
            padEl->setAttribute ("index", i);
            b.pads[i].saveToXml (*padEl);
        }
        root->writeTo (kitFile);
        b.isReady = true;
        return kitFile;
    }

    //==========================================================================
    int numBanksWithFiles() const
    {
        int n = 0;
        for (auto& b : banks) if (b.hasFile()) ++n;
        return n;
    }

    juce::String activeBankName() const
    {
        if (!isLoaded) return "No GJM";
        const auto& b = banks[activeBank];
        return b.displayName.isNotEmpty() ? b.displayName : ("Bank " + juce::String (activeBank + 1));
    }

private:
    static juce::File resolveKitPath (const juce::String& stored, const juce::File& gjmFileSrc)
    {
        if (stored.isEmpty()) return {};
        juce::File f (stored);
        if (f.existsAsFile()) return f;
        juce::File rel = gjmFileSrc.getParentDirectory().getChildFile (stored);
        if (rel.existsAsFile()) return rel;
        return juce::File (stored);   // keep as-is even if currently missing
    }

    static juce::String storeKitPath (const juce::File& kitFile, const juce::File& gjmFileDest)
    {
        if (kitFile == juce::File{}) return {};
        const juce::String rel = kitFile.getRelativePathFrom (gjmFileDest.getParentDirectory());
        return rel.isNotEmpty() ? rel : kitFile.getFullPathName();
    }

    GjmManager (const GjmManager&) = delete;
    GjmManager& operator= (const GjmManager&) = delete;
};
