#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "PadSettings.h"

//==============================================================================
// SeqSnap — step resolution.  The order MUST match the UI combo box order.
//==============================================================================
enum class SeqSnap
{
    Quarter = 0,        // 1/4
    Eighth,             // 1/8
    EighthT,            // 1/8T
    Sixteenth,          // 1/16
    SixteenthT,         // 1/16T
    ThirtySecond,       // 1/32
    ThirtySecondT,      // 1/32T
    SixtyFourth,        // 1/64
    Free                // no snap
};

/** Steps per beat for a snap (0 = Free / no quantise). */
inline int seqSnapStepsPerBeat (SeqSnap s)
{
    switch (s)
    {
        case SeqSnap::Quarter:       return 1;
        case SeqSnap::Eighth:        return 2;
        case SeqSnap::EighthT:       return 3;
        case SeqSnap::Sixteenth:     return 4;
        case SeqSnap::SixteenthT:    return 6;
        case SeqSnap::ThirtySecond:  return 8;
        case SeqSnap::ThirtySecondT: return 12;
        case SeqSnap::SixtyFourth:   return 16;
        case SeqSnap::Free:          return 0;
    }
    return 4;
}

/** Steps per beat used to DRAW the grid.  Free mode still needs columns, so it
    falls back to 1/16 spacing — its hits are simply never snapped. */
inline int seqGridStepsPerBeat (SeqSnap s)
{
    const int n = seqSnapStepsPerBeat (s);
    return n > 0 ? n : 4;
}

inline juce::String seqSnapName (SeqSnap s)
{
    switch (s)
    {
        case SeqSnap::Quarter:       return "1/4";
        case SeqSnap::Eighth:        return "1/8";
        case SeqSnap::EighthT:       return "1/8T";
        case SeqSnap::Sixteenth:     return "1/16";
        case SeqSnap::SixteenthT:    return "1/16T";
        case SeqSnap::ThirtySecond:  return "1/32";
        case SeqSnap::ThirtySecondT: return "1/32T";
        case SeqSnap::SixtyFourth:   return "1/64";
        case SeqSnap::Free:          return "Free";
    }
    return "1/16";
}

//==============================================================================
// Tempo-source constants, shared by the session model (GjmManager) and the UI.
// A session owns a list of named tempos ("songs"); each bank points at one.
namespace TempoSourceDefaults
{
    constexpr int kDefaultIndex = 0;    // every bank starts here
    constexpr int kKitTempo     = -1;   // bank uses its own SeqPattern::bpm
}

//==============================================================================
// Tempo field interaction: drag to set, double-click to reset.
namespace SequencerTempo
{
    constexpr double kResetBpm = 120.0;
    constexpr double kMinBpm   = 20.0;
    constexpr double kMaxBpm   = 300.0;

    /** BPM after dragging `dyPixels` up from `startBpm` (positive dy = faster).
        `fine` (shift) quarters the rate for precision work. */
    inline double bpmFromDrag (double startBpm, int dyPixels, bool fine)
    {
        const double step = fine ? 0.25 : 1.0;
        return juce::jlimit (kMinBpm, kMaxBpm, startBpm + (double) dyPixels * step);
    }
}

//==============================================================================
// Metronome accent grouping, driven by the time signature.
namespace SeqClick
{
    /** Compound meters (6/8, 9/8, 12/8) get a secondary accent every 3rd click. */
    inline int compoundGroup (int numerator, int denominator)
    {
        return (denominator == 8 && numerator > 3 && numerator % 3 == 0) ? 3 : 0;
    }

    /** Which click of the bar this beat is: 0 = bar, 1 = pulse, 2 = plain beat. */
    inline int accentFor (int beat, int numerator, int denominator)
    {
        const int beats = juce::jmax (1, numerator);
        const int inBar = ((beat % beats) + beats) % beats;

        if (inBar == 0) return 0;

        const int group = compoundGroup (numerator, denominator);
        if (group > 0 && inBar % group == 0) return 1;

        return 2;
    }

    /** Click pitch for an accent level — the bar is the highest. */
    inline double pitchFor (int accent)
    {
        return accent == 0 ? 1568.0 : (accent == 1 ? 1318.5 : 1046.5);
    }
}

//==============================================================================
// Tap-tempo accumulator shared by the Rec and Seq tabs.  Keeps a small ring of
// tap timestamps and averages the intervals; returns -1 until there are two.
struct TapTempoState
{
    static constexpr int         kMaxTaps   = 8;
    static constexpr juce::int64 kTimeoutMs = 2000;

    juce::int64 times[kMaxTaps] = {};
    int         count           = 0;

    void addTap (juce::int64 nowMs)
    {
        // Reset the sequence if the gap since the last tap exceeds the timeout.
        if (count > 0 && nowMs - times[0] > kTimeoutMs)
            count = 0;

        // Shift the ring and insert the newest tap at the front.
        for (int i = kMaxTaps - 1; i > 0; --i)
            times[i] = times[i - 1];

        times[0] = nowMs;
        count    = juce::jmin (count + 1, kMaxTaps);
    }

    /** Average BPM from the collected taps, or -1 if there are fewer than two. */
    double getBpm() const
    {
        if (count < 2) return -1.0;

        double sumMs = 0.0;
        for (int i = 0; i < count - 1; ++i)
            sumMs += (double) (times[i] - times[i + 1]);

        const double avgMs = sumMs / (count - 1);
        return avgMs > 0.0 ? 60000.0 / avgMs : -1.0;
    }

    void clear() { count = 0; }
};

//==============================================================================
struct SeqHit
{
    int   tick     = 0;     // absolute ticks from the pattern start
    float velocity = 0.8f;  // 0..1
};

//==============================================================================
// A track's self-contained sound.  Captured from a pad when the track is first
// used, so the pattern keeps playing what it was recorded with, independent of
// the live pads and of which bank is currently selected.
//
// `audio` is runtime-only (never serialised) and shares the pad engine's decoded
// buffer, so detaching costs no duplicate sample RAM.
struct SeqSound
{
    bool        hasSound  = false;
    int         sourcePad = 0;    // pad it came from — label/default only
    PadSettings settings;         // includes sampleFilePath; sound-relevant fields only
    double      sampleRate = 44100.0;   // of the captured audio (runtime; rebuilt on load)
    std::shared_ptr<juce::AudioBuffer<float>> audio;

    juce::String name() const
    {
        return settings.sampleFilePath.isEmpty()
                   ? juce::String{}
                   : juce::File (settings.sampleFilePath).getFileName();
    }

    void saveToXml (juce::XmlElement& el) const
    {
        el.setAttribute ("pad", sourcePad);
        settings.saveToXml (el);   // same attribute names as <Pad>
    }

    void loadFromXml (const juce::XmlElement& el)
    {
        sourcePad = el.getIntAttribute ("pad", 0);
        settings.loadFromXml (el);
        hasSound = settings.sampleFilePath.isNotEmpty();
    }
};

//==============================================================================
struct SeqTrack
{
    float volume = 1.0f;   // mixer fader — always live, layered over the captured sound
    bool  mute   = false;
    SeqSound    sound;
    std::vector<SeqHit> hits;

    void saveToXml (juce::XmlElement& el, int index) const
    {
        el.setAttribute ("index",  index);
        el.setAttribute ("volume", (double) volume);
        el.setAttribute ("mute",   mute ? 1 : 0);

        if (sound.hasSound)
        {
            auto* se = el.createNewChildElement ("Sound");
            sound.saveToXml (*se);
        }

        for (const auto& h : hits)
        {
            auto* he = el.createNewChildElement ("Hit");
            he->setAttribute ("tick", h.tick);
            he->setAttribute ("vel",  (double) h.velocity);
        }
    }

    void loadFromXml (const juce::XmlElement& el)
    {
        volume = (float) el.getDoubleAttribute ("volume", 1.0);
        mute   = el.getIntAttribute ("mute", 0) != 0;
        hits.clear();
        sound = SeqSound{};

        for (auto* child : el.getChildIterator())
        {
            if (child->getTagName() == "Sound")
                sound.loadFromXml (*child);
            else if (child->getTagName() == "Hit")
            {
                SeqHit h;
                h.tick     = child->getIntAttribute ("tick", 0);
                h.velocity = (float) child->getDoubleAttribute ("vel", 0.8);
                hits.push_back (h);
            }
        }
    }
};

//==============================================================================
// SeqPattern — one per bank/kit.  Serialised into the bank's .jai as
// <Sequencer>, so a kit carries its groove as well as its sounds.
//==============================================================================
struct SeqPattern
{
    static constexpr int kTracks = 16;

    SeqSnap snap        = SeqSnap::Sixteenth;
    int     bars        = 1;      // bars of the signature below
    int     sigNum      = 4;      // time signature numerator (beats per bar)
    int     sigDen      = 4;      // time signature denominator (beat unit)
    int     ppq         = 960;
    double  bpm         = 120.0;  // this kit's own tempo — used when the session's
                                  // tempo source for the bank is "Kit" (tempoGroup == -1)
    float   clickVolume = 0.5f;   // bank-level metronome level
    bool    liveMode    = true;   // Live (read-only) vs Arrange
    bool    locked      = false;

    std::array<SeqTrack, kTracks> tracks;

    //==========================================================================
    int ticksPerStep() const { return ppq / juce::jmax (1, seqGridStepsPerBeat (snap)); }

    /** Steps in one bar of THIS signature.  A bar holds (num*4/den) quarter notes
        and each quarter note holds seqGridStepsPerBeat() grid steps -- so 4/4 at
        1/16 is 16 steps, 3/4 is 12, 6/8 is 12, 7/8 is 14. */
    int stepsPerBar() const
    {
        const int num = juce::jmax (1, sigNum);
        const int den = juce::jmax (1, sigDen);
        const int spq = juce::jmax (1, seqGridStepsPerBeat (snap));

        return juce::jmax (1, (num * spq * 4 + den / 2) / den);
    }
    int totalSteps()   const { return juce::jmax (1, bars) * stepsPerBar(); }
    int totalTicks()   const { return totalSteps() * ticksPerStep(); }

    /** Quantises a tick to the current snap.  Free mode returns it unchanged. */
    int snapTick (int tick) const
    {
        if (snap == SeqSnap::Free)
            return tick;

        const int tps = juce::jmax (1, ticksPerStep());
        return (int) std::llround ((double) tick / (double) tps) * tps;
    }

    bool hasAnyHits() const
    {
        for (const auto& t : tracks)
            if (! t.hits.empty()) return true;
        return false;
    }

    //==========================================================================
    void saveToXml (juce::XmlElement& el) const
    {
        el.setAttribute ("snap",     (int) snap);
        el.setAttribute ("bars",     bars);
        el.setAttribute ("sigNum",   sigNum);
        el.setAttribute ("sigDen",   sigDen);
        el.setAttribute ("ppq",      ppq);
        el.setAttribute ("bpm",      bpm);
        el.setAttribute ("clickVol", (double) clickVolume);
        el.setAttribute ("live",     liveMode ? 1 : 0);
        el.setAttribute ("locked",   locked   ? 1 : 0);

        for (int i = 0; i < kTracks; ++i)
        {
            const auto& t = tracks[i];

            // Skip untouched tracks so the kit file stays small.
            if (! t.sound.hasSound && t.hits.empty()
                && t.volume == 1.0f && ! t.mute)
                continue;

            auto* te = el.createNewChildElement ("Track");
            t.saveToXml (*te, i);
        }
    }

    void loadFromXml (const juce::XmlElement& el)
    {
        *this = SeqPattern{};

        snap        = (SeqSnap) juce::jlimit (0, (int) SeqSnap::Free,
                                              el.getIntAttribute ("snap", (int) SeqSnap::Sixteenth));
        bars        = juce::jlimit (1, 32, el.getIntAttribute ("bars", 1));
        sigNum      = juce::jlimit (1, 32, el.getIntAttribute ("sigNum", 4));
        sigDen      = juce::jlimit (1, 32, el.getIntAttribute ("sigDen", 4));
        ppq         = juce::jmax (24, el.getIntAttribute ("ppq", 960));
        bpm         = juce::jlimit (20.0, 300.0, el.getDoubleAttribute ("bpm", 120.0));
        clickVolume = juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("clickVol", 0.5));
        liveMode    = el.getIntAttribute ("live", 1) != 0;
        locked      = el.getIntAttribute ("locked", 0) != 0;

        for (auto* child : el.getChildIterator())
        {
            if (child->getTagName() != "Track") continue;

            const int idx = child->getIntAttribute ("index", -1);
            if (idx < 0 || idx >= kTracks) continue;

            tracks[idx].loadFromXml (*child);
        }
    }
};
