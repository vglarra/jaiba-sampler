#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <vector>

//==============================================================================
// A single timestamped pad-hit event inside a recorded pattern.
// Stored as beat-relative time so patterns are tempo-agnostic.

struct PatternEvent
{
    int    padIndex = 0;
    double beatTime = 0.0;   // beat offset from pattern start (e.g. 0.0, 0.25, 0.5 …)
};

//==============================================================================
// PadSettings — all serializable state for one sample pad.
//
// This is a plain-data struct with no audio engine, no UI references, and no
// JUCE Component inheritance.  It intentionally mirrors ConfigurationManager's
// SampleState but also carries the per-pad global settings (MIDI note, zoom,
// grid, etc.) that previously lived as flat keys in the properties file.
//
// Persistence uses pad-indexed property keys:
//   pad 0  →  "pad0_samplePath", "pad0_pitchCents", …
//   pad 1  →  "pad1_samplePath", "pad1_pitchCents", …
// This means all 16 pads can coexist in the same PropertiesFile without
// collisions, and the existing global flat keys remain intact for backward
// compatibility with the current single-pad format.
//==============================================================================

struct PadSettings
{
    // Identity
    int padIndex = 0;

    // Sample file
    juce::String sampleFilePath;   // empty = no file loaded

    //==========================================================================
    // Pitch
    int    pitchCents      = 0;      // user-visible offset in cents [-4800, +4800]
    int    basePitchOffset = 0;      // hidden correction from last Tune run (semitones)
    double baseTuningHz    = 440.0;  // A4 reference frequency [400, 480]
    int    pitchStepCents  = 100;    // Up/Down step size: 100/50/25/33

    juce::String detectedNoteName;   // e.g. "D3" — empty if Tune not yet run
    double       detectedFreqHz = 0.0;

    //==========================================================================
    // Playback modes
    bool loopEnabled    = false;
    bool oneShotEnabled = false;
    bool reverseEnabled = false;
    bool bounceEnabled  = false;

    //==========================================================================
    // Volume / normalize
    float volumeLevel  = 1.0f;
    bool  normEnabled  = false;
    float normTargetDb = -6.0f;

    //==========================================================================
    // ADSR
    bool  adsrEnabled   = false;
    float adsrAttackMs  = 0.0f;
    float adsrDecayMs   = 0.0f;
    float adsrSustain   = 1.0f;
    float adsrReleaseMs = 0.0f;

    //==========================================================================
    // EQ — three-band parametric
    bool  eqEnabled = false;
    float eq1Freq = 100.0f,  eq1Gain = 0.0f, eq1Q = 1.0f;  int eq1Mode = 2;
    float eq2Freq = 500.0f,  eq2Gain = 0.0f, eq2Q = 1.0f;  int eq2Mode = 2;
    float eq3Freq = 8000.0f, eq3Gain = 0.0f, eq3Q = 1.0f;  int eq3Mode = 2;

    // Pad gain — independent amplification/attenuation applied in EQ tab (0.0–2.0, default 1.0)
    float padGain = 1.0f;

    //==========================================================================
    // Start / end markers
    double startPointSeconds = 0.0;
    double endPointSeconds   = -1.0;  // -1.0 = full sample length

    //==========================================================================
    // Transient detection
    bool  transientDetectionEnabled = true;
    float transientThreshold        = 4.0f;

    //==========================================================================
    // Grid snap
    bool gridSnapEnabled     = false;
    int  gridResolutionIndex = 5;   // index into {1ms,10ms,50ms,100ms,500ms,1s}

    //==========================================================================
    // MIDI routing
    int          midiNote    = 60;
    int          midiChannel = 1;
    juce::String midiDevice;

    //==========================================================================
    // Waveform zoom
    double zoomLevel          = 1.0;
    float  zoomScrollPosition = 0.0f;  // normalized [0, 1]

    //==========================================================================
    // Active tab (0=Controls, 1=ADSR, 2=EQ)
    int activeTab = 0;

    //==========================================================================
    // Saved patterns — name → list of quantised events.
    // Persist via XML (kit files) only; PropertiesFile is flat key-value and can't represent them.

    std::map<juce::String, std::vector<PatternEvent>> savedPatterns;

    void addPattern(const juce::String& name, const std::vector<PatternEvent>& events)
    {
        savedPatterns[name] = events;
    }

    std::vector<PatternEvent> getPattern(const juce::String& name) const
    {
        auto it = savedPatterns.find(name);
        if (it != savedPatterns.end()) return it->second;
        return {};
    }

    juce::StringArray getPatternNames() const
    {
        juce::StringArray names;
        for (auto& kv : savedPatterns)
            names.add(kv.first);
        return names;
    }

    void removePattern(const juce::String& name)  { savedPatterns.erase(name); }
    void clearPatterns()                           { savedPatterns.clear(); }

    //==========================================================================
    // Helpers

    bool isEmpty() const { return sampleFilePath.isEmpty(); }

    void resetToDefaults()
    {
        sampleFilePath.clear();

        pitchCents      = 0;
        basePitchOffset = 0;
        baseTuningHz    = 440.0;
        pitchStepCents  = 100;
        detectedNoteName.clear();
        detectedFreqHz  = 0.0;

        loopEnabled    = false;
        oneShotEnabled = false;
        reverseEnabled = false;
        bounceEnabled  = false;

        volumeLevel  = 1.0f;
        normEnabled  = false;
        normTargetDb = -6.0f;

        adsrEnabled   = false;
        adsrAttackMs  = 0.0f;
        adsrDecayMs   = 0.0f;
        adsrSustain   = 1.0f;
        adsrReleaseMs = 0.0f;

        eqEnabled = false;
        eq1Freq = 100.0f;  eq1Gain = 0.0f; eq1Q = 1.0f; eq1Mode = 2;
        eq2Freq = 500.0f;  eq2Gain = 0.0f; eq2Q = 1.0f; eq2Mode = 2;
        eq3Freq = 8000.0f; eq3Gain = 0.0f; eq3Q = 1.0f; eq3Mode = 2;

        startPointSeconds = 0.0;
        endPointSeconds   = -1.0;

        transientDetectionEnabled = true;
        transientThreshold        = 4.0f;

        gridSnapEnabled     = false;
        gridResolutionIndex = 5;

        midiNote    = 60;
        midiChannel = 1;
        midiDevice.clear();

        zoomLevel          = 1.0;
        zoomScrollPosition = 0.0f;

        activeTab = 0;
    }

    //==========================================================================
    // XML serialization — used by .jai kit files.
    // Each pad is stored as an XmlElement with flat attributes (no "padN_" prefix).

    void saveToXml (juce::XmlElement& el) const
    {
        el.setAttribute ("samplePath",  sampleFilePath);
        el.setAttribute ("pitchCents",  pitchCents);
        el.setAttribute ("basePitch",   basePitchOffset);
        el.setAttribute ("tuningHz",    baseTuningHz);
        el.setAttribute ("pitchStep",   pitchStepCents);
        el.setAttribute ("noteName",    detectedNoteName);
        el.setAttribute ("noteHz",      detectedFreqHz);
        el.setAttribute ("loop",        loopEnabled    ? 1 : 0);
        el.setAttribute ("oneShot",     oneShotEnabled ? 1 : 0);
        el.setAttribute ("reverse",     reverseEnabled ? 1 : 0);
        el.setAttribute ("bounce",      bounceEnabled  ? 1 : 0);
        el.setAttribute ("vol",         (double)volumeLevel);
        el.setAttribute ("normEn",      normEnabled  ? 1 : 0);
        el.setAttribute ("normTarget",  (double)normTargetDb);
        el.setAttribute ("adsrEn",      adsrEnabled  ? 1 : 0);
        el.setAttribute ("adsrAtk",     (double)adsrAttackMs);
        el.setAttribute ("adsrDcy",     (double)adsrDecayMs);
        el.setAttribute ("adsrSus",     (double)adsrSustain);
        el.setAttribute ("adsrRel",     (double)adsrReleaseMs);
        el.setAttribute ("eqEn",        eqEnabled ? 1 : 0);
        el.setAttribute ("eq1f",        (double)eq1Freq);  el.setAttribute ("eq1g", (double)eq1Gain); el.setAttribute ("eq1q", (double)eq1Q); el.setAttribute ("eq1m", eq1Mode);
        el.setAttribute ("eq2f",        (double)eq2Freq);  el.setAttribute ("eq2g", (double)eq2Gain); el.setAttribute ("eq2q", (double)eq2Q); el.setAttribute ("eq2m", eq2Mode);
        el.setAttribute ("eq3f",        (double)eq3Freq);  el.setAttribute ("eq3g", (double)eq3Gain); el.setAttribute ("eq3q", (double)eq3Q); el.setAttribute ("eq3m", eq3Mode);
        el.setAttribute ("padGain",     (double)padGain);
        el.setAttribute ("startSec",    startPointSeconds);
        el.setAttribute ("endSec",      endPointSeconds);
        el.setAttribute ("transEn",     transientDetectionEnabled ? 1 : 0);
        el.setAttribute ("transThresh", (double)transientThreshold);
        el.setAttribute ("gridSnap",    gridSnapEnabled ? 1 : 0);
        el.setAttribute ("gridResIdx",  gridResolutionIndex);
        el.setAttribute ("midiNote",    midiNote);
        el.setAttribute ("midiCh",      midiChannel);
        el.setAttribute ("midiDev",     midiDevice);
        el.setAttribute ("zoom",        zoomLevel);
        el.setAttribute ("zoomScroll",  (double)zoomScrollPosition);
        el.setAttribute ("tab",         activeTab);

        // Saved patterns — stored as <Patterns><Pattern name="..."><Event .../></Pattern></Patterns>
        if (!savedPatterns.empty())
        {
            auto* patternsEl = el.createNewChildElement ("Patterns");
            for (auto& kv : savedPatterns)
            {
                auto* patEl = patternsEl->createNewChildElement ("Pattern");
                patEl->setAttribute ("name", kv.first);
                for (auto& ev : kv.second)
                {
                    auto* evEl = patEl->createNewChildElement ("Event");
                    evEl->setAttribute ("padIndex", ev.padIndex);
                    evEl->setAttribute ("beatTime", ev.beatTime);
                }
            }
        }
    }

    void loadFromXml (const juce::XmlElement& el)
    {
        sampleFilePath   = el.getStringAttribute ("samplePath",  "");
        pitchCents       = el.getIntAttribute    ("pitchCents",  0);
        basePitchOffset  = el.getIntAttribute    ("basePitch",   0);
        baseTuningHz     = el.getDoubleAttribute ("tuningHz",    440.0);
        pitchStepCents   = el.getIntAttribute    ("pitchStep",   100);
        detectedNoteName = el.getStringAttribute ("noteName",    "");
        detectedFreqHz   = el.getDoubleAttribute ("noteHz",      0.0);
        loopEnabled      = el.getIntAttribute    ("loop",        0) != 0;
        oneShotEnabled   = el.getIntAttribute    ("oneShot",     0) != 0;
        reverseEnabled   = el.getIntAttribute    ("reverse",     0) != 0;
        bounceEnabled    = el.getIntAttribute    ("bounce",      0) != 0;
        volumeLevel      = (float)el.getDoubleAttribute ("vol",        1.0);
        normEnabled      = el.getIntAttribute    ("normEn",      0) != 0;
        normTargetDb     = (float)el.getDoubleAttribute ("normTarget", -6.0);
        adsrEnabled      = el.getIntAttribute    ("adsrEn",      0) != 0;
        adsrAttackMs     = (float)el.getDoubleAttribute ("adsrAtk",    0.0);
        adsrDecayMs      = (float)el.getDoubleAttribute ("adsrDcy",    0.0);
        adsrSustain      = (float)el.getDoubleAttribute ("adsrSus",    1.0);
        adsrReleaseMs    = (float)el.getDoubleAttribute ("adsrRel",    0.0);
        eqEnabled        = el.getIntAttribute    ("eqEn",        0) != 0;
        eq1Freq  = (float)el.getDoubleAttribute ("eq1f", 100.0);  eq1Gain = (float)el.getDoubleAttribute ("eq1g", 0.0); eq1Q = (float)el.getDoubleAttribute ("eq1q", 1.0); eq1Mode = el.getIntAttribute ("eq1m", 2);
        eq2Freq  = (float)el.getDoubleAttribute ("eq2f", 500.0);  eq2Gain = (float)el.getDoubleAttribute ("eq2g", 0.0); eq2Q = (float)el.getDoubleAttribute ("eq2q", 1.0); eq2Mode = el.getIntAttribute ("eq2m", 2);
        eq3Freq  = (float)el.getDoubleAttribute ("eq3f", 8000.0); eq3Gain = (float)el.getDoubleAttribute ("eq3g", 0.0); eq3Q = (float)el.getDoubleAttribute ("eq3q", 1.0); eq3Mode = el.getIntAttribute ("eq3m", 2);
        padGain  = (float)el.getDoubleAttribute ("padGain", 1.0);
        startPointSeconds = el.getDoubleAttribute ("startSec",   0.0);
        endPointSeconds   = el.getDoubleAttribute ("endSec",    -1.0);
        transientDetectionEnabled = el.getIntAttribute    ("transEn",      1) != 0;
        transientThreshold        = (float)el.getDoubleAttribute ("transThresh", 4.0);
        gridSnapEnabled     = el.getIntAttribute ("gridSnap",   0) != 0;
        gridResolutionIndex = el.getIntAttribute ("gridResIdx", 5);
        midiNote    = el.getIntAttribute    ("midiNote", 60);
        midiChannel = el.getIntAttribute    ("midiCh",   1);
        midiDevice  = el.getStringAttribute ("midiDev",  "");
        zoomLevel          =        el.getDoubleAttribute ("zoom",       1.0);
        zoomScrollPosition = (float)el.getDoubleAttribute ("zoomScroll", 0.0);
        activeTab = el.getIntAttribute ("tab", 0);

        // Load saved patterns
        savedPatterns.clear();
        if (auto* patternsEl = el.getChildByName ("Patterns"))
        {
            for (auto* patEl : patternsEl->getChildWithTagNameIterator ("Pattern"))
            {
                const juce::String name = patEl->getStringAttribute ("name", "");
                if (name.isEmpty()) continue;
                std::vector<PatternEvent> events;
                for (auto* evEl : patEl->getChildWithTagNameIterator ("Event"))
                {
                    PatternEvent e;
                    e.padIndex = evEl->getIntAttribute    ("padIndex", 0);
                    e.beatTime = evEl->getDoubleAttribute ("beatTime", 0.0);
                    events.push_back (e);
                }
                savedPatterns[name] = std::move (events);
            }
        }
    }

    //==========================================================================
    // Persistence — pad-indexed property keys ("pad0_xxx", "pad1_xxx", …)

    void saveToProperties(juce::PropertiesFile* props) const
    {
        if (props == nullptr) return;
        const juce::String p = "pad" + juce::String(padIndex) + "_";

        props->setValue(p + "samplePath",    sampleFilePath);

        props->setValue(p + "pitchCents",    pitchCents);
        props->setValue(p + "basePitch",     basePitchOffset);
        props->setValue(p + "tuningHz",      baseTuningHz);
        props->setValue(p + "pitchStep",     pitchStepCents);
        props->setValue(p + "noteName",      detectedNoteName);
        props->setValue(p + "noteHz",        detectedFreqHz);

        props->setValue(p + "loop",          loopEnabled);
        props->setValue(p + "oneShot",       oneShotEnabled);
        props->setValue(p + "reverse",       reverseEnabled);
        props->setValue(p + "bounce",        bounceEnabled);

        props->setValue(p + "vol",           (double)volumeLevel);
        props->setValue(p + "normEn",        normEnabled);
        props->setValue(p + "normTarget",    (double)normTargetDb);

        props->setValue(p + "adsrEn",        adsrEnabled);
        props->setValue(p + "adsrAtk",       (double)adsrAttackMs);
        props->setValue(p + "adsrDcy",       (double)adsrDecayMs);
        props->setValue(p + "adsrSus",       (double)adsrSustain);
        props->setValue(p + "adsrRel",       (double)adsrReleaseMs);

        props->setValue(p + "eqEn",          eqEnabled);
        props->setValue(p + "eq1f",          (double)eq1Freq);
        props->setValue(p + "eq1g",          (double)eq1Gain);
        props->setValue(p + "eq1q",          (double)eq1Q);
        props->setValue(p + "eq1m",          eq1Mode);
        props->setValue(p + "eq2f",          (double)eq2Freq);
        props->setValue(p + "eq2g",          (double)eq2Gain);
        props->setValue(p + "eq2q",          (double)eq2Q);
        props->setValue(p + "eq2m",          eq2Mode);
        props->setValue(p + "eq3f",          (double)eq3Freq);
        props->setValue(p + "eq3g",          (double)eq3Gain);
        props->setValue(p + "eq3q",          (double)eq3Q);
        props->setValue(p + "eq3m",          eq3Mode);
        props->setValue(p + "padGain",       (double)padGain);

        props->setValue(p + "startSec",      startPointSeconds);
        props->setValue(p + "endSec",        endPointSeconds);

        props->setValue(p + "transEn",       transientDetectionEnabled);
        props->setValue(p + "transThresh",   (double)transientThreshold);

        props->setValue(p + "gridSnap",      gridSnapEnabled);
        props->setValue(p + "gridResIdx",    gridResolutionIndex);

        props->setValue(p + "midiNote",      midiNote);
        props->setValue(p + "midiCh",        midiChannel);
        props->setValue(p + "midiDev",       midiDevice);

        props->setValue(p + "zoom",          zoomLevel);
        props->setValue(p + "zoomScroll",    (double)zoomScrollPosition);

        props->setValue(p + "tab",           activeTab);
    }

    void loadFromProperties(const juce::PropertiesFile* props)
    {
        if (props == nullptr) return;
        const juce::String p = "pad" + juce::String(padIndex) + "_";

        // If the primary key doesn't exist this pad was never saved — keep defaults
        if (!props->containsKey(p + "samplePath"))
            return;

        sampleFilePath  = props->getValue      (p + "samplePath",  "");

        pitchCents      = props->getIntValue   (p + "pitchCents",  0);
        basePitchOffset = props->getIntValue   (p + "basePitch",   0);
        baseTuningHz    = props->getDoubleValue(p + "tuningHz",    440.0);
        pitchStepCents  = props->getIntValue   (p + "pitchStep",   100);
        detectedNoteName= props->getValue      (p + "noteName",    "");
        detectedFreqHz  = props->getDoubleValue(p + "noteHz",      0.0);

        loopEnabled    = props->getBoolValue   (p + "loop",        false);
        oneShotEnabled = props->getBoolValue   (p + "oneShot",     false);
        reverseEnabled = props->getBoolValue   (p + "reverse",     false);
        bounceEnabled  = props->getBoolValue   (p + "bounce",      false);

        volumeLevel  = (float)props->getDoubleValue(p + "vol",        1.0);
        normEnabled  =        props->getBoolValue  (p + "normEn",     false);
        normTargetDb = (float)props->getDoubleValue(p + "normTarget", -6.0);

        adsrEnabled   =        props->getBoolValue  (p + "adsrEn",  false);
        adsrAttackMs  = (float)props->getDoubleValue(p + "adsrAtk", 0.0);
        adsrDecayMs   = (float)props->getDoubleValue(p + "adsrDcy", 0.0);
        adsrSustain   = (float)props->getDoubleValue(p + "adsrSus", 1.0);
        adsrReleaseMs = (float)props->getDoubleValue(p + "adsrRel", 0.0);

        eqEnabled = props->getBoolValue  (p + "eqEn",  false);
        eq1Freq   = (float)props->getDoubleValue(p + "eq1f", 100.0);
        eq1Gain   = (float)props->getDoubleValue(p + "eq1g",   0.0);
        eq1Q      = (float)props->getDoubleValue(p + "eq1q",   1.0);
        eq1Mode   =        props->getIntValue   (p + "eq1m",     2);
        eq2Freq   = (float)props->getDoubleValue(p + "eq2f", 500.0);
        eq2Gain   = (float)props->getDoubleValue(p + "eq2g",   0.0);
        eq2Q      = (float)props->getDoubleValue(p + "eq2q",   1.0);
        eq2Mode   =        props->getIntValue   (p + "eq2m",     2);
        eq3Freq   = (float)props->getDoubleValue(p + "eq3f", 8000.0);
        eq3Gain   = (float)props->getDoubleValue(p + "eq3g",    0.0);
        eq3Q      = (float)props->getDoubleValue(p + "eq3q",    1.0);
        eq3Mode   =        props->getIntValue   (p + "eq3m",     2);
        padGain   = (float)props->getDoubleValue(p + "padGain", 1.0);

        startPointSeconds = props->getDoubleValue(p + "startSec", 0.0);
        endPointSeconds   = props->getDoubleValue(p + "endSec",  -1.0);

        transientDetectionEnabled = props->getBoolValue  (p + "transEn",      true);
        transientThreshold        = (float)props->getDoubleValue(p + "transThresh", 4.0);

        gridSnapEnabled     = props->getBoolValue(p + "gridSnap",   false);
        gridResolutionIndex = props->getIntValue (p + "gridResIdx", 5);

        midiNote    = props->getIntValue(p + "midiNote", 60);
        midiChannel = props->getIntValue(p + "midiCh",   1);
        midiDevice  = props->getValue   (p + "midiDev",  "");

        zoomLevel          =        props->getDoubleValue(p + "zoom",       1.0);
        zoomScrollPosition = (float)props->getDoubleValue(p + "zoomScroll", 0.0);

        activeTab = props->getIntValue(p + "tab", 0);
    }
};
