#pragma once

#include <array>
#include <vector>
#include <juce_core/juce_core.h>
#include "PadSettings.h"
#include "Sequencer.h"

//==============================================================================
// A named, portable tempo owned by the session — i.e. a "song".  Banks point at
// one through GjmBank::tempoGroup; several banks sharing a source are tied to
// the same tempo, which is what makes them one composition.
struct TempoSource
{
    juce::String name = "Song 1";
    double       bpm  = 120.0;
};

//==============================================================================
// GjmBank — one slot in a 16-bank Global Jaiba Map
//==============================================================================
struct GjmBank
{
    juce::String               kitFilePath;   // resolved absolute path to .jai (empty = unassigned)
    juce::String               displayName;   // short name for UI (file stem by default)
    std::array<PadSettings, 16> pads;         // cached pad settings populated after parse
    SeqPattern                  sequence;     // per-bank step pattern (from <Sequencer>)
    int                         tempoGroup = 0;   // index into tempoSources; kKitTempo = the kit's own
    bool                        isReady = false;
    bool                        isDirty = false;  // unsaved edits to this bank's kit (not persisted)

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
        isDirty = false;
        tempoGroup = TempoSourceDefaults::kDefaultIndex;
        sequence = SeqPattern{};
        for (int i = 0; i < 16; ++i) { pads[i] = PadSettings{}; pads[i].padIndex = i; }
    }
};

//==============================================================================
// Everything a .jai kit file holds.
struct KitData
{
    std::array<PadSettings, 16> pads;
    SeqPattern                  sequence;

    KitData()
    {
        for (int i = 0; i < 16; ++i)
            pads[i].padIndex = i;
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

    // Session tempos ("songs").  Always holds at least one entry: index 0 is the
    // default every bank starts on, which is what makes a session behave as one
    // global tempo until songs are created deliberately.
    std::vector<TempoSource> tempoSources;

    // Transport / mix options that belong to the session.
    bool  syncClickToSeq  = true;    // lock the metronome's phase to the sequencer
    float seqMasterVolume = 1.0f;    // balance the whole sequencer pool

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
        tempoSources.clear();
        tempoSources.push_back (TempoSource{ "Song 1", 120.0 });
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
        syncClickToSeq  = true;
        seqMasterVolume = 1.0f;
    }

    //==========================================================================
    // Tempo resolution: a bank's BPM follows its tempo source.
    // `kitBpmOverride` supplies the live pattern tempo for the ACTIVE bank, whose
    // cached banks[].sequence may be one edit behind (seqPattern is the live copy).
    double tempoForBank (int bankIdx, double kitBpmOverride = -1.0) const
    {
        if (bankIdx < 0 || bankIdx >= kNumBanks) return 120.0;

        const auto& b = banks[bankIdx];

        if (b.tempoGroup == TempoSourceDefaults::kKitTempo)
            return kitBpmOverride > 0.0 ? kitBpmOverride : b.sequence.bpm;

        if (b.tempoGroup >= 0 && b.tempoGroup < (int) tempoSources.size())
            return tempoSources[(size_t) b.tempoGroup].bpm;

        return tempoSources.empty() ? 120.0 : tempoSources.front().bpm;
    }

    /** "Kit" or the source's name, for the UI. */
    juce::String tempoSourceName (int bankIdx) const
    {
        if (bankIdx < 0 || bankIdx >= kNumBanks) return {};

        const int g = banks[bankIdx].tempoGroup;
        if (g == TempoSourceDefaults::kKitTempo) return "Kit";

        if (g >= 0 && g < (int) tempoSources.size())
            return tempoSources[(size_t) g].name;

        return tempoSources.empty() ? juce::String{} : tempoSources.front().name;
    }

    //==========================================================================
    // Thread-safe static .jai parser — call from any thread.
    // Returns all-default PadSettings / empty pattern for missing or invalid files.
    static KitData parseKit (const juce::File& file)
    {
        KitData result;

        if (!file.existsAsFile()) return result;

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || (xml->getTagName() != "JaibaKit" && xml->getTagName() != "JaivaKit")) return result;

        for (auto* padEl : xml->getChildIterator())
        {
            const juce::String tag = padEl->getTagName();

            if (tag == "Pad")
            {
                const int idx = padEl->getIntAttribute ("index", -1);
                if (idx < 0 || idx >= 16) continue;
                result.pads[idx].loadFromXml (*padEl);
                result.pads[idx].padIndex = idx;
            }
            else if (tag == "Sequencer")
            {
                result.sequence.loadFromXml (*padEl);
            }
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

        // Default tempo source; replaced below if the session defines <Tempos>.
        tempoSources.clear();
        tempoSources.push_back (TempoSource{ "Song 1", 120.0 });

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
                banks[idx].tempoGroup = bankEl->getIntAttribute (
                    "tempoGroup", TempoSourceDefaults::kDefaultIndex);
            }
            else if (bankEl->getTagName() == "Tempos")
            {
                std::vector<TempoSource> loaded;
                for (auto* t : bankEl->getChildIterator())
                {
                    if (t->getTagName() != "Tempo") continue;
                    TempoSource src;
                    src.name = t->getStringAttribute (
                        "name", "Song " + juce::String ((int) loaded.size() + 1));
                    src.bpm  = juce::jlimit (20.0, 300.0, t->getDoubleAttribute ("bpm", 120.0));
                    loaded.push_back (src);
                }
                if (! loaded.empty())
                    tempoSources = std::move (loaded);
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

        // Transport / mix options (absent in older sessions -> defaults).
        syncClickToSeq  = xml->getIntAttribute ("syncClick", 1) != 0;
        seqMasterVolume = juce::jlimit (0.0f, 1.0f,
                                        (float) xml->getDoubleAttribute ("seqVolume", 1.0));

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
        auto kit     = parseKit (juce::File (b.kitFilePath));
        b.pads       = std::move (kit.pads);
        b.sequence   = std::move (kit.sequence);
        b.isReady    = true;
    }

    //==========================================================================
    // Save the GJM manifest XML (kit file paths + names).
    bool saveManifest (const juce::File& file) const
    {
        auto root = std::make_unique<juce::XmlElement> ("GlobalJaibaMap");
        root->setAttribute ("version",   1);
        root->setAttribute ("savedDate", juce::Time::getCurrentTime().toString (true, true));
        root->setAttribute ("syncClick", syncClickToSeq ? 1 : 0);
        root->setAttribute ("seqVolume", (double) seqMasterVolume);

        // Session tempos ("songs"), referenced by each bank's tempoGroup.
        {
            auto* temposEl = root->createNewChildElement ("Tempos");
            temposEl->setAttribute ("default", TempoSourceDefaults::kDefaultIndex);
            for (size_t t = 0; t < tempoSources.size(); ++t)
            {
                auto* tempoEl = temposEl->createNewChildElement ("Tempo");
                tempoEl->setAttribute ("index", (int) t);
                tempoEl->setAttribute ("name",  tempoSources[t].name);
                tempoEl->setAttribute ("bpm",   tempoSources[t].bpm);
            }
        }

        for (int i = 0; i < kNumBanks; ++i)
        {
            auto* bankEl = root->createNewChildElement ("Bank");
            bankEl->setAttribute ("index",      i);
            bankEl->setAttribute ("kitPath",    storeKitPath (juce::File (banks[i].kitFilePath), file));
            bankEl->setAttribute ("name",       banks[i].displayName);
            bankEl->setAttribute ("tempoGroup", banks[i].tempoGroup);
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

        // Per-bank step pattern travels with the kit.
        auto* seqEl = root->createNewChildElement ("Sequencer");
        b.sequence.saveToXml (*seqEl);

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

    // True when any bank's kit has unsaved edits (cleared by Save Bank Kit / Save Session).
    bool anyBankDirty() const
    {
        for (const auto& b : banks) if (b.isDirty) return true;
        return false;
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

        // Kit paths are stored relative to the .gjm when possible (see storeKitPath),
        // so resolve them against its folder.  getChildFile() returns an absolute
        // path verbatim, so absolute stored paths work too -- and unlike constructing
        // juce::File from a bare relative string, it does not trip the debug
        // assertion that raw File paths must be absolute.
        return gjmFileSrc.getParentDirectory().getChildFile (stored);
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
