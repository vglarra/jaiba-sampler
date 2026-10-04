// Sequencer model tests — no audio device needed.
//
// Covers the parts of the Seq feature that are pure logic: metronome accent
// grouping, session tempo sources ("songs"), session transport/mix options and
// the kit pattern round-trip.
//
// Build + run: tests/run_tests.sh   (needs a prior `ninja -C build`)
#include <juce_data_structures/juce_data_structures.h>

#include "Sequencer.h"
#include "GjmManager.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

static bool near (double a, double b, double tol = 1e-6)
{
    return std::abs (a - b) <= tol;
}

/** Accent levels for `beats` consecutive clicks, as a compact string. */
static juce::String accents (int beats, int num, int den)
{
    juce::String r;
    for (int b = 0; b < beats; ++b)
        r += juce::String (SeqClick::accentFor (b, num, den));
    return r;
}

int main()
{
    //==========================================================================
    // Metronome accent grouping (time signature)
    // 0 = bar accent, 1 = compound pulse, 2 = plain beat.
    //==========================================================================
    {
        check (accents (8, 4, 4)  == "02220222",       "4/4 accents every 4th click");
        check (accents (6, 3, 4)  == "022022",         "3/4 accents every 3rd click");
        check (accents (6, 2, 4)  == "020202",         "2/4 accents every 2nd click");
        check (accents (10, 5, 4) == "0222202222",     "5/4 accents every 5th click");
        check (accents (14, 7, 8) == "02222220222222", "7/8 accents the bar only");

        // Compound meters: bar accent plus a pulse every 3rd click.
        check (accents (12, 6, 8) == "022122022122",
               "6/8 accents the bar and every dotted-quarter pulse");
        check (accents (18, 9, 8) == "022122122022122122",
               "9/8 accents the bar and every dotted-quarter pulse");
        check (accents (4, 6, 8)  == "0221",
               "6/8 grouping wraps correctly at an arbitrary beat");

        check (SeqClick::accentFor (-1, 4, 4) == 2,
               "negative beats do not break the grouping");
        check (SeqClick::pitchFor (0) > SeqClick::pitchFor (1)
               && SeqClick::pitchFor (1) > SeqClick::pitchFor (2),
               "bar accent is the highest pitch, then the pulse, then the beat");
        check (SeqClick::pitchFor (2) < SeqClick::pitchFor (0),
               "an unaccented click sits below the bar accent");
    }

    //==========================================================================
    // Tempo field: drag to set, double-click to reset
    //==========================================================================
    {
        check (near (SequencerTempo::kResetBpm, 120.0), "double-click reset lands on 120");

        // Dragging up is faster, down is slower.
        check (near (SequencerTempo::bpmFromDrag (120.0, 10, false), 130.0),
               "dragging up 10 px adds 10 BPM");
        check (near (SequencerTempo::bpmFromDrag (120.0, -10, false), 110.0),
               "dragging down 10 px subtracts 10 BPM");
        check (near (SequencerTempo::bpmFromDrag (120.0, 0, false), 120.0),
               "no drag leaves the tempo alone");

        // Shift is fine-grained.
        check (near (SequencerTempo::bpmFromDrag (120.0, 10, true), 122.5),
               "shift drag is quarter-rate");

        // Clamped to the supported range in both directions.
        check (near (SequencerTempo::bpmFromDrag (120.0, 100000, false), 300.0),
               "drag clamps at the top of the range");
        check (near (SequencerTempo::bpmFromDrag (120.0, -100000, false), 20.0),
               "drag clamps at the bottom of the range");
    }

    //==========================================================================
    // Session tempo sources ("songs") and transport/mix options
    //==========================================================================
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("jaiba_seq_model_test.gjm");
        tmp.deleteFile();

        GjmManager m;
        m.reset();

        check (m.tempoSources.size() == 1, "session starts with one tempo source");
        check (m.tempoSources[0].name == "Song 1", "default source is named \"Song 1\"");
        check (near (m.tempoSources[0].bpm, 120.0), "default source is 120 BPM");

        // Three banks tied to a second song; one on the kit's own tempo.
        m.tempoSources.push_back ({ "Song 2", 95.0 });
        m.banks[0].tempoGroup = 0;
        m.banks[1].tempoGroup = 1;
        m.banks[2].tempoGroup = 1;
        m.banks[3].tempoGroup = TempoSourceDefaults::kKitTempo;
        m.banks[3].sequence.bpm = 140.0;

        check (near (m.tempoForBank (0), 120.0), "bank 0 follows the default song");
        check (near (m.tempoForBank (1), 95.0),  "bank 1 follows Song 2");
        check (near (m.tempoForBank (2), 95.0),  "bank 2 is tied to Song 2");
        check (near (m.tempoForBank (3), 140.0), "bank 3 uses its kit tempo");
        check (near (m.tempoForBank (3, 133.0), 133.0),
               "kit-tempo override wins (live pattern)");
        check (m.tempoSourceName (1) == "Song 2", "source name resolves");
        check (m.tempoSourceName (3) == "Kit",    "kit source names as Kit");

        // Session options.
        m.syncClickToSeq  = false;
        m.seqMasterVolume = 0.42f;
        m.timeSig         = "9/8";
        check (m.timeSigNumerator() == 9 && m.timeSigDenominator() == 8,
               "time signature parses");

        check (m.saveManifest (tmp), "session manifest writes");

        GjmManager r;
        r.reset();
        check (r.loadManifest (tmp), "session manifest re-parses");
        check (r.tempoSources.size() == 2, "both sources round-trip");
        check (r.tempoSources[0].name == "Song 1" && near (r.tempoSources[0].bpm, 120.0),
               "source 0 name + bpm round-trip");
        check (r.tempoSources[1].name == "Song 2" && near (r.tempoSources[1].bpm, 95.0),
               "source 1 name + bpm round-trip");
        check (r.banks[1].tempoGroup == 1 && r.banks[2].tempoGroup == 1,
               "tied banks round-trip");
        check (r.banks[3].tempoGroup == TempoSourceDefaults::kKitTempo,
               "kit-tempo bank round-trips");
        check (! r.syncClickToSeq, "click-sync option round-trips");
        check (near ((double) r.seqMasterVolume, 0.42), "sequencer master level round-trips");
        check (r.timeSig == "9/8", "time signature round-trips");

        // An out-of-range group falls back to the default source.
        r.banks[5].tempoGroup = 99;
        check (near (r.tempoForBank (5), 120.0), "out-of-range group falls back to default");

        // A malformed signature must not divide by zero.
        r.timeSig = "garbage";
        check (r.timeSigNumerator() >= 1 && r.timeSigDenominator() == 4,
               "malformed time signature degrades safely");

        // ---- Backward compatibility: an old manifest with no <Tempos> ----
        const auto oldFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("jaiba_seq_old_session.gjm");
        {
            juce::XmlElement root ("GlobalJaibaMap");
            auto* b0 = root.createNewChildElement ("Bank");
            b0->setAttribute ("index", 0);
            b0->setAttribute ("kitPath", "kit-001.jai");
            b0->setAttribute ("name", "Legacy");
            root.writeTo (oldFile);
        }

        GjmManager old;
        check (old.loadManifest (oldFile), "legacy manifest loads");
        check (old.tempoSources.size() == 1 && old.tempoSources[0].name == "Song 1",
               "legacy session gets one default song");
        check (old.banks[0].tempoGroup == TempoSourceDefaults::kDefaultIndex,
               "legacy banks default to the default source");
        check (near (old.tempoForBank (0), 120.0),
               "legacy session behaves as one global tempo");
        check (old.syncClickToSeq && near ((double) old.seqMasterVolume, 1.0)
               && old.timeSig == "4/4",
               "legacy session gets sensible option defaults");

        tmp.deleteFile();
        oldFile.deleteFile();
    }

    //==========================================================================
    // Kit pattern + captured track sound round-trip (the .jai side)
    //==========================================================================
    {
        SeqPattern p;
        p.snap = SeqSnap::Sixteenth;
        p.bars = 2;
        p.ppq  = 960;
        p.bpm  = 137.0;
        p.clickVolume = 0.33f;
        p.liveMode = false;

        p.tracks[0].volume = 0.75f;
        p.tracks[0].mute   = true;
        p.tracks[0].hits.push_back ({ 0,   0.9f });
        p.tracks[0].hits.push_back ({ 240, 0.4f });

        p.tracks[3].sound.hasSound  = true;
        p.tracks[3].sound.sourcePad = 3;
        p.tracks[3].sound.settings.sampleFilePath = "/tmp/kick.wav";
        p.tracks[3].sound.settings.volumeLevel    = 0.8f;
        p.tracks[3].sound.settings.eqEnabled      = true;
        p.tracks[3].sound.settings.eq1Freq        = 120.0f;
        p.tracks[3].hits.push_back ({ 480, 1.0f });

        check (p.totalSteps() == 32, "2 bars at 1/16 is 32 steps");
        check (p.totalTicks() == 32 * 240, "step count converts to ticks");

        juce::XmlElement xml ("Sequencer");
        p.saveToXml (xml);

        SeqPattern q;
        q.loadFromXml (xml);
        check (q.tracks[0].hits.size() == 2, "pattern XML loads back at all");
        check (q.bars == 2 && q.ppq == 960, "bars + ppq round-trip");
        check (near (q.bpm, 137.0), "kit tempo round-trips");
        check (near ((double) q.clickVolume, 0.33), "click volume round-trips");
        check (q.liveMode == false, "mode round-trips");
        check (near ((double) q.tracks[0].volume, 0.75), "track volume round-trips");
        check (q.tracks[0].mute, "track mute round-trips");
        check (q.tracks[0].hits.size() == 2, "track hits round-trip");
        check (q.tracks[0].hits[1].tick == 240, "hit tick round-trips");
        check (near ((double) q.tracks[0].hits[1].velocity, 0.4, 1e-3),
               "hit velocity round-trips");
        check (q.tracks[3].sound.hasSound, "captured sound flag round-trips");
        check (q.tracks[3].sound.settings.sampleFilePath == "/tmp/kick.wav",
               "captured sample path round-trips");
        check (near ((double) q.tracks[3].sound.settings.volumeLevel, 0.8, 1e-3),
               "captured volume round-trips");
        check (q.tracks[3].sound.audio == nullptr,
               "audio buffers are NOT serialised (rebuilt on load)");
    }

    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
                 failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
